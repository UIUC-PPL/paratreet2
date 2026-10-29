#!/bin/bash
# Rebuild the whole FoF stack against the HIP-enabled charm
# (design/phase1-gpu.md section 0: the GPU arm needs
# reconverse-linux-x86_64-amd, not the default build). Build order is the
# one the top-level README requires: siblings first, then src, then fof,
# then the application.
#
# AGGREGATION= / PROFILE= match design/frontier-labeling-ab.md's build
# line. AGGREGATION changes unionFindLib.h's class layout, so the library
# and every client must agree — leave both at their defaults or change
# both.
#
#   ./rebuild_deps.sh                      # CPU-only fof3 (no device code)
#   GPU=1 ./rebuild_deps.sh                # + fof/gpu, -DFOF_GPU, Kokkos at link
#   GPU=1 PROJECTIONS=1 ./rebuild_deps.sh  # + Projections traces
#
# PROJECTIONS is NOT inherited from the charm build. Charm's
# --enable-tracing makes tracing AVAILABLE; -tracemode projections at the
# APPLICATION link is what turns it on, and without it +traceroot is
# accepted in silence and the run produces no logs (design/phase1-gpu.md
# trap 16). Relinking without it is how a traced binary silently loses
# its tracing.
set -e

CHARM=${CHARM:-$HOME/charm_reconverse/reconverse-linux-x86_64-amd}
PT=${PT:-$HOME/paratreet2}
UF=${UF:-$HOME/unionfind}
HTRAM=${HTRAM:-$HOME/htram}
JOBS=${JOBS:-8}

# Modules and the charm library path come from the one shared env file.
# FOF3_BIN skips its binary preflight: the binary is what we are rebuilding,
# and the old one may be linked against a different charm.
FOF3_CHARM_HOME=$CHARM
FOF3_BIN=/nonexistent
source "$PT/examples/fof3/scripts/frontier_env.sh"

# ld needs liblct.so to resolve liblci.so's DT_NEEDED at link time
# (design/phase1-gpu.md trap 6). This used to point at the LCI FORK
# ($HOME/charm_reconverse/lci/lib64). That is wrong as of 2026-08-19: the
# charm build autofetches LCI via FetchContent and installs both liblci.so
# and liblct.so into $CHARM/lib, so the fork's copies were a DIFFERENT
# build of a DIFFERENT revision sitting ahead of them on the search path.
# Point at the charm build's own lib -- the one whose headers were compiled
# against -- so link and run agree.
export LD_LIBRARY_PATH=$CHARM/lib:$LD_LIBRARY_PATH

echo "### charm  = $CHARM"
echo "### GPU    = ${GPU:-0}"

# A HIP-enabled charm makes charm++.h include <hip/hip_runtime.h>, which
# hard-#errors without a platform macro. That hits EVERY client of this
# charm, so the siblings need it too — paratreet detects it for itself in
# src/Makefile.common, but these Makefiles have no such hook, so it goes
# in through their charmc override.
HIPDEF=-D__HIP_PLATFORM_AMD__
SIB_CHARMC="$CHARM/bin/charmc -DUNIONFIND $HIPDEF"

echo "### 1/6 prefixLib"
make -C "$UF/prefixLib" clean >/dev/null 2>&1 || true
make -C "$UF/prefixLib" CHARM_DIR="$CHARM" CHARMC="$SIB_CHARMC" -j"$JOBS"

echo "### 2/6 htram"
# htram master (2026-09) builds its libraries from Makefile_htram; the old
# top-level unionfind_smp target is gone.
make -C "$HTRAM" -f Makefile_htram clean-libs >/dev/null 2>&1 || true
rm -f "$HTRAM/htram_group.o"
make -C "$HTRAM" -f Makefile_htram libhtram_group_unionfind.a \
  CHARMC_SMP="$CHARM/bin/charmc $HIPDEF"

echo "### 3/6 unionfind"
make -C "$UF" clean >/dev/null 2>&1 || true
make -C "$UF" CHARM_DIR="$CHARM" CHARMC="$SIB_CHARMC" AGGREGATION= PROFILE= -j"$JOBS"

if [ "${GPU:-0}" = "1" ]; then
  echo "### 4a/6 fof device library (hipcc, no Charm headers)"
  make -C "$PT/fof/gpu" clean >/dev/null 2>&1 || true
  make -C "$PT/fof/gpu" libfofdevice.a
fi

echo "### 4/6 paratreet src"
make -C "$PT/src" clean >/dev/null 2>&1 || true
make -C "$PT/src" CHARM_HOME="$CHARM" -j"$JOBS"

echo "### 5/6 fof module"
make -C "$PT/fof" clean >/dev/null 2>&1 || true
make -C "$PT/fof" CHARM_HOME="$CHARM" GPU="${GPU:-0}" -j"$JOBS"

echo "### 6/6 examples/fof3 (PROJECTIONS=${PROJECTIONS:-0})"
make -C "$PT/examples/fof3" clean >/dev/null 2>&1 || true
make -C "$PT/examples/fof3" CHARM_HOME="$CHARM" GPU="${GPU:-0}" \
  PROJECTIONS="${PROJECTIONS:-0}" -j"$JOBS"
if [ "${PROJECTIONS:-0}" = "1" ]; then
  grep -q "tracemode projections" "$PT/examples/fof3/.tracemode" \
    || { echo "ERROR: PROJECTIONS=1 requested but .tracemode says otherwise"; exit 1; }
fi

echo "### done"
ls -la "$PT/examples/fof3/FoF3"
