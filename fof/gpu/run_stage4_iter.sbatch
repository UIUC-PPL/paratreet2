#!/bin/bash
# Stage 4, part 2: the multi-iteration gate that part 1 could not run.
#
# run_stage4.sbatch's -i 3 arms all failed, and they failed in the CPU-ONLY
# arm too, so they were not testing the device path at all: paratreet's
# TreePiece::buildTree swapped in incoming_particles unconditionally, and
# for an app that does not move particles (fof3: perturb_particles = false)
# Driver::run skips perturb/rebucket entirely, so from iteration 2 that
# buffer is EMPTY. Every TreePiece got zero particles and the run aborted
# with "final gathered 0 records". Fixed in src/TreePiece.h; this is what
# proves it, and what finally exercises the two per-iteration bugs stage 4
# fixed in the device path (design/phase1-gpu.md section 18.3).
#
# What each -i 3 arm is actually checking:
#   * the CPU arms: that the core fix restores multi-iteration at all, and
#     that iterations 2 and 3 produce the SAME components as iteration 1.
#   * the replace/verify arms: that the device deposit counters reset (an
#     unreset counter hangs, silently), and that the device's top tree is
#     rebuilt per tree upload rather than cached across builds.
#
# The last arm is the FOF_GPU_RELEASE A/B, which only means anything with
# more than one iteration: releasing the staging costs a ~600 MB PINNED
# re-allocation at the START of every later iteration, and a single-
# iteration run never pays it. That is why the default is OFF.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:40:00
#SBATCH --job-name=fof3_stage4_iter
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=stage4iter.%j.out
#SBATCH --error=stage4iter.%j.err

set -x
cd "$SLURM_SUBMIT_DIR"

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh

export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
export PMI_MAX_KVS_ENTRIES=4194304
export FOF_COUNT_VERIFY=1
export PARATREET_DEVICE_TREE=1

APP=../../examples/fof3/FoF3
PEMAP=1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127
BIG=/lustre/orion/csc710/scratch/rrao/lambb.00500

run() {
  local label=$1 nt=$2 mode=$3; shift 3
  local e=""
  case "$mode" in
    replace) e="FOF_GPU_PHASE1=1" ;;
    verify)  e="FOF_GPU_VERIFY=1" ;;
    off)     e="PARATREET_DEVICE_TREE=0" ;;
  esac
  echo "=== $label (ntasks=$nt mode=$mode ${EXTRA_ENV}) ==="
  env $e $EXTRA_ENV \
    srun --mpi=cray_shasta --network=single_node_vni \
    --unbuffered --cpu-bind=none --distribution=block:block \
    --ntasks=$nt --gpus-per-node=8 \
    $APP "$@" +noisomalloc +ppn 14 +pemap $PEMAP
  echo "exit: $?"
}

run "100k cpu i3"      8 off     -f ../../inputs/100k.tipsy -d oct -u dist -c full  -i 3
run "100k replace i3"  8 replace -f ../../inputs/100k.tipsy -d oct -u dist -c full  -i 3
run "100k verify i3"   8 verify  -f ../../inputs/100k.tipsy -d oct -u dist -c full  -i 3

run "80M cpu l12 i3"       8 off     -f $BIG -d oct -u dist -c stats -l 12  -i 3
run "80M replace l128 i3"  8 replace -f $BIG -d oct -u dist -c stats -l 128 -i 3
run "80M verify l12 i3"    8 verify  -f $BIG -d oct -u dist -c stats -l 12  -i 3

# The release A/B, which needs iterations to mean anything.
EXTRA_ENV="FOF_GPU_RELEASE=1" \
  run "80M replace l128 i3 release" 8 replace -f $BIG -d oct -u dist -c stats -l 128 -i 3
EXTRA_ENV=
