#!/bin/bash
# Kokkos 4.6.01 for Frontier: HIP backend, gfx90a (MI250X), plus Serial.
# ArborX needs Kokkos >= 4.5 CONFIG-installed.
set -e
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
SRC=$CMP/kokkos-src
BLD=$CMP/kokkos-build
INS=$CMP/kokkos
rm -rf "$BLD"; mkdir -p "$BLD"; cd "$BLD"
cmake "$SRC" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INS" \
  -DCMAKE_CXX_COMPILER=hipcc \
  -DCMAKE_CXX_STANDARD=20 \
  -DKokkos_ENABLE_HIP=ON \
  -DKokkos_ARCH_AMD_GFX90A=ON \
  -DKokkos_ENABLE_SERIAL=ON \
  -DKokkos_ENABLE_OPENMP=OFF \
  -DKokkos_ENABLE_HIP_RELOCATABLE_DEVICE_CODE=OFF \
  -DBUILD_SHARED_LIBS=OFF
make -j32
make install
echo "KOKKOS_BUILD_RC=0"
