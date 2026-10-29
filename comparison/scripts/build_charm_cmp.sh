#!/bin/bash
# Fresh charm_reconverse build for the FoF comparison campaign.
# Built with the ALLGATHER-BOOTSTRAP LCI patch (lci-patched, branch
# allgather-bootstrap) because the 512-node points need it: upstream LCI
# publishes O(rank_n^2) PMI KVS entries and Cray PMI's fence overflows an
# int32 at 4096 ranks.
#   suffix "cmp" -> reconverse-linux-x86_64-amd-cmp, so the existing
#   -amd-lciag / -amd-lcw builds that produced the Aug-2026 reference
#   numbers are left untouched.
set -o pipefail
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
cd /ccs/home/rrao/charm_reconverse || exit 1
CC=gcc CXX=g++ ./build charm++ reconverse-linux-x86_64 amd \
  --suffix=cmp \
  --with-production --enable-tracing --force -j32 \
  --with-fetch-reconverse-dir=$PWD/reconverse \
  --with-cmake-args="-DCMAKE_HIP_ARCHITECTURES=gfx90a -DFETCHCONTENT_SOURCE_DIR_LCI=$PWD/lci-patched"
echo "BUILD_CHARM_RC=$?"
