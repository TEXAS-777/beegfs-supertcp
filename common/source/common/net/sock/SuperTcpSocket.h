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
      };

      SuperTcpSocket() : PooledSocket() {}

      static bool isSuperTcpAvailable();

      static std::unique_ptr<SuperTcpSocket> create();

      static bool superTcpRuntimeAvailable();
      static void superTcpRuntimeInitOnce();
};
