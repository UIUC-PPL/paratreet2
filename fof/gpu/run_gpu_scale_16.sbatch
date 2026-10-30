#!/bin/bash
# 2B particles, 16 physical nodes, with Projections traces: the first
# run of the stage 0-3 device path at production scale.
#
# Derived from examples/fof3/scripts/run_fof3_scale_16.sbatch (same
# input, same 128 x 14 decomposition, same -u dist / lci_ndevices /
# poll-thread settings), with four changes the GPU arm needs:
#   1. rocm/6.2.4 loaded and the rocm paths on LD_LIBRARY_PATH -- charm
#      and Kokkos are both built against 6.2.4, and a mismatch shows up
#      as an undefined hsa_* symbol at load (design section 11.3 trap 8);
#   2. --gpus-per-node=8, so the 8 processes per node map 1:1 onto the 8
#      GCDs -- the invariant Device::checkMapping enforces and aborts on;
#   3. PARATREET_DEVICE_TREE=1, which emits the flat device tree at
#      build time (stage 1);
#   4. a SEPARATE traceroot per arm, created before the run. Projections
#      does not create it, and a missing traceroot costs the whole run.
#
# Traces go to Lustre, not to the example directory: 16 x 8 x 14 = 1792
# PEs of logs is not something to point at a home filesystem that is 85%
# full.
#
# +logsize is 20000000, not the CPU script's 100000000. It is a per-PE
# entry ceiling that LogPool reserve()s up front, and at 112 PEs per node
# 100M entries x ~80 B reserves ~900 GB of address space per node against
# 512 GB of RAM -- survivable only because reserve() commits nothing until
# the entries are written. A 2-node smoke run at 100k measured ~68 KB
# compressed per PE, so 20M entries is still two orders of magnitude of
# headroom. logsize changes buffering, never trace CONTENT, so this costs
# nothing in comparability against the CPU runs.
#
# THE GATE STILL RUNS. FOF_GPU_STAGE0=1 leaves the CPU chain in place and
# compares every particle's device label against it, aborting on the
# first disagreement, so these are correctness runs as much as
# measurement runs -- and the first at 2B, 128 processes, and -u dist.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:45:00
#SBATCH --job-name=fof3_gpu_scale_16
#SBATCH --nodes=16
#SBATCH --ntasks=128
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=gpu_scale_16.%j.out
#SBATCH --error=gpu_scale_16.%j.err

set -x

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh


# CXI multi-process-per-node fixes, unchanged from the CPU script:
# job_vni + cray_shasta PMI plugin; LCI_ATTR_BACKEND=ofi +
# FI_CXI_RX_MATCH_MODE=hybrid (CXI hardware match-table exhaustion at 8
# procs/node); PMI_MAX_KVS_ENTRIES bumped; +backend_poll_thread 2.
export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
export PMI_MAX_KVS_ENTRIES=4194304
export FOF_COUNT_VERIFY=1

INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
PPN=14
NDEVICES=7
PEMAP=1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127
TRACEBASE=/lustre/orion/csc710/scratch/rrao/fof3_traces/$SLURM_JOB_ID

# Walltime guard. 45 minutes is a backfill request, not a measured
# bound, so each arm checks that enough of it is left before starting.
# Being killed mid-arm would truncate that arm's 1792 trace files into
# an unreadable set AND cost the arms after it, which is strictly worse
# than skipping cleanly and reporting what ran.
JOB_START=$(date +%s)
LIMIT_S=$((45*60))
have_time() {  # have_time <seconds_needed>
  local left=$(( LIMIT_S - ( $(date +%s) - JOB_START ) ))
  echo "walltime remaining: ${left}s (arm wants $1s)"
  [ "$left" -ge "$1" ]
}

cd /ccs/home/rrao/paratreet2/examples/fof3

run() { # label gpu leaf seconds_needed
  local label=$1 gpu=$2 leaf=$3 need=$4
  have_time "$need" || { echo "=== SKIP $label (insufficient walltime) ==="; return 0; }
  local troot="$TRACEBASE/$label"
  # Projections will NOT create this, and it is silently fatal if absent.
  mkdir -p "$troot" || exit 1
  echo "=== $label (gpu=$gpu leaf=$leaf traceroot=$troot) ==="
  date
  PARATREET_DEVICE_TREE=$gpu FOF_GPU_STAGE0=$gpu \
    srun --mpi=cray_shasta --network=job_vni --unbuffered \
    --cpu-bind=none --distribution=block:block \
    --ntasks=128 --gpus-per-node=8 \
    ./FoF3 -f "$INPUT" -d oct -u dist -l $leaf +noisomalloc +ppn $PPN \
    +pemap $PEMAP \
    +lci_ndevices $NDEVICES \
    +backend_poll_thread 2 \
    +traceroot "$troot" +logsize 20000000
  echo "exit: $?"
  date
  du -sh "$troot" 2>/dev/null
}

# Ordered by value, because the walltime is a guess and the last arm is
# the one that gets dropped. The GPU arms run the CPU chain too (the
# gate compares against it), so gpu_l12 alone yields BOTH the device and
# the CPU phase-1 numbers -- which makes the CPU-only arm the one worth
# losing. It earns its place only as an untainted CPU timeline to diff
# against, not as the source of the baseline number.

# 1. Apples-to-apples with the CPU tree, and the first per-particle gate
#    at 2B / 128 processes.
run gpu_l12 1 12 900

# 2. The tuned device configuration (section 16: the device wants a much
#    coarser tree than the CPU does). The CPU chain here is deliberately
#    mis-tuned -- it is the oracle in this arm, not the baseline.
run gpu_l128 1 128 900

# 3. CPU-only, same tree as arm 1: a device-free timeline to diff.
run cpu_l12 0 12 600
