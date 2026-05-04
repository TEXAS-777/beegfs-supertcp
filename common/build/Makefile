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
ifeq ($(BEEGFS_USE_SUPERTCP),1)
ifneq ($(strip $(MAKECMDGOALS)),clean)
ifndef MTCP_DIR
$(error BEEGFS_USE_SUPERTCP=1 requires MTCP_DIR=/path/to/SuperTCP-repo (must contain include/ and lib/libmtcp.a))
endif
endif

$(call define-dep-lib, supertcp,\
   -I$(MTCP_DIR)/include,\
   $(MTCP_DIR)/lib/libmtcp.a,\
   -L$(MTCP_DIR)/lib -lmtcp $(shell pkg-config --libs libdpdk 2>/dev/null) -lnuma -lgmp -lpcap -lpthread -ldl)

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
