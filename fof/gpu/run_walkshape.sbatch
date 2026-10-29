#!/bin/bash
# Walk-shape retest (design/phase1-gpu.md section 16).
#
# Stage 2 chose one wavefront per leaf over one thread per leaf and
# measured 850 -> 516 ms, on the evidence that the serial per-thread
# leaf-pair product dominated. Stage 3's connectivity suppression then
# removed ~40x of the work behind each leaf, so the team's fixed cost --
# a broadcast and a barrier on EVERY pop -- now amortises over far less
# work. This re-tests the choice instead of inheriting it.
#
# Both arms run the SAME traversal body (Wave/Solo adapters over
# walkOneLeaf), so a difference here is a difference in shape and nothing
# else, and both must still reproduce the CPU labels exactly.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=01:30:00
#SBATCH --job-name=fof3_walkshape
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=walkshape.%j.out
#SBATCH --error=walkshape.%j.err

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

run() { # label ntasks shape args...
  local label=$1 nt=$2 shape=$3; shift 3
  echo "=== $label (ntasks=$nt walk=$shape) ==="
  FOF_GPU_STAGE0=1 FOF_GPU_WALK=$shape \
    srun --mpi=cray_shasta --network=single_node_vni \
    --unbuffered --cpu-bind=none --distribution=block:block \
    --ntasks=$nt --gpus-per-node=8 \
    $APP "$@" +ppn 14 +pemap $PEMAP
  echo "exit: $?"
}

# 1. Correctness of the shared body FIRST: the traversal was moved behind
#    the Wave/Solo adapter, so the team arm has to be re-proved too.
run "1k team"    2 team -f ../../inputs/1k.tipsy   -d oct -u serial
run "1k solo"    2 solo -f ../../inputs/1k.tipsy   -d oct -u serial
run "100k team"  2 team -f ../../inputs/100k.tipsy -d oct -u serial
run "100k solo"  2 solo -f ../../inputs/100k.tipsy -d oct -u serial

# 2. The measurement. Shape and leaf size are ONE coupled knob -- the two
#    shapes cross between 3.7 and 8.3 particles/leaf -- so sweeping either
#    with the other pinned gives a wrong answer. Sweep both.
for L in 12 32 64 96 128 192; do
  for W in solo team; do
    run "80M leaf=$L" 8 $W -f $BIG -d oct -u serial -l $L
  done
done

# 3. The shipped default selects the shape from measured leaf occupancy;
#    confirm it picks solo at the default tree and team at a large one.
echo "=== 80M leaf=12 auto ==="
FOF_GPU_STAGE0=1 srun --mpi=cray_shasta --network=single_node_vni \
  --unbuffered --cpu-bind=none --distribution=block:block \
  --ntasks=8 --gpus-per-node=8 \
  $APP -f $BIG -d oct -u serial -l 12 +ppn 14 +pemap $PEMAP
echo "exit: $?"
echo "=== 80M leaf=128 auto ==="
FOF_GPU_STAGE0=1 srun --mpi=cray_shasta --network=single_node_vni \
  --unbuffered --cpu-bind=none --distribution=block:block \
  --ntasks=8 --gpus-per-node=8 \
  $APP -f $BIG -d oct -u serial -l 128 +ppn 14 +pemap $PEMAP
echo "exit: $?"
