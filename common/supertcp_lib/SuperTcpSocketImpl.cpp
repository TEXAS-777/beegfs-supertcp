#include "SuperTcpSocketImpl.h"
#include "SuperTcpListenerThread.h"
#include <common/net/sock/SocketException.h>
#include <common/net/sock/SocketConnectException.h>
#include <common/net/sock/SocketTimeoutException.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>

// Default connect timeout (matches StandardSocket's 5s).
#define SUPERTCP_CONNECT_TIMEOUT_MS  5000


// ---------------------------------------------------------------------------
// Process-wide mTCP lifecycle. mtcp_init must be called exactly once per
// process before any other mtcp_* call from any thread.
// ---------------------------------------------------------------------------
namespace {

const char* DEFAULT_SUPERTCP_CONFIG = "/etc/beegfs/supertcp.conf";

std::once_flag g_initFlag;
bool           g_initOk      = false;
int            g_numCores    = 1;
std::atomic<int> g_coreCursor{0};

void initRuntimeOnce()
{
   const char* cfg = std::getenv("BEEGFS_SUPERTCP_CONFIG");
   if (!cfg || !*cfg)
      cfg = DEFAULT_SUPERTCP_CONFIG;

   if (mtcp_init(cfg) != 0)
      return; // g_initOk stays false → runtime_available() == false → caller falls back to TCP

   struct mtcp_conf conf = {};
   if (mtcp_getconf(&conf) == 0 && conf.num_cores > 0)
      g_numCores = conf.num_cores;

   g_initOk = true;
}

// Per-thread mtcp context. mtcp_create_context expects the calling thread to
// already be pinned via mtcp_core_affinitize.
thread_local bool   t_ctxInited = false;
thread_local int    t_coreId    = -1;

void ensureThreadContext()
{
   if (t_ctxInited)
      return;

   if (!g_initOk)
      throw SocketException("SuperTcpSocket: mtcp_init failed earlier; runtime unavailable");

   t_coreId = g_coreCursor.fetch_add(1, std::memory_order_relaxed) % g_numCores;

   // mtcp_init binds the calling (main) thread to its master lcore via
   // rte_eal_init; threads spawned afterwards inherit that mask. Reset to
   // ALL configured mtcp cores before mtcp_core_affinitize so the listener
   // thread can actually pick a different core than the master.
   {
      cpu_set_t allCores;
      CPU_ZERO(&allCores);
      for (int c = 0; c < g_numCores; c++)
         CPU_SET(c, &allCores);
      // pthread_setaffinity_np is a no-op for cores already in the mask;
      // expanding requires the calling user to have permission (no cgroup
      // restriction), which is the normal case for daemons run via systemd.
      pthread_setaffinity_np(pthread_self(), sizeof(allCores), &allCores);
   }

   if (mtcp_core_affinitize(t_coreId) != 0)
      throw SocketException("SuperTcpSocket: mtcp_core_affinitize failed for core "
                            + std::to_string(t_coreId)
                            + ": errno=" + std::to_string(errno)
                            + " (" + std::strerror(errno) + ")");

   if (mtcp_create_context(t_coreId) == nullptr)
      throw SocketException("SuperTcpSocket: mtcp_create_context failed for core "
                            + std::to_string(t_coreId));

   t_ctxInited = true;
}

[[noreturn]] void throwSysErr(const char* what)
{
   throw SocketException(std::string("SuperTcpSocket: ") + what + ": "
                         + std::strerror(errno));
}

// Wait for `sock` to become writable (i.e., connect() completes) within
// timeoutMS via a one-shot mtcp_epoll. Throws SocketConnectException on
// timeout / error, returns normally on success. The epoll fd is created
// fresh and then leaked (mtcp doesn't expose epoll_close); per-thread mctx
// teardown will reclaim it.
void waitForConnectWritable(int sock, const std::string& peername, int timeoutMS)
{
   int ep = mtcp_epoll_create(1);
   if (ep < 0)
      throw SocketConnectException(
         std::string("SuperTcpSocket: mtcp_epoll_create during connect to ")
         + peername + ": " + std::strerror(errno));

   struct mtcp_epoll_event ev = {};
   ev.events      = MTCP_EPOLLOUT | MTCP_EPOLLERR | MTCP_EPOLLHUP;
   ev.data.sockid = sock;

   if (mtcp_epoll_ctl(ep, MTCP_EPOLL_CTL_ADD, sock, &ev) < 0)
      throw SocketConnectException(
         std::string("SuperTcpSocket: mtcp_epoll_ctl during connect to ")
         + peername + ": " + std::strerror(errno));

   struct mtcp_epoll_event got[1];
   int n = mtcp_epoll_wait(ep, got, 1, timeoutMS);

   if (n < 0)
      throw SocketConnectException(
         std::string("SuperTcpSocket: mtcp_epoll_wait during connect to ")
         + peername + ": " + std::strerror(errno));

   if (n == 0)
      throw SocketConnectException(
         std::string("SuperTcpSocket: timeout connecting to ") + peername);

   // Mirrors StandardSocket: error events take precedence over POLLOUT
   // because both can fire together when remote refuses.
   if (got[0].events & (MTCP_EPOLLERR | MTCP_EPOLLHUP))
      throw SocketConnectException(
         std::string("SuperTcpSocket: connect refused / hung up: ") + peername);
}

bool resolveHostV4(const char* host, uint16_t port, struct sockaddr_in* outAddr)
{
   struct addrinfo hints = {};
   hints.ai_family   = AF_INET;
   hints.ai_socktype = SOCK_STREAM;

   struct addrinfo* res = nullptr;
   if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res)
      return false;

