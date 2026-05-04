#pragma once

#include <common/net/sock/SuperTcpSocket.h>

/**
 * Concrete SuperTCP transport.
 *
 * Phase-1 scaffolding only: every method throws. mTCP / DPDK calls
 * (`mtcp_socket`, `mtcp_connect`, `mtcp_read`, `mtcp_write`,
 * `mtcp_epoll_*`) wire in here in subsequent phases. See
 * apps/beegfs/README.md and docs/local/BeeGFS.md for the porting plan.
 *
 * The dlopen contract requires this TU to export a C symbol named
 * `beegfs_supertcp_socket_impl` of type SuperTcpSocket::ImplCallbacks.
 */
class SuperTcpSocketImpl : public SuperTcpSocket
{
   public:
      SuperTcpSocketImpl();
      virtual ~SuperTcpSocketImpl() override;

      virtual void connect(const char* hostname, uint16_t port) override;
      virtual void connect(const SocketAddress& serv_addr) override;
      virtual void bindToAddr(const SocketAddress& ipAddr) override;
      virtual void listen() override;
      virtual Socket* accept(struct sockaddr_storage* addr, socklen_t* addrLen) override;
      virtual void shutdown() override;
      virtual void shutdownAndRecvDisconnect(int timeoutMS) override;

#ifdef BEEGFS_NVFS
      virtual ssize_t read(const void* buf, size_t len, uint32_t lkey,
         const uint64_t rbuf, uint32_t rkey) override;
      virtual ssize_t write(const void* buf, size_t len, uint32_t lkey,
         const uint64_t rbuf, uint32_t rkey) override;
#endif

      virtual ssize_t send(const void* buf, size_t len, int flags) override;
      virtual ssize_t sendto(const void* buf, size_t len, int flags,
         const SocketAddress* to) override;

      virtual ssize_t recv(void* buf, size_t len, int flags) override;
      virtual ssize_t recvT(void* buf, size_t len, int flags, int timeoutMS) override;

      virtual int getFD() const override { return fd; }

   private:
      int fd = -1; // mtcp socket descriptor (per-thread mctx scoped)
};

extern "C" {
   extern SuperTcpSocket::ImplCallbacks beegfs_supertcp_socket_impl;
}
