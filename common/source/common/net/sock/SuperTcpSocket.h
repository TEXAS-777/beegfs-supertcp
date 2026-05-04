#pragma once

#include <common/Common.h>
#include "PooledSocket.h"

/**
 * Abstract base for the SuperTCP (DPDK / mTCP user-space TCP) transport.
 *
 * Mirrors the dlopen-plugin pattern used by RDMASocket: the concrete
 * implementation lives in libbeegfs_supertcp.so and is loaded at runtime
 * (so the main daemons do not need to link against DPDK / mTCP unless the
 * SuperTCP transport is actually requested at runtime).
 *
 * Plugin contract: the shared object must export a
 *    extern "C" SuperTcpSocket::ImplCallbacks beegfs_supertcp_socket_impl;
 * symbol with the function-pointer table populated.
 */
class SuperTcpSocket : public PooledSocket
{
   public:
      struct ImplCallbacks
      {
         bool (*supertcp_runtime_available)();
         void (*supertcp_runtime_init_once)();
         SuperTcpSocket* (*supertcp_socket_create)();

         // Phase 4: launch a single-thread mTCP listener that accepts
         // SuperTCP connections on `port`, reads BeeGFS messages, and
         // dispatches via the app's NetMessageFactory inline. Returns an
         // opaque handle; pass to stop_and_join.
         //
         // `app` is typed as void* in the C ABI to avoid pulling
         // AbstractApp into the dlopen contract — the plugin reinterprets
         // back to AbstractApp* internally.
         void* (*supertcp_listener_start)(void* app, uint16_t port);
         void  (*supertcp_listener_stop_and_join)(void* handle);
      };

      SuperTcpSocket() : PooledSocket() {}

      static bool isSuperTcpAvailable();

      static std::unique_ptr<SuperTcpSocket> create();

      static bool superTcpRuntimeAvailable();
      static void superTcpRuntimeInitOnce();

      // Phase 4 listener-thread API. AbstractApp* is opaque here; pass via
      // void* to avoid including AbstractApp.h in this abstract header.
      static void* startListener(void* app, uint16_t port);
      static void  stopAndJoinListener(void* handle);
};