   std::memset(outAddr, 0, sizeof(*outAddr));
   outAddr->sin_family = AF_INET;
   outAddr->sin_port   = htons(port);
   outAddr->sin_addr   =
      reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr;

   freeaddrinfo(res);
   return true;
}

// ---------------------------------------------------------------------------
// ImplCallbacks export — matches abstract dlopen contract.
// ---------------------------------------------------------------------------
bool supertcp_runtime_available_cb()
{
   std::call_once(g_initFlag, initRuntimeOnce);
   return g_initOk;
}

void supertcp_runtime_init_once_cb()
{
   std::call_once(g_initFlag, initRuntimeOnce);
}

SuperTcpSocket* new_supertcp_socket()
{
   return new SuperTcpSocketImpl();
}

void* listener_start_cb(void* app_opaque, uint16_t port)
{
   AbstractApp* app = static_cast<AbstractApp*>(app_opaque);
   auto* t = new SuperTcpListenerThread(app, port);
   t->start();
   return t;
}

void listener_stop_and_join_cb(void* handle)
{
   if (!handle)
      return;
   auto* t = static_cast<SuperTcpListenerThread*>(handle);
   t->selfTerminate();
   t->join();
   delete t;
}

} // namespace

extern "C" SuperTcpSocket::ImplCallbacks beegfs_supertcp_socket_impl = {
   supertcp_runtime_available_cb,
   supertcp_runtime_init_once_cb,
   new_supertcp_socket,
   listener_start_cb,
   listener_stop_and_join_cb,
};


// ---------------------------------------------------------------------------
// SuperTcpSocketImpl
// ---------------------------------------------------------------------------

SuperTcpSocketImpl::SuperTcpSocketImpl()
{
   this->sockType = NICADDRTYPE_SUPERTCP;

   std::call_once(g_initFlag, initRuntimeOnce);
   if (!g_initOk)
      throw SocketException("SuperTcpSocket: mtcp_init failed; runtime unavailable");

   ensureThreadContext();

   mtcpSock = mtcp_socket(AF_INET, SOCK_STREAM, 0);
   if (mtcpSock < 0)
      throwSysErr("mtcp_socket");

   // BeeGFS callers expect non-blocking send/recv with their own retry loops.
   // recv()/send() on a listening socket return errors anyway; this is for
   // connected sockets but harmless to set early.
   if (mtcp_setsock_nonblock(mtcpSock) < 0)
      throwSysErr("mtcp_setsock_nonblock");
}

