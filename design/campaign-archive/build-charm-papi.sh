#!/bin/bash
# Build a TRACING charm/reconverse with PAPI counters enabled, alongside the
# existing charm-prodtr rather than modifying it -- every binary the campaign
# has built points at charm-prodtr and must keep working.
#
# Purpose (Kale, 2026-08-24): name the mechanism behind relay106's result
# rather than infer it.  The free work counters already showed the GPU walk
# does the SAME WORK when capped and does it 16.4% faster (leaf_visits
# 9,907,448 -> 9,900,835, i.e. -0.067%; same_frag byte-identical).  PAPI turns
# "it must be memory" into a measurement:
#   PAPI_TOT_INS  the CONTROL -- should be flat if the work is really the same
#   PAPI_L3_TCM   the signal   -- pool halves, so misses should fall
#   PAPI_TLB_DM   the signal   -- 77 MB less pool per process
#
# The existing tracing build has the machine layer present but disabled:
#   charm-prodtr/.../include/conv-autoconfig.h:  #define CMK_HAS_COUNTER_PAPI 0
#   charm-prodtr/.../CMakeCache.txt:             PAPI:BOOL=OFF
# so this is a reconfigure of the same source at the same commit, with
# -DPAPI=ON and the papi module loaded.  Nothing else changes.
set -eu
M=$HOME/software/merged
SRC=$M/charm-prodtr
DST=$M/charm-papi
B=reconverse-linux-x86_64-amd

source /opt/cray/pe/lmod/lmod/init/bash
module load craype-x86-trento libfabric/2.3.1 craype-network-ofi \
  perftools-base/24.11.0 xpmem/1.0.1-1.5_1_gfb6998056825 cray-pmi/6.1.15 \
  Core/25.03 hwloc/2.11.1 gcc-native/13.2 craype/2.7.33 cray-dsmml/0.3.0 \
  cray-mpich/8.1.31 cray-libsci/24.11.0 PrgEnv-gnu/8.6.0 rocm/6.2.4 \
  cmake/3.30.5 > /dev/null 2>&1
# NOT PIPED.  `module` is a shell FUNCTION; `module load X | tail -1` runs it in
# a SUBSHELL and every setenv it performs is discarded when that subshell exits.
# That is why the first two attempts saw OLCF_PAPI_ROOT unset even though the
# module loads "succeeded".  Every other build script in this campaign has the
# same piped pattern and gets away with it only because cmake is given absolute
# compiler paths and the login environment already carries the rest.
module load papi/7.1.0 > /dev/null 2>&1
echo "############ PAPI MODULE"
# The OLCF module sets OLCF_PAPI_ROOT (plus CMAKE_PREFIX_PATH and
# PKG_CONFIG_PATH).  It does NOT set PAPI_DIR -- my first attempt gated on that
# name and aborted, correctly, before copying or building anything.
PAPI_DIR="${OLCF_PAPI_ROOT:-${PAPI_ROOT:-${PAPI_DIR:-}}}"
echo "  OLCF_PAPI_ROOT=${OLCF_PAPI_ROOT:-unset}"
echo "  using PAPI_DIR=${PAPI_DIR:-unset}"
[ -n "${PAPI_DIR:-}" ] || { echo "  !!! no PAPI root in the environment"; exit 2; }
[ -f "$PAPI_DIR/include/papi.h" ] || { echo "  !!! papi.h not found under $PAPI_DIR"; exit 2; }
[ -f "$PAPI_DIR/lib/libpapi.so" ] || [ -f "$PAPI_DIR/lib64/libpapi.so" ] \
  || { echo "  !!! libpapi.so not found under $PAPI_DIR"; exit 2; }
export PAPI_DIR
echo "  papi.h and libpapi.so ok"

echo "############ SOURCE"
echo "  charm-prodtr @ $(git -C $SRC rev-parse --short HEAD 2>/dev/null || echo '(not a git tree)')"
if [ -d "$DST" ]; then
  echo "  $DST exists -- reusing it (delete it by hand to start clean)"
else
  echo "  copying $SRC -> $DST  (463 MB, keeps charm-prodtr untouched)"
  cp -a "$SRC" "$DST"
  rm -rf "$DST/$B"          # drop the configured no-PAPI build tree
fi

echo "############ CONFIGURE with -DPAPI=ON"
mkdir -p "$DST/$B"; cd "$DST/$B"
cmake .. \
  -DNETWORK=reconverse -DRECONVERSE=1 -DSMP=0 -DCMK_SMP=ON -DCMK_CPV_IS_SMP=ON \
  -DTRACING=1 -DTRACING_COMMTHREAD=0 \
  -DPAPI=ON \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=0 -DENABLE_FORTRAN=1 \
  -DRECONVERSE_ATOMIC_QUEUE=ON -DRECONVERSE_ENABLE_CPU_AFFINITY=ON \
  -DRECONVERSE_BUILD_TESTS=OFF -DRECONVERSE_AUTOFETCH_LCI2=0 \
  -DLCI_DIR=$M/lci-install/lib64/cmake \
  -DCMAKE_C_COMPILER=/opt/cray/pe/gcc-native/13/bin/gcc \
  -DCMAKE_CXX_COMPILER=/opt/cray/pe/gcc-native/13/bin/g++ \
  -DCMAKE_PREFIX_PATH="${PAPI_DIR}" \
  > $M/log-charmpapi-cmake.txt 2>&1 || { echo "  !!! cmake FAILED"; tail -30 $M/log-charmpapi-cmake.txt; exit 1; }
echo "  cmake ok"

echo "############ THE GATE THAT DECIDES IF THIS WAS WORTH IT"
HDR=$DST/$B/include/conv-autoconfig.h
if [ -f "$HDR" ]; then
  grep -m1 "CMK_HAS_COUNTER_PAPI" "$HDR" | sed 's/^/  /'
  grep -q "define CMK_HAS_COUNTER_PAPI 1" "$HDR" || {
    echo "  !!! CMK_HAS_COUNTER_PAPI is still 0 -- cmake accepted -DPAPI=ON but"
    echo "      the configure test did not find PAPI.  Building further is waste."
    grep -i papi $M/log-charmpapi-cmake.txt | tail -10 | sed 's/^/      /'
    exit 3; }
  echo "  CMK_HAS_COUNTER_PAPI 1 -- PAPI is really in"
else
  echo "  !!! conv-autoconfig.h not generated"; exit 3
fi

echo "############ BUILD"
make -j16 > $M/log-charmpapi-make.txt 2>&1 || { echo "  !!! make FAILED"; tail -40 $M/log-charmpapi-make.txt; exit 1; }
echo "  charm built: $DST/$B"
ls -la $DST/$B/lib/libconverse* 2>/dev/null | head -3
echo "############ DONE -- next: rebuild FoF3 against CHARM_HOME=$DST/$B"
