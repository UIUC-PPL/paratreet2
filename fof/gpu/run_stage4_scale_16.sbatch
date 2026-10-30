#!/bin/bash
# Stage 4 at production scale: 2B particles, 16 nodes, 128 processes.
#
# This is the run the whole stage exists for. Section 17 measured a device
# pass 12-18x faster than phaseA+phaseB+merge and an application 56%
# SLOWER, because the pass ran beside the chain instead of replacing it.
# Stage 4 replaced it. The question here is the only one that matters:
# is the application faster end to end at 2B, and by how much.
#
# UNTRACED, deliberately. Section 17 collected Projections traces and they
# are the right tool for the section-17.3 phase-3 regression, but a claim
# about iteration time has to be measured without per-event logging on the
# hot path. Traces are the next job, not this one.
#
# -i 2 on the arms that carry the headline: iteration 0 pays a one-time
# ~0.6 s of Kokkos/HIP init and pinned allocation inside phase 1 (measured
# at 80M: phase 1 was 0.983 s on iteration 0 and 0.374 s after), so a
# single-iteration run reports the device path's startup as its cost.
# Both iterations are reported; the steady-state one is the number.
#
# Arms:
#   1. cpu_l12          the baseline, device-free
#   2. replace_l128     the shipping configuration (section 16: the device
#                       wants the coarse tree the CPU is punished by)
#   3. replace_l12      the same tree as the baseline, apples to apples
#   4. verify_l12       the per-particle gate at 2B, 128 processes, -u dist
#   5. replace_l128_async  the completion-path A/B. FOF_GPU_ASYNC=1 drops
#                       the fences and takes completion through
#                       hapiAddCallback. At 80M the async pass already
#                       reported ~98% of itself as BLOCKING, which says
#                       the enqueue itself -- not just the leaf-count
#                       readback the design predicted -- is what blocks.
#                       This arm settles it at scale. (It did: 0.3 ms of
#                       308 ms deferred, so synchronous is now the
#                       default. See section 18.8.)
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:30:00
#SBATCH --job-name=fof3_stage4_16
#SBATCH --nodes=16
#SBATCH --ntasks=128
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=stage4_16.%j.out
#SBATCH --error=stage4_16.%j.err

set -x

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh


export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
export PMI_MAX_KVS_ENTRIES=4194304
export FOF_COUNT_VERIFY=1

INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
PEMAP=1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127

# Walltime guard, kept from section 17.5 trap 18: 30 minutes is a backfill
# request, not a measured bound (the section-17 job did three 2B arms in
# 1:42), so each arm checks that enough is left before starting. Skipping
# an arm cleanly beats being killed mid-arm.
JOB_START=$(date +%s)
LIMIT_S=$((30*60))
have_time() {
  local left=$(( LIMIT_S - ( $(date +%s) - JOB_START ) ))
  echo "walltime remaining: ${left}s (arm wants $1s)"
  [ "$left" -ge "$1" ]
}

cd /ccs/home/rrao/paratreet2/examples/fof3

run() { # label mode leaf iters seconds_needed [extra env]
  local label=$1 mode=$2 leaf=$3 iters=$4 need=$5 extra=$6
  have_time "$need" || { echo "=== SKIP $label (insufficient walltime) ==="; return 0; }
  local e=""
  case "$mode" in
    replace) e="PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1" ;;
    verify)  e="PARATREET_DEVICE_TREE=1 FOF_GPU_VERIFY=1" ;;
    off)     e="PARATREET_DEVICE_TREE=0" ;;
  esac
  echo "=== $label (mode=$mode leaf=$leaf iters=$iters $extra) ==="
  date
  env $e $extra \
    srun --mpi=cray_shasta --network=job_vni --unbuffered \
    --cpu-bind=none --distribution=block:block \
    --ntasks=128 --gpus-per-node=8 \
    ./FoF3 -f "$INPUT" -d oct -u dist -c stats -l $leaf -i $iters \
    +noisomalloc +ppn 14 +pemap $PEMAP +lci_ndevices 7 +backend_poll_thread 2
  echo "exit: $?"
  date
}

# Ordered by value: the two arms that carry the end-to-end claim first,
# so a walltime miss costs a supporting arm rather than the headline.
run cpu_l12           off     12  2 300
run replace_l128      replace 128 2 300
run replace_l12       replace 12  2 300
run verify_l12        verify  12  1 300
run replace_l128_async replace 128 2 300 FOF_GPU_ASYNC=1
