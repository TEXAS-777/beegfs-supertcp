#pragma once

#include <common/app/AbstractApp.h>
#include <common/app/log/LogContext.h>
#include <common/net/sock/SuperTcpSocket.h>
#include <common/threading/PThread.h>

#include <unordered_map>

/**
 * Single-threaded SuperTCP listener + data-path handler.
 *
 * Why a dedicated thread: mTCP's mctx_t is thread-local and per-CPU. A socket
 * created on one thread cannot be safely used on another. BeeGFS's normal
 * Worker pool would hand a socket between accept-thread → poll-thread →
 * worker-thread, which breaks mTCP's invariants. So we run accept + epoll +
 * read + message-dispatch all on this single thread, mirroring the
 * apps/example/epserver.c pattern.
 *
 * Lifecycle:
 *   - run(): mtcp_core_affinitize + mtcp_create_context, then bind + listen
 *     SuperTcpSocket on listenPort, then loop on mtcp_epoll_wait.
 *   - Listener event → mtcp_accept, register accepted in mtcp_epoll.
 *   - Accepted-socket event → recvExact(header) + recvExact(body) → invoke
 *     NetMessageFactory + msg->processIncoming(rctx) inline.
 *
 * Phase 4 trade-off: single-thread throughput ceiling. For multi-core
 * scaling, would need either (a) one listener thread per core with
 * mtcp_init_rss flow distribution, or (b) cross-thread socket handoff in
 * mtcp itself. Left as Phase 5+.
 */
class SuperTcpListenerThread : public PThread
{
   public:
      SuperTcpListenerThread(AbstractApp* app, unsigned short listenPort);
      virtual ~SuperTcpListenerThread();

      virtual void run() override;

   private:
      AbstractApp*    app;
      LogContext      log;
      unsigned short  listenPort;

      SuperTcpSocket* listenSock = nullptr;
      int             mtcpEpollFD = -1;

      // Per-connection receive buffers, keyed by mtcp socket id. We allocate
      // lazily on first data event for a given accepted socket.
      struct ConnState {
         std::unique_ptr<SuperTcpSocket> sock;
         std::vector<char> recvBuf;
         std::vector<char> sendBuf;
      };
      std::unordered_map<int, ConnState> conns;

      void runOnce();
      void initThreadContext();
      bool setupListener();
      void onListenerReadable();
      void onAcceptedReadable(int sockid, ConnState& state);
      void closeConn(int sockid);
};
