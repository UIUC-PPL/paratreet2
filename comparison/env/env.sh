# Common Frontier environment for the FoF comparison campaign.
# `module` is not a function in non-interactive shells -> source lmod init first.
if ! command -v module >/dev/null 2>&1; then
  source /usr/share/lmod/lmod/init/bash
fi
# Updated 2026-10-29: hwloc/2.11.1 and cmake/3.30.5 were retired after the
# campaign, and Lmod aborts the WHOLE compound load when one name is missing,
# so rocm never loaded. Errors are no longer sent to /dev/null (that is what
# hid it) and rocm is asserted. Same set as examples/fof3/scripts/frontier_env.sh.
module unload darshan-runtime >/dev/null 2>&1 || true
module load PrgEnv-gnu/8.6.0 gcc-native/13.2 craype/2.7.33 cray-mpich/8.1.31 \
  libfabric/2.3.1 xpmem hwloc/2.13.0 rocm/6.2.4 cray-libsci/24.11.0 \
  cmake/3.31.11
[ -n "${ROCM_PATH:-}" ] || { echo "### ABORT: rocm module did not load" >&2; exit 3; }
# The campaign's working tree (data/, clones, builds, logs). The paper-era
# copy is purgeable scratch; its logs and binaries are archived read-only in
# /ccs/proj/csc710/rrao/fof-sc26w-archive (see comparison/README.md).
export CMP=${CMP_DIR:-/lustre/orion/csc710/scratch/rrao/compare}

# Cray MPICH picks a NIC per rank by NUMA node by default, and REFUSES to
# start if a rank is not confined to one NUMA node:
#   MPICH ERROR: Unable to use a NIC_POLICY of 'NUMA'. Rank 98 is not
#   confined to a single NUMA node. There are 4 numa_nodes detected
# Frontier has 4 NUMA domains of 16 cores; 8 ranks x 7 cores straddles them
# whatever the binding, and --cpu-bind=none straddles them outright. Frontier
# has 4 NICs and 8 ranks/node, so round-robin is the right policy anyway.
# (This killed the first cosmo2b conversion, job 5403507, in 2 seconds.)
# Set UNCONDITIONALLY. A `${MPICH_OFI_NIC_POLICY:-ROUND-ROBIN}` default is
# WORTHLESS here: Frontier's site modules already export
# MPICH_OFI_NIC_POLICY=NUMA, so the default never fires and the "fix" does
# nothing at all -- job 5403617 died with the identical error 5403507 had,
# after the variable had supposedly been fixed. Override with CMP_NIC_POLICY
# if a run ever needs a different one.
export MPICH_OFI_NIC_POLICY=${CMP_NIC_POLICY:-ROUND-ROBIN}
