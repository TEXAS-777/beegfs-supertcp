#include "SuperTcpListenerThread.h"

#include <common/app/AbstractApp.h>
#include <common/net/message/AbstractNetMessageFactory.h>
#include <common/net/message/NetMessage.h>
#include <common/net/sock/IPAddress.h>
#include <common/net/sock/SocketException.h>
#include <common/net/sock/SocketTimeoutException.h>
#include <common/net/sock/SocketDisconnectException.h>
#include <common/toolkit/StringTk.h>

#include <arpa/inet.h>

// We are an mtcp lthread, so unlock the UCTX-only API surface from mtcp_api.h.
#ifndef ENABLE_UCTX
#define ENABLE_UCTX
#endif
#include <mtcp_api.h>
#include <mtcp_epoll.h>


#define BEEGFS_SUPERTCP_BUF_SIZE         (4 * 1024 * 1024)
#define BEEGFS_SUPERTCP_EPOLL_HINT       (256)
#define BEEGFS_SUPERTCP_EPOLL_TIMEOUT_MS (1000)


SuperTcpListenerThread::SuperTcpListenerThread(Args* args_)
   : args(args_),
     log("SuperTcpAccept")
{
}

SuperTcpListenerThread::~SuperTcpListenerThread()
{
   conns.clear();
   if (listenSock)
      delete listenSock;
}

// Defined in SuperTcpSocketImpl.cpp — flips a thread_local flag so
// ensureThreadContext() takes the lthread path (no core_affinitize, just
// mtcp_create_context lookup of g_mctx).
extern "C" void supertcp_mark_lthread();

void SuperTcpListenerThread::lthreadEntry(void* argsOpaque)
{
   Args* args = static_cast<Args*>(argsOpaque);

   // Tell ensureThreadContext we're in lthread context for this thread.
   supertcp_mark_lthread();

   // Sanity-pin this lthread's mctx (also populates g_mctx slot for any
   // sub-component that calls mtcp_create_context lazily).
   if (mtcp_create_context(0) == nullptr)
   {
      fprintf(stderr, "SuperTcpListener: mtcp_create_context(0) returned NULL\n");
      return;
   }

   SuperTcpListenerThread t(args);
   try
   {
      if (!t.setupListener())
         return;
      args->listening = true;

      while (!args->shouldStop)
         t.runOnce();

      t.log.log(Log_DEBUG, "SuperTCP listener stopping (shouldStop set).");
   }
   catch (std::exception& e)
   {
      t.log.logErr(std::string("SuperTcpListener fatal: ") + e.what());
      if (args->app)
         args->app->handleComponentException(e);
   }

   args->listening = false;
}

bool SuperTcpListenerThread::setupListener()
{
   try
   {
      auto sock = SuperTcpSocket::create();
      SocketAddress sa(IPAddress(static_cast<in_addr_t>(htonl(INADDR_ANY))),
                       args->port);
      sock->bindToAddr(sa);
      sock->listen();
      listenSock = sock.release();
   }
   catch (SocketException& e)
   {
      log.logErr(std::string("SuperTCP listen socket setup failed: ") + e.what());
      return false;
   }

   mtcpEpollFD = mtcp_epoll_create(BEEGFS_SUPERTCP_EPOLL_HINT);
   if (mtcpEpollFD < 0)
   {
      log.logErr("mtcp_epoll_create failed");
      delete listenSock;
      listenSock = nullptr;
      return false;
   }

   struct mtcp_epoll_event ev = {};
   ev.events      = MTCP_EPOLLIN;
   ev.data.sockid = listenSock->getFD();
   if (mtcp_epoll_ctl(mtcpEpollFD, MTCP_EPOLL_CTL_ADD, listenSock->getFD(), &ev) < 0)
   {
      log.logErr("mtcp_epoll_ctl ADD listen failed");
      return false;
   }

   log.log(Log_NOTICE, std::string("Listening for SuperTCP connections: Port ")
                       + StringTk::intToStr(args->port));
   return true;
}

