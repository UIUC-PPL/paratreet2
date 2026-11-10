#!/bin/bash
# CPU-only FoF3 build on the Mac: the steps of fof/gpu/rebuild_deps.sh
# without the Frontier environment, the HIP define, or the htram rebuild
# (the existing ~/htram/libhtram_group_unionfind.a is used as is, so the
# htram tree is not touched).
set -e
CHARM=${CHARM:-$HOME/charm_reconverse}
PT=$HOME/paratreet2
UF=$HOME/unionfind
JOBS=${JOBS:-8}
SLIM_PARTICLE=1
SIB_CHARMC="$CHARM/bin/charmc -DUNIONFIND"

echo "### charm = $CHARM"; "$CHARM/bin/charmc" -v 2>&1 | head -3 || true

echo "### 1/5 prefixLib"
make -C "$UF/prefixLib" clean >/dev/null 2>&1 || true
make -C "$UF/prefixLib" CHARM_DIR="$CHARM" CHARMC="$SIB_CHARMC" -j"$JOBS"

echo "### 2/5 unionfind"
make -C "$UF" clean >/dev/null 2>&1 || true
make -C "$UF" CHARM_DIR="$CHARM" CHARMC="$SIB_CHARMC" AGGREGATION= PROFILE= -j"$JOBS"

echo "### 3/5 paratreet src"
make -C "$PT/src" clean >/dev/null 2>&1 || true
make -C "$PT/src" CHARM_HOME="$CHARM" SLIM_PARTICLE="$SLIM_PARTICLE" -j"$JOBS"

echo "### 4/5 fof module"
make -C "$PT/fof" clean >/dev/null 2>&1 || true
make -C "$PT/fof" CHARM_HOME="$CHARM" GPU=0 SLIM_PARTICLE="$SLIM_PARTICLE" -j"$JOBS"

echo "### 5/5 examples/fof3"
make -C "$PT/examples/fof3" clean >/dev/null 2>&1 || true
make -C "$PT/examples/fof3" CHARM_HOME="$CHARM" GPU=0 PROJECTIONS=0 SLIM_PARTICLE="$SLIM_PARTICLE" -j"$JOBS"

echo "### done"
ls -la "$PT/examples/fof3/FoF3"
