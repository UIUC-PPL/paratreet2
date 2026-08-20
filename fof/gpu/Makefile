# Device side of FoF phase 1 (design/phase1-gpu.md).
#
# Built by hipcc/nvcc_wrapper, NOT by charmc: no Charm header is on this
# include path and no Charm library is linked, so a failure here is a
# Kokkos/HIP failure and nothing else (the tests/treecache discipline).
# The resulting object is handed to charmc at the application link.
#
#   make                 # libfofdevice.a + the standalone gate
#   make test            # run the standalone gate (needs a GPU node)
#   make KOKKOS_DIR=... ARCH=...
#
# NVIDIA: make HIPCC=$(KOKKOS_DIR)/bin/nvcc_wrapper ARCH_FLAG="-arch=sm_80"

# Must match src/Makefile.common's default (relay16 defect 1: both used to
# name one user's home).
KOKKOS_DIR ?= $(HOME)/kokkos
ROCM_DIR   ?= /opt/rocm-6.2.4
HIPCC      ?= $(ROCM_DIR)/bin/hipcc
ARCH       ?= gfx90a
ARCH_FLAG  ?= --offload-arch=$(ARCH)

# -ffp-contract=off: hipcc defaults to fused multiply-add, the CPU walk
# does not, and a fused vs unfused dx*dx+dy*dy+dz*dz can flip a particle
# pair sitting exactly on the linking length. The CPU arm must be built
# with the same flag for the A/B to mean anything
# (design/phase1-gpu.md section 6.2).
CXXFLAGS = -std=c++17 -O3 -ffp-contract=off $(ARCH_FLAG) \
           -I$(KOKKOS_DIR)/include -fPIC

KOKKOS_LIBS = -L$(KOKKOS_DIR)/lib64 -lkokkoscontainers -lkokkosalgorithms \
              -lkokkoscore -ldl

all: libfofdevice.a fof-device-test

FoFDevice.o: FoFDevice.cpp FoFDevice.h
	$(HIPCC) $(CXXFLAGS) -c $< -o $@

libfofdevice.a: FoFDevice.o
	ar cr $@ $^
	ranlib $@

# The gate links with hipcc (it is a plain program); the APPLICATION
# links libfofdevice.a with charmc plus $(KOKKOS_LIBS).
# NOTE: hipcc puts -x hip in front of every positional argument, so an
# archive named on the command line is compiled as HIP SOURCE ("source
# file is not valid UTF-8"). Archives must arrive through -L/-l.
fof-device-test: standalone.cpp libfofdevice.a
	$(HIPCC) $(CXXFLAGS) -o $@ standalone.cpp -L. -lfofdevice $(KOKKOS_LIBS)

test: fof-device-test
	./fof-device-test 1000000

clean:
	rm -f *.o *.a fof-device-test

.PHONY: all test clean
