#!/bin/bash
# ArborX with MPI + Kokkos/HIP for the distributed DBSCAN (= FoF) comparison.
set -e
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
module load boost/1.86.0-mpi >/dev/null 2>&1 || module load boost/1.86.0 >/dev/null 2>&1
SRC=$CMP/arborx
BLD=$CMP/arborx-build
mkdir -p "$BLD"; cd "$BLD"
cmake "$SRC" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=hipcc \
  -DCMAKE_CXX_STANDARD=20 \
  -DKokkos_ROOT="$CMP/kokkos" \
  -DARBORX_ENABLE_MPI=ON \
  -DARBORX_ENABLE_BENCHMARKS=ON \
  -DARBORX_ENABLE_TESTS=OFF \
  -DARBORX_ENABLE_EXAMPLES=OFF \
  -DCMAKE_INSTALL_PREFIX="$CMP/arborx-install" \
  -DCOSMOIO_INCLUDE_DIR="$CMP/src"
make -j32
echo "ARBORX_BUILD_RC=0"
