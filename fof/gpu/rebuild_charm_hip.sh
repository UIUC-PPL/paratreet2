#!/bin/bash
# Rebuild the HIP-enabled charm++ FoF3 links against (both arms: a GPU=1
# FoF3 is linked against this build even when FOF_GPU_PHASE1 is off).
#
# Recipe as of 2026-10-29: charm branch `recharm`; reconverse is the
# contrib/reconverse submodule and buildcmake uses it by default (the old
# standalone charm_reconverse/reconverse clone, and the
# --with-fetch-reconverse-dir flag this script used to pass, are gone). LCI is
# autofetched at reconverse's pinned default, which since 2026-10-20 already
# contains the allgather bootstrap, so >4096 ranks needs no tag override.
#
#   --with-production      Release, not the Debug build stage 0 ran on
#   --enable-tracing       Projections available; -tracemode at the APP link
#                          is what turns it on (phase1-gpu.md trap 16)
#   --enable-lbuserdata    same option set as the CPU build
#   gfx90a only            charm's default is a seven-arch list and compiles
#                          far longer
#
# rocm/6.2.4 must be loaded first: conv-mach-hip.sh picks $ROCM_PATH and only
# falls back to /opt/rocm-default when it is unset -- the two-ROCm link
# collision of phase1-gpu.md trap 3. The shared env file guarantees it.
set -e

CHARM_SRC=${CHARM_SRC:-$HOME/charm_reconverse}
PT=${PT:-$HOME/paratreet2}
JOBS=${JOBS:-32}

FOF3_BIN=/nonexistent   # no binary preflight: we are rebuilding what it links
FOF3_CHARM_HOME=$CHARM_SRC/reconverse-linux-x86_64-amd
mkdir -p "$FOF3_CHARM_HOME/lib"   # the env file insists the dir exists
source "$PT/examples/fof3/scripts/frontier_env.sh"

cd "$CHARM_SRC"
echo "### charm $(git rev-parse --short HEAD) ($(git rev-parse --abbrev-ref HEAD))," \
     "reconverse $(git -C contrib/reconverse rev-parse --short HEAD)"

# The build repoints the top-level lib/include symlinks at the newest build.
# NAMD's env puts charm_reconverse/lib on LD_LIBRARY_PATH and its binaries
# load libreconverse/liblci from the CPU build, so put them back afterwards.
prev_lib=$(readlink lib || true)
prev_inc=$(readlink include || true)

CC=gcc CXX=g++ ./build charm++ reconverse-linux-x86_64 amd \
  --with-production --enable-tracing --enable-lbuserdata \
  --force -j"$JOBS" \
  --with-cmake-args="-DCMAKE_HIP_ARCHITECTURES=gfx90a"

[ -n "$prev_lib" ] && ln -sfn "$prev_lib" lib
[ -n "$prev_inc" ] && ln -sfn "$prev_inc" include
echo "### lib -> $(readlink lib)   include -> $(readlink include)"
echo "### LCI $(git -C reconverse-linux-x86_64-amd/_deps/lci-src rev-parse --short HEAD)"