void SuperTcpListenerThread::runOnce()
{
   struct mtcp_epoll_event events[BEEGFS_SUPERTCP_EPOLL_HINT];

   int n = mtcp_epoll_wait(mtcpEpollFD, events, BEEGFS_SUPERTCP_EPOLL_HINT,
                           BEEGFS_SUPERTCP_EPOLL_TIMEOUT_MS);
   if (n < 0)
   {
      log.logErr("mtcp_epoll_wait error");
      return;
   }

   for (int i = 0; i < n; i++)
   {
      int sockid = events[i].data.sockid;

      if (sockid == listenSock->getFD())
      {
         onListenerReadable();
      }
      else
      {
         auto it = conns.find(sockid);
         if (it != conns.end())
            onAcceptedReadable(sockid, it->second);
         else
            log.log(Log_WARNING, "epoll event for unknown sockid; ignoring");
      }
   }
}

void SuperTcpListenerThread::onListenerReadable()
{
   try
   {
      while (true)
      {
         struct sockaddr_storage peer = {};
         socklen_t peerLen = sizeof(peer);

         Socket* accepted = listenSock->accept(&peer, &peerLen);
         if (!accepted)
            break;

         std::unique_ptr<SuperTcpSocket> stSock(static_cast<SuperTcpSocket*>(accepted));
         int fd = stSock->getFD();

         struct mtcp_epoll_event ev = {};
         ev.events      = MTCP_EPOLLIN;
         ev.data.sockid = fd;
         if (mtcp_epoll_ctl(mtcpEpollFD, MTCP_EPOLL_CTL_ADD, fd, &ev) < 0)
         {
            log.logErr("mtcp_epoll_ctl ADD accepted failed");
            continue;
         }

         ConnState state;
         state.sock = std::move(stSock);
         state.recvBuf.resize(BEEGFS_SUPERTCP_BUF_SIZE);
         state.sendBuf.resize(BEEGFS_SUPERTCP_BUF_SIZE);
         conns.emplace(fd, std::move(state));

         log.log(Log_DEBUG, std::string("Accepted SuperTCP connection [SockID: ")
                            + StringTk::intToStr(fd) + std::string("]"));
      }
   }
   catch (SocketException& e)
   {
      // EAGAIN-equivalent at no-more-pending; fall through.
   }
}

void SuperTcpListenerThread::onAcceptedReadable(int sockid, ConnState& state)
{
   constexpr int recvTimeoutMS = 5000;

   try
   {
      char* bufIn = state.recvBuf.data();
      const unsigned bufInLen = state.recvBuf.size();

      ssize_t numReceived =
         state.sock->recvExactT(bufIn, NETMSG_MIN_LENGTH, 0, recvTimeoutMS);

      unsigned msgLength =
         NetMessageHeader::extractMsgLengthFromBuf(bufIn, numReceived);
      if (msgLength > bufInLen)
      {
         log.log(Log_NOTICE,
                 std::string("Oversized message from ")
                 + state.sock->getPeername() + "; disconnecting");
         closeConn(sockid);
         return;
      }

      if (msgLength > static_cast<unsigned>(numReceived))
         state.sock->recvExactT(&bufIn[numReceived],
                                msgLength - numReceived, 0, recvTimeoutMS);

      auto factory = args->app->getNetMessageFactory();
      auto msg = factory->createFromRaw(bufIn, msgLength);

      if (msg->getMsgType() == NETMSGTYPE_Invalid)
      {
         log.log(Log_NOTICE,
                 std::string("Invalid message from ") + state.sock->getPeername()
                 + "; disconnecting");
         closeConn(sockid);
         return;
      }

      NetMessage::ResponseContext rctx(nullptr, state.sock.get(),
                                       state.sendBuf.data(), state.sendBuf.size(),
                                       nullptr);
      bool ok = msg->processIncoming(rctx);
      if (!ok)
      {
         log.log(Log_NOTICE,
                 std::string("processIncoming failed for ")
                 + state.sock->getPeername() + "; disconnecting");
         closeConn(sockid);
         return;
      }
   }
   catch (SocketTimeoutException& e)
   {
      log.log(Log_NOTICE,
              std::string("Connection timed out: ") + state.sock->getPeername());
      closeConn(sockid);
   }
   catch (SocketDisconnectException& e)
   {
      closeConn(sockid);
   }
   catch (SocketException& e)
   {
      log.log(Log_NOTICE,
              std::string("Connection error from ") + state.sock->getPeername()
              + ": " + e.what());
      closeConn(sockid);
   }
}

void SuperTcpListenerThread::closeConn(int sockid)
{
   mtcp_epoll_ctl(mtcpEpollFD, MTCP_EPOLL_CTL_DEL, sockid, nullptr);
   conns.erase(sockid);
}
