#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include <common/app/log/LogContext.h>
#include <common/net/sock/SuperTcpSocket.h>


class AbstractApp;


/**
 * Single-mtcp-lthread listener that does accept + epoll + recv +
 * NetMessageFactory dispatch all inline.
 *
 * Why an mtcp lthread (not a kernel pthread): our libmtcp.a is built with
 * ENABLE_UCTX=1 (Intel lthread mode). In that mode, mctx_t is reachable
 * only from threads spawned via mtcp_thread_create + driven by
 * mtcp_run_app. A plain pthread cannot get a usable mctx_t (its g_mctx
 * slot is never populated), so all SuperTcpSocket operations from outside
 * the lthread would fail.
 *
 * Lifecycle:
 *   - Plugin's listener_start_cb allocates SuperTcpListenerThread::Args,
 *     calls mtcp_thread_create(SuperTcpListenerThread::lthreadEntry,
 *     args, lcore), then spawns a pthread that calls mtcp_run_app()
 *     (which blocks until all lthreads exit).
 *   - lthreadEntry runs on the mtcp scheduler with a valid mctx via
 *     mtcp_create_context. It creates the SuperTcpSocket listener and
 *     loops on mtcp_epoll_wait.
 *   - listener_stop_and_join_cb flips Args::shouldStop; the lthread
 *     observes it on the next epoll_wait, exits; mtcp_run_app returns;
 *     the runner pthread is joined.
 */
class SuperTcpListenerThread
{
   public:
      struct ConnState
      {
         std::unique_ptr<SuperTcpSocket> sock;
         std::vector<char> recvBuf;
         std::vector<char> sendBuf;
      };

      struct Args
      {
         AbstractApp*       app;
         uint16_t           port;
         std::atomic<bool>  shouldStop{false};
         // Set by lthreadEntry after setupListener succeeds; read by
         // listener_start_cb to gate logging.
         std::atomic<bool>  listening{false};
      };

      // mtcp lthread entry point. Signature is void(void*) to match
      // mtcp_app_func_t. Owns its instance for the duration of the run.
      static void lthreadEntry(void* argsOpaque);

   private:
      explicit SuperTcpListenerThread(Args* args);
      ~SuperTcpListenerThread();

      Args*           args;
      LogContext      log;

      SuperTcpSocket* listenSock = nullptr;
      int             mtcpEpollFD = -1;
      std::unordered_map<int, ConnState> conns;

      bool setupListener();
      void runOnce();
      void onListenerReadable();
      void onAcceptedReadable(int sockid, ConnState& state);
      void closeConn(int sockid);
};
