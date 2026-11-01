#!/bin/bash
# Stage 2 gate (design/phase1-gpu.md): the device union-find + leaf-batched
# traversal, i.e. phaseA + phaseB + merge on the GPU.
#
# The gate is an EXACT PER-PARTICLE comparison, not a component count: the
# CPU chain runs first and leaves each particle's process-level tip in
# group_number, and the device pass must reproduce that value for every
# particle on every process. Both use union-by-minimum-global-order, which
# is a total order, so the answer is order-independent and the two must
# agree exactly.
#
# The measurement is against design/fof3-lambb500-scaling.md's 1-node row:
# phaseA 1.563 s + phaseB 0.152 s + merge 0.003 s = 1.718 s at 8 procs x
# 14 PEs on this same dataset.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:30:00
#SBATCH --job-name=fof3_gpu_stage2
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=stage2.%j.out
#SBATCH --error=stage2.%j.err

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

run() { # label ntasks gpu args...
  local label=$1 nt=$2 gpu=$3; shift 3
  echo "=== $label (ntasks=$nt gpu=$gpu) ==="
  FOF_GPU_STAGE0=$gpu srun --mpi=cray_shasta --network=single_node_vni \
    --unbuffered --cpu-bind=none --distribution=block:block \
    --ntasks=$nt --gpus-per-node=8 \
    $APP "$@" +ppn 14 +pemap $PEMAP
  echo "exit: $?"
}

# 1. Small first: a label mismatch here is debuggable, and the 100k arm
#    also carries the full O(n^2) reference for the CPU side.
run "100k device UF"  2 1 -f ../../inputs/100k.tipsy -d oct -u serial
run "1k device UF"    2 1 -f ../../inputs/1k.tipsy   -d oct -u serial

# 2. The real comparison, same configuration as the scaling ledger's
#    1-node row (8 processes x 14 PEs, lambb.00500).
run "80M CPU only"    8 0 -f $BIG -d oct -u serial
run "80M device UF"   8 1 -f $BIG -d oct -u serial