SuperTcpSocketImpl::SuperTcpSocketImpl(int mtcpSock_,
                                       const IPAddress& peerIP_,
                                       std::string peername_)
{
   this->sockType  = NICADDRTYPE_SUPERTCP;
   this->mtcpSock  = mtcpSock_;
   this->peerIP    = peerIP_;
   this->peername  = std::move(peername_);

   if (mtcp_setsock_nonblock(mtcpSock) < 0)
      throwSysErr("mtcp_setsock_nonblock(accepted)");
}

SuperTcpSocketImpl::~SuperTcpSocketImpl()
{
   if (recvEp >= 0)
   {
      // mTCP's epoll fd is closed implicitly when context dies; nothing to do.
      recvEp = -1;
   }
   if (mtcpSock >= 0)
   {
      mtcp_close(mtcpSock);
      mtcpSock = -1;
   }
}

int SuperTcpSocketImpl::ensureRecvEpoll()
{
   if (recvEp >= 0)
      return recvEp;

   recvEp = mtcp_epoll_create(1);
   if (recvEp < 0)
      return -1;

   struct mtcp_epoll_event ev = {};
   ev.events      = MTCP_EPOLLIN;
   ev.data.sockid = mtcpSock;

   if (mtcp_epoll_ctl(recvEp, MTCP_EPOLL_CTL_ADD, mtcpSock, &ev) < 0)
   {
      // best-effort cleanup (no mtcp_epoll_close in API; leak is bounded by
      // thread/context lifetime)
      recvEp = -1;
      return -1;
   }
   return recvEp;
}

void SuperTcpSocketImpl::connect(const char* hostname, uint16_t port)
{
   ensureThreadContext();

   struct sockaddr_in saddr = {};
   if (!resolveHostV4(hostname, port, &saddr))
      throw SocketConnectException(
         std::string("SuperTcpSocket: DNS resolution failed for ") + hostname);

   peerIP   = IPAddress(saddr.sin_addr.s_addr);
   peername = std::string(hostname) + ":" + std::to_string(port);

   if (mtcp_connect(mtcpSock,
                    reinterpret_cast<struct sockaddr*>(&saddr),
                    sizeof(saddr)) < 0)
   {
      // Socket was set non-blocking in the constructor, so connect typically
      // returns EINPROGRESS — wait for writability via mtcp_epoll.
      if (errno != EINPROGRESS)
         throw SocketConnectException(
            std::string("SuperTcpSocket: mtcp_connect to ") + peername
            + ": " + std::strerror(errno));

      waitForConnectWritable(mtcpSock, peername, SUPERTCP_CONNECT_TIMEOUT_MS);
   }
}

void SuperTcpSocketImpl::connect(const SocketAddress& servAddr)
{
   ensureThreadContext();

   struct sockaddr_in saddr = servAddr.toIPv4Sockaddr();

   peerIP   = servAddr.addr;
   peername = servAddr.toString();

   if (mtcp_connect(mtcpSock,
                    reinterpret_cast<struct sockaddr*>(&saddr),
                    sizeof(saddr)) < 0)
   {
      if (errno != EINPROGRESS)
         throw SocketConnectException(
            std::string("SuperTcpSocket: mtcp_connect to ") + peername
            + ": " + std::strerror(errno));

      waitForConnectWritable(mtcpSock, peername, SUPERTCP_CONNECT_TIMEOUT_MS);
   }
}

void SuperTcpSocketImpl::bindToAddr(const SocketAddress& ipAddr)
{
   ensureThreadContext();

   struct sockaddr_in saddr = ipAddr.toIPv4Sockaddr();

   if (mtcp_bind(mtcpSock,
                 reinterpret_cast<struct sockaddr*>(&saddr),
                 sizeof(saddr)) < 0)
      throwSysErr("mtcp_bind");

   bindIP   = ipAddr.addr;
   bindPort = ipAddr.port;
}

void SuperTcpSocketImpl::listen()
{
   ensureThreadContext();

   // Backlog: BeeGFS exposes connBacklogTCP elsewhere; the abstract Socket
   // listen() takes no arg. Use a sane default; Phase-3 plumbs the config
   // value through.
   if (mtcp_listen(mtcpSock, 64) < 0)
      throwSysErr("mtcp_listen");

   isListener = true;
}

