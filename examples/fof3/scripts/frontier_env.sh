# Shared Frontier environment for every FoF3 job and build script.
#
#   source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh
#
# SOURCE it, never pipe it (a pipe runs it in a subshell and the module
# environment is lost). An `exit` here exits the calling script: that is
# deliberate, every check below is fatal.
#
# Why one file: on 2026-09-19 Frontier retired hwloc/2.11.1, cmake/3.30.5 and
# python/3.13.0, and Lmod aborts an ENTIRE compound `module load` when one
# name is missing -- rocm/6.2.4 sat on the same line, never loaded, and every
# rank died at the loader (libamdhip64.so.6 not found). Twenty-odd scripts
# each carried their own copy of that line.

# --- modules: REQUIRED set only, one load -----------------------------------
# darshan-runtime makes the load error out on this PrgEnv; drop it first.
module unload darshan-runtime >/dev/null 2>&1 || true
module load craype-x86-trento craype-network-ofi PrgEnv-gnu/8.6.0 \
  gcc-native/13.2 craype/2.7.33 cray-mpich/8.1.31 libfabric/2.3.1 xpmem \
  hwloc/2.13.0 rocm/6.2.4 cray-libsci/24.11.0 cmake/3.31.11
# Even the CPU arm needs ROCm: a GPU=1 FoF3 links libamdhip64 regardless of
# whether FOF_GPU_PHASE1 is set.
[ -n "${ROCM_PATH:-}" ] || { echo "### ABORT: rocm module did not load" >&2; exit 3; }

# --- charm build -------------------------------------------------------------
# Set UNCONDITIONALLY. A `${CHARM_HOME:-default}` would silently keep whatever
# the submitting shell exported (sbatch propagates the environment), which is
# how a run ends up on the wrong build. Override with FOF3_CHARM_HOME.
#
# Do NOT put charm_reconverse/lib (the top-level symlink) on the path: it
# follows the most recent charm build and is kept on the CPU build for NAMD,
# and LD_LIBRARY_PATH beats the binary's RUNPATH -- an AMD-built FoF3 would
# load the CPU build's libreconverse.so.
export CHARM_HOME=${FOF3_CHARM_HOME:-/ccs/home/rrao/charm_reconverse/reconverse-linux-x86_64-amd}
[ -d "$CHARM_HOME/lib" ] || { echo "### ABORT: no charm build at $CHARM_HOME" >&2; exit 3; }
export LD_LIBRARY_PATH=$CHARM_HOME/lib:$ROCM_PATH/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

# --- binary preflight --------------------------------------------------------
# Abort, do not merely report: an unresolved library at 512 nodes is ~1000
# node-hours of identical loader failures. Also refuse a binary LINKED against
# a different charm build than CHARM_HOME. That has to be read from the
# binary's RUNPATH (charmc records the build's lib there): ldd cannot tell,
# because it resolves through the LD_LIBRARY_PATH set just above and so
# always "finds" libreconverse under CHARM_HOME.
FOF3_BIN=${FOF3_BIN:-/ccs/home/rrao/paratreet2/examples/fof3/FoF3}
if [ -x "$FOF3_BIN" ]; then
  _fof3_ldd=$(ldd "$FOF3_BIN" 2>&1)
  if grep -q "not found" <<<"$_fof3_ldd"; then
    grep "not found" <<<"$_fof3_ldd" >&2
    echo "### ABORT: $FOF3_BIN has unresolved shared libraries" >&2; exit 3
  fi
  _fof3_want=$(realpath "$CHARM_HOME/lib")
  _fof3_ok=0
  _fof3_rp=$(readelf -d "$FOF3_BIN" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')
  for _d in ${_fof3_rp//:/ }; do
    [ "$(realpath -m "$_d")" = "$_fof3_want" ] && _fof3_ok=1
  done
  if [ "$_fof3_ok" != 1 ]; then
    echo "### ABORT: $FOF3_BIN was linked against '$_fof3_rp', not $CHARM_HOME/lib" >&2
    echo "### ABORT: set FOF3_CHARM_HOME to the build it was linked against, or rebuild it" >&2
    exit 3
  fi
  echo "### fof3 binary=$FOF3_BIN md5=$(md5sum "$FOF3_BIN" | cut -c1-12) charm=$CHARM_HOME"
  unset _fof3_ldd _fof3_want _fof3_ok _fof3_rp _d
fi
echo "### rocm=$ROCM_PATH"
