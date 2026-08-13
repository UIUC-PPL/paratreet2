# Stage-0 integration gate (design/phase1-gpu.md section 8).
#
# The build shape this proves is the one the FoF application will use:
#   - the Charm TU is compiled by CHARMC and sees NO Kokkos header;
#   - the device TU is compiled by hipcc and sees NO Charm header;
#   - charmc does the final link, pulling in libfofdevice.a plus Kokkos
#     and the HIP runtime.
#
# CHARM_HOME must be the HIP-ENABLED build: the top-level
# charm_reconverse/include symlink points at reconverse-linux-x86_64,
# which has no hapi.h and no libhybridapi.a.

BASE_PATH := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CHARM_HOME ?= $(HOME)/charm_reconverse/reconverse-linux-x86_64-amd
KOKKOS_DIR ?= /ccs/home/rrao/kokkos
ROCM_DIR   ?= /opt/rocm-6.2.4
GPU_PATH    = $(BASE_PATH)/fof/gpu

# -D__HIP_PLATFORM_AMD__: the HIP headers pick their platform from this
# macro. hipcc defines it implicitly; g++ (what charmc drives) does not,
# so without it hip_runtime_api.h declares no hipStream_t and every
# hapi* prototype that names one silently fails to declare.
CHARMC = $(CHARM_HOME)/bin/charmc -g -O3 -c++-option -std=gnu++17 \
         -D__HIP_PLATFORM_AMD__

KOKKOS_LIBS = -L$(KOKKOS_DIR)/lib64 -lkokkoscontainers -lkokkosalgorithms \
              -lkokkoscore -ldl

# NO -lamdhip64 and NO rpath into $(ROCM_DIR) here, deliberately. TWO
# ROCm installs are in play on this machine: charm's HIP support was
# built against /opt/rocm-default (-> 6.4.2, and charmc puts
# -L/opt/rocm-default/lib on every link line), while Kokkos was built
# with hipcc from 6.2.4. Naming 6.2.4's lib dir as well makes ld resolve
# 6.4.2's libamdhip64 against 6.2.4's libhsa-runtime64 and the link fails
# on a versioned symbol ("undefined reference to
# hsa_amd_enable_logging@ROCR_1"). Exactly one HIP runtime must be in the
# link: charm's. The 6.2.4-compiled Kokkos objects are code-object
# compatible with the 6.4.2 runtime, and the standalone gate
# (fof/gpu, pure 6.2.4) covers the other direction.
# The real fix is to build Kokkos and charm against the same ROCm; until
# then, do not add ROCm paths here.

# charmc's link line names liblci.so but not its own dependency liblct.so,
# so ld cannot resolve the transitive DT_NEEDED without being told where
# to look (the "undefined reference to LCT_*" failure). -rpath-link fixes
# the link; -rpath also frees the RUN from needing LD_LIBRARY_PATH.
LCI_LIB_DIR ?= $(HOME)/charm_reconverse/lci/lib64
# libreconverse.so lives in the charm build's own lib dir and is likewise
# not on any default search path at RUN time. Baking both into RPATH keeps
# the binary runnable from a batch script with no LD_LIBRARY_PATH setup.
RPATH_LINK = -L$(LCI_LIB_DIR) -Wl,-rpath-link,$(LCI_LIB_DIR) \
             -Wl,-rpath,$(LCI_LIB_DIR) -Wl,-rpath,$(CHARM_HOME)/lib

# --allow-shlib-undefined: charmc hardcodes -L/opt/rocm-default/lib
# (= 6.4.2, whatever ROCm charm's HIP support was configured against),
# while the run environment and Kokkos are ROCm 6.2.4. With 6.2.4 on the
# search path ld resolves 6.4.2's libamdhip64 against 6.2.4's
# libhsa-runtime64 and fails on a versioned symbol
# ("hsa_amd_enable_logging@ROCR_1") that nothing here calls. Telling ld
# not to chase a shared library's own dependencies leaves the DT_NEEDED
# soname to be satisfied at RUN time, where LD_LIBRARY_PATH puts 6.2.4
# first and the whole process is then consistently 6.2.4.
# PROPER FIX (not ours to make here): rebuild the HIP charm with
# ROCM_PATH=/opt/rocm-6.2.4 so charmc stops naming rocm-default at all.
ALLOW_UNDEF = -Wl,--allow-shlib-undefined

all: kokkos-spike

spike.decl.h spike.def.h: spike.ci
	$(CHARMC) spike.ci

$(GPU_PATH)/libfofdevice.a:
	$(MAKE) -C $(GPU_PATH) libfofdevice.a

spike.o: spike.C spike.decl.h $(GPU_PATH)/FoFDevice.h
	$(CHARMC) -c spike.C

kokkos-spike: spike.o $(GPU_PATH)/libfofdevice.a
	$(CHARMC) -language charm++ -o $@ spike.o \
	    -L$(GPU_PATH) -lfofdevice $(KOKKOS_LIBS) $(RPATH_LINK) $(ALLOW_UNDEF)

clean:
	rm -f *.decl.h *.def.h *.o kokkos-spike charmrun

.PHONY: all clean
