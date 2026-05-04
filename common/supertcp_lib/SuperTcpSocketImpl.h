#pragma once

#include <common/net/sock/SuperTcpSocket.h>

/**
 * Concrete SuperTCP transport — wires BeeGFS Socket ops to mTCP user-space
 * TCP (DPDK underneath). The daemons stay un-coupled from DPDK at link time:
 * libbeegfs_supertcp.so is the only TU that touches mtcp_api.h.
 *
 * Threading: mTCP requires each thread that uses sockets to hold a per-core
 * mctx_t obtained via mtcp_create_context(cpu) after mtcp_core_affinitize(cpu).
 * The plugin maintains a thread_local mctx that is lazy-created on first use.
 * Phase-2 strategy: round-robin core assignment from a global counter modulo
 * mtcp_conf::num_cores. This is correctness-only — see TODO in
 * SuperTcpSocketImpl.cpp for the NUMA / dedicated-core concerns.
 *
 * Lifecycle: mtcp_init() is called once per process via std::call_once on
 * first SuperTcpSocketImpl construction. Config path comes from the
 * BEEGFS_SUPERTCP_CONFIG env var (default /etc/beegfs/supertcp.conf).
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

      // Note: this is an mTCP socket id, NOT a kernel fd. Do not pass to
      // poll/epoll/select on the kernel side. Use the (Phase-3) kernel
      // event-bridge fd if multiplexing with kernel-side state is needed.
      virtual int getFD() const override { return mtcpSock; }

   private:
      // For accept(): wrap an already-connected mtcp socket id.
      SuperTcpSocketImpl(int mtcpSock, const IPAddress& peerIP, std::string peername);

      // Lazy-create a thread-local mtcp epoll fd for recvT timeouts. Returns
      // -1 if the underlying mtcp_epoll_create fails.
      int ensureRecvEpoll();

      int  mtcpSock = -1;
      int  recvEp   = -1;       // thread-local mtcp epoll, lazy
      bool isListener = false;
};

extern "C" {
   extern SuperTcpSocket::ImplCallbacks beegfs_supertcp_socket_impl;
}