Socket* SuperTcpSocketImpl::accept(struct sockaddr_storage* addr, socklen_t* addrLen)
{
   ensureThreadContext();

   struct sockaddr_in peer = {};
   socklen_t          plen = sizeof(peer);

   int sub = mtcp_accept(mtcpSock,
                         reinterpret_cast<struct sockaddr*>(&peer),
                         &plen);
   if (sub < 0)
      throwSysErr("mtcp_accept");

   if (addr && addrLen && *addrLen >= sizeof(peer))
   {
      std::memcpy(addr, &peer, sizeof(peer));
      *addrLen = sizeof(peer);
   }

   IPAddress    peerAddr(peer.sin_addr.s_addr);
   std::string  peerName = peerAddr.toString() + ":"
                         + std::to_string(ntohs(peer.sin_port));

   return new SuperTcpSocketImpl(sub, peerAddr, std::move(peerName));
}

void SuperTcpSocketImpl::shutdown()
{
   if (mtcpSock >= 0)
   {
      mtcp_close(mtcpSock);
      mtcpSock = -1;
   }
}

void SuperTcpSocketImpl::shutdownAndRecvDisconnect(int timeoutMS)
{
   // mTCP exposes no half-close (no SHUT_WR). Best-effort: drain whatever
   // the peer sends until either the peer closes (RDHUP / read returns 0)
   // or timeoutMS elapses, then close. Used by callers that want to send
   // a "goodbye" message and let the peer ack-close cleanly before we
   // tear down the connection.
   if (mtcpSock < 0)
      return;

   int ep = ensureRecvEpoll();
   if (ep >= 0)
   {
      char buf[4096];
      const int deadlineMs = timeoutMS > 0 ? timeoutMS : 0;

      // Single epoll_wait then drain — a tighter loop with deadline
      // tracking is overkill for a "goodbye" path.
      struct mtcp_epoll_event ev[1];
      int n = mtcp_epoll_wait(ep, ev, 1, deadlineMs);
      if (n > 0)
      {
         while (true)
         {
            ssize_t r = mtcp_read(mtcpSock, buf, sizeof(buf));
            if (r <= 0) // peer closed (0) or would-block (-1 EAGAIN)
               break;
         }
      }
   }

   shutdown();
}

#ifdef BEEGFS_NVFS
ssize_t SuperTcpSocketImpl::read(const void*, size_t, uint32_t,
                                 const uint64_t, uint32_t)
{
   throw SocketException("SuperTcpSocket: NVFS RDMA read not supported on user-space TCP");
}
ssize_t SuperTcpSocketImpl::write(const void*, size_t, uint32_t,
                                  const uint64_t, uint32_t)
{
   throw SocketException("SuperTcpSocket: NVFS RDMA write not supported on user-space TCP");
}
#endif

ssize_t SuperTcpSocketImpl::send(const void* buf, size_t len, int /*flags*/)
{
   ensureThreadContext();

   ssize_t n = mtcp_write(mtcpSock,
                          reinterpret_cast<const char*>(buf),
                          len);
   if (n < 0)
      throwSysErr("mtcp_write");
   return n;
}

ssize_t SuperTcpSocketImpl::sendto(const void*, size_t, int, const SocketAddress*)
{
   throw SocketException("SuperTcpSocket: sendto (UDP) not supported — mTCP is stream-only");
}

ssize_t SuperTcpSocketImpl::recv(void* buf, size_t len, int /*flags*/)
{
   ensureThreadContext();

   ssize_t n = mtcp_read(mtcpSock,
                         reinterpret_cast<char*>(buf),
                         len);
   if (n < 0)
      throwSysErr("mtcp_read");
   return n;
}

ssize_t SuperTcpSocketImpl::recvT(void* buf, size_t len, int flags, int timeoutMS)
{
   ensureThreadContext();

   int ep = ensureRecvEpoll();
   if (ep < 0)
      throwSysErr("mtcp_epoll_create");

   struct mtcp_epoll_event evs[1];
   int ready = mtcp_epoll_wait(ep, evs, 1, timeoutMS);
   if (ready < 0)
      throwSysErr("mtcp_epoll_wait");

   if (ready == 0)
      throw SocketTimeoutException("SuperTcpSocket: recvT timeout");

   return recv(buf, len, flags);
}
