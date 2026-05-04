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

#include <mtcp_api.h>
#include <mtcp_epoll.h>


#define BEEGFS_SUPERTCP_BUF_SIZE     (4 * 1024 * 1024)
#define BEEGFS_SUPERTCP_EPOLL_HINT   (256)
#define BEEGFS_SUPERTCP_EPOLL_TIMEOUT_MS  (1000)


SuperTcpListenerThread::SuperTcpListenerThread(AbstractApp* app_, unsigned short port)
   : PThread("SuperTcpAccept"),
     app(app_),
     log("SuperTcpAccept"),
     listenPort(port)
{
}

SuperTcpListenerThread::~SuperTcpListenerThread()
{
   conns.clear();
   if (listenSock)
      delete listenSock;
}

void SuperTcpListenerThread::run()
{
   try
   {
      initThreadContext();

      if (!setupListener())
         return;

      while (!getSelfTerminate())
         runOnce();

      log.log(Log_DEBUG, "SuperTCP listener stopping.");
   }
   catch (std::exception& e)
   {
      log.logErr(std::string("SuperTcpListenerThread fatal: ") + e.what());
      app->handleComponentException(e);
   }
}

void SuperTcpListenerThread::initThreadContext()
{
   // First-touch construction of a SuperTcpSocket from this thread will
   // trigger ensureThreadContext() inside the plugin (mtcp_core_affinitize +
   // mtcp_create_context with a round-robin core). We rely on that path here
   // rather than calling mtcp_* directly, because the plugin owns the
   // affinitization policy.
}

bool SuperTcpListenerThread::setupListener()
{
   try
   {
      auto sock = SuperTcpSocket::create();
      // 0.0.0.0:listenPort
      SocketAddress sa(IPAddress(static_cast<in_addr_t>(htonl(INADDR_ANY))), listenPort);
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
                       + StringTk::intToStr(listenPort));
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

         Socket* accepted = listenSock->accept(
            reinterpret_cast<struct sockaddr_storage*>(&peer), &peerLen);
         if (!accepted)
            break;

         std::unique_ptr<SuperTcpSocket> stSock(static_cast<SuperTcpSocket*>(accepted));
         int fd = stSock->getFD();

         // Register accepted in our mtcp_epoll
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
      // EAGAIN-like when no more pending — fall through silently.
   }
}

void SuperTcpListenerThread::onAcceptedReadable(int sockid, ConnState& state)
{
   constexpr int recvTimeoutMS = 5000;

   try
   {
      char* bufIn = state.recvBuf.data();
      const unsigned bufInLen = state.recvBuf.size();

      // 1. Receive at least the message header.
      ssize_t numReceived =
         state.sock->recvExactT(bufIn, NETMSG_MIN_LENGTH, 0, recvTimeoutMS);

      // 2. Parse expected total length from the header.
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

      // 3. Receive the rest of the message.
      if (msgLength > static_cast<unsigned>(numReceived))
         state.sock->recvExactT(&bufIn[numReceived],
                                msgLength - numReceived, 0, recvTimeoutMS);

      // 4. Build NetMessage from raw bytes.
      auto factory = app->getNetMessageFactory();
      auto msg = factory->createFromRaw(bufIn, msgLength);

      if (msg->getMsgType() == NETMSGTYPE_Invalid)
      {
         log.log(Log_NOTICE,
                 std::string("Invalid message from ") + state.sock->getPeername()
                 + "; disconnecting");
         closeConn(sockid);
         return;
      }

      // 5. Process inline. The message's processIncoming() writes the response
      //    via state.sock->send(...) using the same thread-local mctx — safe
      //    because we are still on this listener thread.
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

      // 6. Re-arm: mtcp_epoll is level-triggered by default in our config,
      //    so the next data event will fire automatically. Nothing to do.
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
