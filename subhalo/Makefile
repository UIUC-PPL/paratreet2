# Sub-halo tools (design/subhalo-checks.md).
#
#   make                 # Frontier: unbind_rate and unbind, Kokkos/HIP by hipcc
#   make unbind_rate     # the synthetic checks and rates only (no HDF5)
#   make convert         # tipsy2hbt and hbtstat (HDF5 only)
#
# Any other machine: name a C++ compiler, a Kokkos install and HDF5, e.g.
# the Mac with Homebrew and a Kokkos built for OpenMP:
#   make KCXX=g++-16 ARCH_FLAG=-fopenmp KOKKOS_DIR=$HOME/kokkos-openmp \
#        KOKKOS_LIBDIR=$HOME/kokkos-openmp/lib HDF5_DIR=/opt/homebrew
#
# Like fof/gpu: built by hipcc, not charmc, with no Charm header on the
# include path. -ffp-contract=off keeps a fused multiply-add from changing a
# pair term between backends (fof/gpu/Makefile explains the same choice).

KOKKOS_DIR    ?= $(HOME)/kokkos
KOKKOS_LIBDIR ?= $(KOKKOS_DIR)/lib64
ROCM_DIR      ?= /opt/rocm-6.2.4
KCXX          ?= $(ROCM_DIR)/bin/hipcc
ARCH_FLAG     ?= --offload-arch=gfx90a
HDF5_DIR      ?= /opt/cray/pe/hdf5/1.14.3.9/gnu/12.3
HOSTCXX       ?= c++

CXXFLAGS    = -std=c++17 -O3 -ffp-contract=off $(ARCH_FLAG) -I$(KOKKOS_DIR)/include
KOKKOS_LIBS = -L$(KOKKOS_LIBDIR) -lkokkoscontainers -lkokkosalgorithms -lkokkoscore -ldl
HDF5_FLAGS  = -I$(HDF5_DIR)/include
HDF5_LIBS   = -L$(HDF5_DIR)/lib -Wl,-rpath,$(HDF5_DIR)/lib -lhdf5

all: unbind_rate unbind

unbind_rate: unbind_rate.cpp unbind_kernel.h unbind_groups.h
	$(KCXX) $(CXXFLAGS) -o $@ unbind_rate.cpp $(KOKKOS_LIBS)

unbind: unbind.cpp unbind_kernel.h unbind_groups.h
	$(KCXX) $(CXXFLAGS) $(HDF5_FLAGS) -o $@ unbind.cpp $(KOKKOS_LIBS) $(HDF5_LIBS)

convert: tipsy2hbt hbtstat

tipsy2hbt: tipsy2hbt.cpp
	$(HOSTCXX) -O2 -std=c++17 $(HDF5_FLAGS) -o $@ $< $(HDF5_LIBS)

hbtstat: hbtstat.cpp
	$(HOSTCXX) -O2 -std=c++17 $(HDF5_FLAGS) -o $@ $< $(HDF5_LIBS)

clean:
	rm -f unbind_rate unbind tipsy2hbt hbtstat *.o

.PHONY: all convert clean
