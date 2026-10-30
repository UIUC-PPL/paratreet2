#!/bin/bash
# Stage 0, second half (design/phase1-gpu.md section 8): the K0/K5 staging
# round trip inside the real application, against real TreePiece particle
# blocks, with the CPU chain still computing the answer.
#
# The gate is twofold: FOF3GPU must report zero round-trip failures (which
# checks the per-PE base/offset arithmetic end to end, not merely that a
# transfer happened), AND the component count must be unchanged from the
# CPU-only arm, since the staging must not perturb anything.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:20:00
#SBATCH --job-name=fof3_gpu_stage0
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=stage0_fof3.%j.out
#SBATCH --error=stage0_fof3.%j.err

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

run() { # label, ntasks, staging on/off, extra args...
  local label=$1 nt=$2 gpu=$3; shift 3
  echo "=== $label (ntasks=$nt FOF_GPU_STAGE0=$gpu) ==="
  FOF_GPU_STAGE0=$gpu srun --mpi=cray_shasta --network=single_node_vni \
    --unbuffered --cpu-bind=none --distribution=block:block \
    --ntasks=$nt --gpus-per-node=8 \
    $APP "$@" +noisomalloc +ppn 14 +pemap $PEMAP
  echo "exit: $?"
}

# 1. Small, 2 processes: exercises the chain and the offsets where a
#    wrong answer is obvious. Both arms must report the same components.
run "100k CPU-only"   2 0 -f ../../inputs/100k.tipsy -d oct -u serial
run "100k staging"    2 1 -f ../../inputs/100k.tipsy -d oct -u serial

# 2. Real scale, 8 processes = 8 GCDs: what the pack actually costs on
#    ~10M particles per process out of 112-byte Particle structs.
run "80M CPU-only"    8 0 -f $BIG -d oct -u serial
run "80M staging"     8 1 -f $BIG -d oct -u serial
