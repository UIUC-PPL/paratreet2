#!/bin/bash
# Rebuild the whole paratreet2 FoF stack (prefixLib, htram, unionfind,
# fof device lib, paratreet src, fof module, examples/fof3) against the
# freshly built charm reconverse-linux-x86_64-amd-cmp2, which carries the
# allgather OFI bootstrap (upstream LCI 06748025) needed above ~2048 ranks.
# GPU=1 because the reference numbers in design/fof3-bigscale-2026-08-23.md
# are all from the device phase-1 arm.
set -o pipefail
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
CHARM=/ccs/home/rrao/charm_reconverse/reconverse-linux-x86_64-amd-cmp2 \
  GPU=1 PROJECTIONS=0 JOBS=16 \
  /ccs/home/rrao/paratreet2/fof/gpu/rebuild_deps.sh
echo "BUILD_FOF3_RC=$?"
