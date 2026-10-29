#!/bin/bash
# Charm++/reconverse build for the FoF comparison campaign, on UPSTREAM LCI.
#
# Supersedes build_charm_cmp.sh. That one used the local fork
# (lci-patched, branch allgather-bootstrap) because the 512-node points need
# an allgather OFI bootstrap: the old alltoall path publishes O(rank_n^2) PMI
# KVS entries and Cray PMI's fence overflows an int32 at 4096 ranks.
#
# uiuc-hpc/lci master now carries that upstream as
#   06748025 "Use allgather for OFI bootstrap (#194)"
# so the fork is no longer needed -- this build autofetches instead. The
# reconverse default tag (reconverse/CMakeLists.txt:43) was bumped to match,
# and is passed explicitly here so the log records it.
#
# suffix "cmp2" -> reconverse-linux-x86_64-amd-cmp2, leaving -amd-cmp (the
# build that produced every result through 2026-09-02) untouched for A/B.
set -o pipefail
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
LCI_TAG=067480250660927010d02375b5cc1bef470d3da4
cd /ccs/home/rrao/charm_reconverse || exit 1
CC=gcc CXX=g++ ./build charm++ reconverse-linux-x86_64 amd \
  --suffix=cmp2 \
  --with-production --enable-tracing --force -j32 \
  --with-fetch-reconverse-dir=$PWD/reconverse \
  --with-cmake-args="-DCMAKE_HIP_ARCHITECTURES=gfx90a -DRECONVERSE_AUTOFETCH_LCI2_TAG=$LCI_TAG"
rc=$?
echo "BUILD_CHARM_RC=$rc"
# Provenance gate: the fetched clone must actually be the allgather commit,
# and the built library must carry the upstream allgather log strings.
b=/ccs/home/rrao/charm_reconverse/reconverse-linux-x86_64-amd-cmp2
echo "=== fetched LCI clone ==="
git -C "$b/_deps/lci-src" log --oneline -1 2>&1
echo "=== liblci.so allgather strings ==="
strings "$b/lib/liblci.so" | grep -E "Bootstrap round .* allgather|LCI_BOOTSTRAP" || echo "MISSING"
exit $rc
