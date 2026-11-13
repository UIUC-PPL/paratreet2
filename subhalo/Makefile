# Sub-halo tools (design/subhalo-checks.md).
#
#   make                 # Frontier: unbind_rate and unbind, Kokkos/HIP by hipcc
#   make unbind_rate     # the synthetic checks and rates only (no HDF5)
#   make convert         # tipsy2hbt and hbtstat (HDF5 only)
#   make compare         # hbtref, unbindcmp, unbindspec, unbindcheck: the
#                        # comparison with HBT-HERONS (HDF5; OpenMP for speed)
#   make device          # libsubhalodevice.a: unbinding inside FoF3 (step 2),
#                        # linked by FoF3 built with GPU=1
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
HOSTOMP       ?= -fopenmp

CXXFLAGS    = -std=c++17 -O3 -ffp-contract=off $(ARCH_FLAG) -I$(KOKKOS_DIR)/include
KOKKOS_LIBS = -L$(KOKKOS_LIBDIR) -lkokkoscontainers -lkokkosalgorithms -lkokkoscore -ldl
HDF5_FLAGS  = -I$(HDF5_DIR)/include
HDF5_LIBS   = -L$(HDF5_DIR)/lib -Wl,-rpath,$(HDF5_DIR)/lib -lhdf5

all: unbind_rate unbind

unbind_rate: unbind_rate.cpp unbind_kernel.h unbind_groups.h
	$(KCXX) $(CXXFLAGS) -o $@ unbind_rate.cpp $(KOKKOS_LIBS)

unbind: unbind.cpp unbind_kernel.h unbind_groups.h
	$(KCXX) $(CXXFLAGS) $(HDF5_FLAGS) -o $@ unbind.cpp $(KOKKOS_LIBS) $(HDF5_LIBS)

# Step 2 (design/subhalo-checks.md, "Step 2: unbinding inside FoF3"): the
# device side FoF3 links through the firewall header SubhaloDevice.h, as it
# links fof/gpu/libfofdevice.a. -fPIC as there.
device: libsubhalodevice.a

SubhaloDevice.o: SubhaloDevice.cpp SubhaloDevice.h unbind_kernel.h unbind_groups.h
	$(KCXX) $(CXXFLAGS) -fPIC -c $< -o $@

libsubhalodevice.a: SubhaloDevice.o
	ar cr $@ $^
	ranlib $@

convert: tipsy2hbt hbtstat

tipsy2hbt: tipsy2hbt.cpp
	$(HOSTCXX) -O2 -std=c++17 $(HDF5_FLAGS) -o $@ $< $(HDF5_LIBS)

hbtstat: hbtstat.cpp
	$(HOSTCXX) -O2 -std=c++17 $(HDF5_FLAGS) -o $@ $< $(HDF5_LIBS)

# The comparison with HBT-HERONS (design/subhalo-checks.md, "Exact reference
# for the unbinding kernel"). Plain C++ by the host compiler. unbindspec and
# unbindcheck use OpenMP: on the Mac pass HOSTCXX=g++-16 HDF5_DIR=/opt/homebrew
# (Apple's c++ has no -fopenmp; HOSTOMP= builds them serial).
compare: hbtref unbindcmp unbindspec unbindcheck

hbtref: hbtref.cpp
	$(HOSTCXX) -O2 -std=c++17 $(HDF5_FLAGS) -o $@ $< $(HDF5_LIBS)

unbindcmp: unbindcmp.cpp unbindrec.h
	$(HOSTCXX) -O2 -std=c++17 -o $@ $<

unbindspec: unbindspec.cpp unbindspec.h unbindrec.h
	$(HOSTCXX) -O3 -std=c++17 -ffp-contract=off $(HOSTOMP) $(HDF5_FLAGS) -o $@ $< $(HDF5_LIBS)

unbindcheck: unbindcheck.cpp unbindspec.h unbindrec.h
	$(HOSTCXX) -O3 -std=c++17 -ffp-contract=off $(HOSTOMP) $(HDF5_FLAGS) -o $@ $< $(HDF5_LIBS)

clean:
	rm -f unbind_rate unbind tipsy2hbt hbtstat hbtref unbindcmp unbindspec unbindcheck *.o *.a

.PHONY: all device convert compare clean
