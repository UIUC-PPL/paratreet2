#!/bin/bash
# Stage 2 follow-up: leaf size sweep (design/phase1-gpu.md section 3.2's
# open question, and the "most likely source of disappointing first
# numbers" the plan flagged).
#
# 12 particles/leaf is a CPU tuning. On the device it means 2.7M leaves
# each running a full independent descent, and the traversal is bound by
# uncoalesced node loads rather than by the distance tests. Bigger leaves
# trade fewer descents for more (dense, coalesced) pair work.
#
# The label gate travels with every arm: both sides use the same tree, so
# device-vs-CPU must still agree exactly whatever the leaf size.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:30:00
#SBATCH --job-name=fof3_gpu_leaf
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=stage2leaf.%j.out
#SBATCH --error=stage2leaf.%j.err

set -x
cd "$SLURM_SUBMIT_DIR"
# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh
export LCI_ATTR_BACKEND=ofi FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd PMI_MAX_KVS_ENTRIES=4194304
export PARATREET_DEVICE_TREE=1

APP=../../examples/fof3/FoF3
PEMAP=1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127
BIG=/lustre/orion/csc710/scratch/rrao/lambb.00500

for L in 12 32 64 128; do
  for GPU in 1 0; do
    echo "=== 80M leaf=$L gpu=$GPU ==="
    FOF_GPU_STAGE0=$GPU srun --mpi=cray_shasta --network=single_node_vni \
      --unbuffered --cpu-bind=none --distribution=block:block \
      --ntasks=8 --gpus-per-node=8 \
      $APP -f $BIG -d oct -u serial -l $L +noisomalloc +ppn 14 +pemap $PEMAP
    echo "exit: $?"
  done
done
