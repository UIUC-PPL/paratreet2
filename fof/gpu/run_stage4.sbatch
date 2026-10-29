#!/bin/bash
# Stage 4 gate (design/phase1-gpu.md section 18): the device path REPLACES
# the CPU chain.
#
# Through stage 3 the device pass ran BESIDE phaseA+phaseB+merge+relabel
# and was checked against them particle by particle. That gate stays (it is
# FOF_GPU_VERIFY=1 now, the same thing FOF_GPU_STAGE0=1 selected), but it
# says nothing about the new code, which is deviceAdopt: turning the
# device's labels into the section-4 contract (uf_parent / roots /
# rep_label / root_counts) that phase 3, UF_2 and the histogram consume.
#
# The gate on THAT is a chain of four links, each of which is checked here:
#
#   1. device labels == CPU labels          <- the verify arms (exact, per
#                                              particle, aborts on the first
#                                              disagreement)
#   2. group_number == device labels        <- by construction; deviceAdopt
#                                              assigns it and nothing else does
#   3. the representative structure reproduces the particle labels
#                                           <- FOF_COUNT_VERIFY=1, which
#                                              recomputes the per-label counts
#                                              from the particles and aborts on
#                                              disagreement. Exported below.
#   4. end to end, the replace arm and the CPU arm agree
#                                           <- the FOF3 components line, which
#                                              the harness documents as the
#                                              cross-run determinism observable
#
# -c full additionally runs the serial O(n^2) grid reference on PE 0 for
# the small inputs, so those arms are checked against something that shares
# no code with either the CPU chain or the device.
#
# MULTI-ITERATION ARMS ARE NOT OPTIONAL. Every device run so far has been
# -i 1 (the default), and the path did not survive a second iteration: the
# per-process deposit counters were never reset (round 3's trigger compares
# fetch_add against CkNodeSize, so iteration 2 hangs with no error), and the
# device's top tree was cached on "have I built one" rather than on "is this
# the tree I built it from". Both are fixed; -i 3 is what keeps them fixed.
#
# -i 3 needed a CORE fix before it tested anything: TreePiece::reset()
# cleared `particles` every iteration and only perturb/rebucket refilled
# them, which a non-perturbing app never runs (trap 20). Until that landed
# these arms failed in the CPU arm too.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=01:00:00
#SBATCH --job-name=fof3_stage4
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=stage4.%j.out
#SBATCH --error=stage4.%j.err

set -x
cd "$SLURM_SUBMIT_DIR"

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh

export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
export PMI_MAX_KVS_ENTRIES=4194304
# Link 3 of the correctness chain above. Not a debug switch here: it is the
# only thing that checks the run-length representative construction.
export FOF_COUNT_VERIFY=1
export PARATREET_DEVICE_TREE=1

APP=../../examples/fof3/FoF3
PEMAP=1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127
BIG=/lustre/orion/csc710/scratch/rrao/lambb.00500

# run <label> <ntasks> <mode: off|replace|verify> [args...]
run() {
  local label=$1 nt=$2 mode=$3; shift 3
  local env=""
  case "$mode" in
    replace) env="FOF_GPU_PHASE1=1" ;;
    verify)  env="FOF_GPU_VERIFY=1" ;;
    off)     env="PARATREET_DEVICE_TREE=0" ;;
  esac
  echo "=== $label (ntasks=$nt mode=$mode ${EXTRA_ENV}) ==="
  env $env $EXTRA_ENV \
    srun --mpi=cray_shasta --network=single_node_vni \
    --unbuffered --cpu-bind=none --distribution=block:block \
    --ntasks=$nt --gpus-per-node=8 \
    $APP "$@" +ppn 14 +pemap $PEMAP
  echo "exit: $?"
}

# ---- 1. Small, -c full: an independent O(n^2) reference on PE 0 ----
# The cpu arms are the answer the replace arms have to reproduce EXACTLY,
# and -c full checks both of them against the grid reference besides.
run "1k cpu"        2 off     -f ../../inputs/1k.tipsy   -d oct -u dist -c full
run "1k replace"    2 replace -f ../../inputs/1k.tipsy   -d oct -u dist -c full
run "100k cpu"      2 off     -f ../../inputs/100k.tipsy -d oct -u dist -c full
run "100k replace"  2 replace -f ../../inputs/100k.tipsy -d oct -u dist -c full
run "100k verify"   2 verify  -f ../../inputs/100k.tipsy -d oct -u dist -c full

# Same, at the process count the production runs use.
run "100k cpu p8"       8 off     -f ../../inputs/100k.tipsy -d oct -u dist -c full
run "100k replace p8"   8 replace -f ../../inputs/100k.tipsy -d oct -u dist -c full
run "100k verify p8"    8 verify  -f ../../inputs/100k.tipsy -d oct -u dist -c full

# The asynchronous completion path (FOF_GPU_ASYNC=1): same kernels, same
# order, same stream, three fewer fences. It has to give the same answer
# as the default synchronous form, or those missing fences are hiding a
# real dependency. It is opt-in because at 2B it defers 0.3 ms of a 308 ms
# pass (design/phase1-gpu.md section 18.8).
EXTRA_ENV="FOF_GPU_ASYNC=1" \
  run "100k replace async" 8 replace -f ../../inputs/100k.tipsy -d oct -u dist -c full
EXTRA_ENV=

# ---- 2. Multi-iteration: the deposit counters and the top-tree cache ----
run "100k cpu i3"      8 off     -f ../../inputs/100k.tipsy -d oct -u dist -c full -i 3
run "100k replace i3"  8 replace -f ../../inputs/100k.tipsy -d oct -u dist -c full -i 3
run "100k verify i3"   8 verify  -f ../../inputs/100k.tipsy -d oct -u dist -c full -i 3

# ---- 3. 80M: the measurement, and the gate at a scale -c full cannot reach ----
# -c stats keeps the distributed component histogram (and with it
# FOF_COUNT_VERIFY) without the PE-0 gather.
run "80M cpu l12"      8 off     -f $BIG -d oct -u dist -c stats -l 12
run "80M replace l12"  8 replace -f $BIG -d oct -u dist -c stats -l 12
run "80M verify l12"   8 verify  -f $BIG -d oct -u dist -c stats -l 12
# The device wants the coarse tree and the CPU does not (section 16), so
# this is the configuration the replace path would actually ship with.
run "80M replace l128" 8 replace -f $BIG -d oct -u dist -c stats -l 128

# ---- 4. Does releasing the staging buy back section 17.3's phase 3? ----
# Identical to the arm above except that the ~600 MB of pinned staging is
# freed after the scatter instead of staying resident. If phase 3 and the
# histogram move between these two, memory is the 17.3 regression. (It
# did not: 12 ms over three iterations, against ~100 ms per iteration
# spent re-allocating pinned memory. Hence the default is OFF, and this
# arm needs -i > 1 to mean anything at all -- see run_stage4_iter.sbatch.)
EXTRA_ENV="FOF_GPU_RELEASE=1" \
  run "80M replace l128 release" 8 replace -f $BIG -d oct -u dist -c stats -l 128 -i 3
EXTRA_ENV=

# ---- 5. Multi-iteration at 80M, replace: the shipping configuration ----
run "80M replace l128 i3" 8 replace -f $BIG -d oct -u dist -c stats -l 128 -i 3
