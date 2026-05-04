#include "SuperTcpSocketImpl.h"
#include <common/net/sock/SocketException.h>

namespace {

[[noreturn]] void notYetImplemented(const char* what)
{
   throw SocketException(std::string("SuperTcpSocket: not yet implemented: ") + what);
}

bool supertcp_runtime_available_stub()
{
   // Phase 1 scaffolding: the runtime is never claimed available. The plugin
   // loads (so isSuperTcpAvailable() returns true), but transport selection
   // logic should treat "available && !runtime_available" as "plugin present
   // but cannot be used yet" and fall back to TCP.
   return false;
}

void supertcp_runtime_init_once_stub()
{
   // No-op during scaffolding.
}

SuperTcpSocket* new_supertcp_socket()
{
   return new SuperTcpSocketImpl();
}

} // namespace

extern "C" SuperTcpSocket::ImplCallbacks beegfs_supertcp_socket_impl = {
   supertcp_runtime_available_stub,
   supertcp_runtime_init_once_stub,
   new_supertcp_socket,
};

SuperTcpSocketImpl::SuperTcpSocketImpl()
{
   this->sockType = NICADDRTYPE_SUPERTCP;
}

SuperTcpSocketImpl::~SuperTcpSocketImpl() = default;

void SuperTcpSocketImpl::connect(const char*, uint16_t)            { notYetImplemented("connect(host,port)"); }
void SuperTcpSocketImpl::connect(const SocketAddress&)             { notYetImplemented("connect(SocketAddress)"); }
void SuperTcpSocketImpl::bindToAddr(const SocketAddress&)          { notYetImplemented("bindToAddr"); }
void SuperTcpSocketImpl::listen()                                  { notYetImplemented("listen"); }
Socket* SuperTcpSocketImpl::accept(struct sockaddr_storage*,
                                   socklen_t*)                     { notYetImplemented("accept"); }
void SuperTcpSocketImpl::shutdown()                                { notYetImplemented("shutdown"); }
void SuperTcpSocketImpl::shutdownAndRecvDisconnect(int)            { notYetImplemented("shutdownAndRecvDisconnect"); }

#ifdef BEEGFS_NVFS
ssize_t SuperTcpSocketImpl::read(const void*, size_t, uint32_t,
                                 const uint64_t, uint32_t)         { notYetImplemented("read[NVFS]"); }
ssize_t SuperTcpSocketImpl::write(const void*, size_t, uint32_t,
                                  const uint64_t, uint32_t)        { notYetImplemented("write[NVFS]"); }
#endif

ssize_t SuperTcpSocketImpl::send(const void*, size_t, int)         { notYetImplemented("send"); }
ssize_t SuperTcpSocketImpl::sendto(const void*, size_t, int,
                                   const SocketAddress*)           { notYetImplemented("sendto"); }
ssize_t SuperTcpSocketImpl::recv(void*, size_t, int)               { notYetImplemented("recv"); }
ssize_t SuperTcpSocketImpl::recvT(void*, size_t, int, int)         { notYetImplemented("recvT"); }
