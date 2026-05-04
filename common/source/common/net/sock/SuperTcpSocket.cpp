#include "SuperTcpSocket.h"

#include <dlfcn.h>

namespace {
   SuperTcpSocket::ImplCallbacks* socket_impl;

   struct SuperTcpLibLoader {
      struct dlclose {
         void operator()(void* v) { ::dlclose(v); }
      };

      std::unique_ptr<void, dlclose> lib;

      SuperTcpLibLoader() {
         // RTLD_NOW resolves all symbols at load time so we fail fast if the
         // plugin's mtcp/DPDK dependencies are missing.
         lib.reset(dlopen("libbeegfs_supertcp.so", RTLD_NOW));
         if (!lib)
            return;

         socket_impl = (SuperTcpSocket::ImplCallbacks*)
            dlsym(lib.get(), "beegfs_supertcp_socket_impl");

         if (!socket_impl)
            lib.reset();
      }
   };

   SuperTcpLibLoader lib_loader;
}

bool SuperTcpSocket::isSuperTcpAvailable()
{
   return bool(lib_loader.lib);
}

bool SuperTcpSocket::superTcpRuntimeAvailable()
{
   return socket_impl && socket_impl->supertcp_runtime_available();
}

void SuperTcpSocket::superTcpRuntimeInitOnce()
{
   if (socket_impl)
      socket_impl->supertcp_runtime_init_once();
}

std::unique_ptr<SuperTcpSocket> SuperTcpSocket::create()
{
   if (!socket_impl)
      throw std::logic_error(
         "beegfs_supertcp_socket_create called with no SuperTCP support");

   return std::unique_ptr<SuperTcpSocket>(socket_impl->supertcp_socket_create());
}
