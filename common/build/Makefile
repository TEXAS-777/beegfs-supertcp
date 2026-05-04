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
# During scaffolding the plugin has no external deps; once SuperTcpSocketImpl
# wires in mtcp / DPDK calls, add them to the deps list.
ifeq ($(BEEGFS_USE_SUPERTCP),1)
$(call build-shared-library,\
   beegfs_supertcp,\
   $(shell find ../supertcp_lib -iname '*.cpp'),\
   ,\
   ../source ../supertcp_lib)
endif


$(call build-test,\
   test-runner,\
   $(shell find ../tests -name '*.cpp'),\
   common,\
   ../tests)
