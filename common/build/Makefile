include ../../build/Makefile


$(call build-static-library,\
   beegfs-common,\
   $(shell find ../source -iname '*.cpp'),\
   dl nl3-route,\
   ../source)

$(call build-shared-library,\
   beegfs_ib,\
   $(shell find ../ib_lib -iname '*.cpp'),\
   rdma,\
   ../source ../ib_lib)

# SuperTCP transport plugin: opt-in via BEEGFS_USE_SUPERTCP=1.
# Requires:
#   MTCP_DIR=<path-to-SuperTCP-repo>   (must contain include/ and lib/libmtcp.a)
#   libdpdk available via pkg-config   (DPDK installed system-wide)
#   libnuma, libgmp, libpcap           (system packages)
#
# Optional: MTCP_DPDK_ABI=<N>
#   Pin the plugin's NEEDED entries to specific librte_*.so.<N> instead of
#   following pkg-config / the librte_*.so symlinks. Required when the
#   system has multiple DPDK ABIs installed and the .so symlinks point at
#   an older version than what libmtcp.a was compiled against (typical
#   symptom: dlopen fails at runtime with `undefined symbol: rte_eth_fp_ops`).
#   Auto-detect via: $(MTCP_DIR)/scripts/detect-dpdk-abi.sh
ifeq ($(BEEGFS_USE_SUPERTCP),1)
ifneq ($(strip $(MAKECMDGOALS)),clean)
ifndef MTCP_DIR
$(error BEEGFS_USE_SUPERTCP=1 requires MTCP_DIR=/path/to/SuperTCP-repo (must contain include/ and lib/libmtcp.a))
endif
endif

# When MTCP_DPDK_ABI is set, replace the version-less -lrte_X form (which
# follows the librte_X.so symlink) with -l:librte_X.so.<abi> so the resulting
# .so has versioned NEEDED entries. We list the libs that libmtcp.a actually
# undefined-references (the rest are pulled in transitively at runtime by
# DPDK's own PMD loading mechanism, which goes to pmds-<abi>.0/).
ifneq ($(MTCP_DPDK_ABI),)
SUPERTCP_DPDK_LIBS := \
   -l:librte_ethdev.so.$(MTCP_DPDK_ABI) \
   -l:librte_eal.so.$(MTCP_DPDK_ABI) \
   -l:librte_mbuf.so.$(MTCP_DPDK_ABI) \
   -l:librte_mempool.so.$(MTCP_DPDK_ABI) \
   -l:librte_ring.so.$(MTCP_DPDK_ABI) \
   -l:librte_timer.so.$(MTCP_DPDK_ABI) \
   -l:librte_telemetry.so.$(MTCP_DPDK_ABI) \
   -l:librte_kvargs.so.$(MTCP_DPDK_ABI) \
   -l:librte_net.so.$(MTCP_DPDK_ABI) \
   -l:librte_meter.so.$(MTCP_DPDK_ABI) \
   -l:librte_hash.so.$(MTCP_DPDK_ABI) \
   -l:librte_pci.so.$(MTCP_DPDK_ABI) \
   -l:librte_bus_pci.so.$(MTCP_DPDK_ABI) \
   -lbsd
else
SUPERTCP_DPDK_LIBS := $(shell pkg-config --libs libdpdk 2>/dev/null)
endif

$(call define-dep-lib, supertcp,\
   -I$(MTCP_DIR)/include,\
   $(MTCP_DIR)/lib/libmtcp.a,\
   -L$(MTCP_DIR)/lib -lmtcp $(SUPERTCP_DPDK_LIBS) -lnuma -lgmp -lpcap -lpthread -ldl)

$(call build-shared-library,\
   beegfs_supertcp,\
   $(shell find ../supertcp_lib -iname '*.cpp'),\
   supertcp,\
   ../source ../supertcp_lib)
endif


$(call build-test,\
   test-runner,\
   $(shell find ../tests -name '*.cpp'),\
   common,\
   ../tests)
