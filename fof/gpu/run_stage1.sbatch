#!/bin/bash
# Stage 1 gate (design/phase1-gpu.md): the flat device tree, emitted at
# TREE BUILD time and uploaded once.
#
# Three things are being gated, in order of what would fail first:
#   1. the flat tree matches the live tree it came from, checked on the
#      HOST as it is built (PARATREET_DEVICE_TREE_VERIFY=1);
#   2. it survives the concatenation, index rebase and upload, checked ON
#      THE DEVICE (structural scan + particles-under-roots == staged);
#   3. CPU results are unchanged — the emit is additive, so component
#      counts must be identical with the flat tree off and on.
# Tree-build delta is the measurement: the whole "the tree is free" claim
# rests on it.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:25:00
#SBATCH --job-name=fof3_gpu_stage1
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=stage1.%j.out
#SBATCH --error=stage1.%j.err

set -x
cd "$SLURM_SUBMIT_DIR"

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh

export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
export PMI_MAX_KVS_ENTRIES=4194304
export FOF_COUNT_VERIFY=1

APP=../../examples/fof3/FoF3
PEMAP=1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127
BIG=/lustre/orion/csc710/scratch/rrao/lambb.00500

run() { # label ntasks tree verify stage0 args...
  local label=$1 nt=$2 tree=$3 verify=$4 stage0=$5; shift 5
  echo "=== $label (tree=$tree verify=$verify stage0=$stage0) ==="
  PARATREET_DEVICE_TREE=$tree PARATREET_DEVICE_TREE_VERIFY=$verify \
  FOF_GPU_STAGE0=$stage0 \
    srun --mpi=cray_shasta --network=single_node_vni --unbuffered \
    --cpu-bind=none --distribution=block:block \
    --ntasks=$nt --gpus-per-node=8 \
    $APP "$@" +ppn 14 +pemap $PEMAP
  echo "exit: $?"
}

# 1. Small: host-side flat-tree verification on, where a structural bug is
#    caught at the tree that produced it.
run "100k tree off"        2 0 0 0 -f ../../inputs/100k.tipsy -d oct -u serial
run "100k tree+verify"     2 1 1 0 -f ../../inputs/100k.tipsy -d oct -u serial
run "100k tree+verify+gpu" 2 1 1 1 -f ../../inputs/100k.tipsy -d oct -u serial

# 2. Real scale: the tree-build delta (off vs on) and the upload cost.
run "80M tree off"         8 0 0 0 -f $BIG -d oct -u serial
run "80M tree on"          8 1 0 0 -f $BIG -d oct -u serial
run "80M tree+verify"      8 1 1 0 -f $BIG -d oct -u serial
run "80M tree+gpu upload"  8 1 0 1 -f $BIG -d oct -u serial
