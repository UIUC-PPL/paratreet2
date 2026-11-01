#!/bin/bash
# Stage 3 gate (design/phase1-gpu.md section 15): connectivity suppression
# and the dense-node cell grid.
#
# Correctness is the SAME gate as stage 2 and it is not weakened by
# anything here: both mechanisms only remove work that provably cannot
# change the partition (suppression prunes pairs whose endpoints are
# already one component; the grid solves a dense node's internal pairs
# exactly, by the c = b/sqrt(6) cell guarantees). So every arm must still
# report 0 label mismatches against the CPU chain, particle by particle.
#
# The measurement is an attribution, which is why the grid threshold is
# swept rather than fixed:
#   FOF_GPU_GRID=0     suppression only  -> vs stage 2's recorded 516 ms
#   FOF_GPU_GRID=4     the CPU's -G tuning
#   FOF_GPU_GRID=1     ) the device trade is not the CPU's: free unions
#   FOF_GPU_GRID=0.25  ) cost nothing here, cell probes do
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:55:00
#SBATCH --job-name=fof3_gpu_stage3
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=stage3.%j.out
#SBATCH --error=stage3.%j.err

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

run() { # label ntasks gpu grid args...
  local label=$1 nt=$2 gpu=$3 grid=$4; shift 4
  echo "=== $label (ntasks=$nt gpu=$gpu grid=$grid) ==="
  FOF_GPU_STAGE0=$gpu FOF_GPU_GRID=$grid \
    srun --mpi=cray_shasta --network=single_node_vni \
    --unbuffered --cpu-bind=none --distribution=block:block \
    --ntasks=$nt --gpus-per-node=8 \
    $APP "$@" +ppn 14 +pemap $PEMAP
  echo "exit: $?"
}

# 1. Small first, both arms of the grid gate. The 1k/100k inputs also
#    carry the full O(n^2) reference for the CPU side, so a wrong grid
#    shows up as a changed component count, not only as a mismatch.
run "100k grid off" 2 1 0    -f ../../inputs/100k.tipsy -d oct -u serial
run "100k grid 4"   2 1 4    -f ../../inputs/100k.tipsy -d oct -u serial
run "100k grid .25" 2 1 0.25 -f ../../inputs/100k.tipsy -d oct -u serial
run "1k grid 4"     2 1 4    -f ../../inputs/1k.tipsy   -d oct -u serial

# 2. The real comparison, same configuration as the scaling ledger's
#    1-node row (8 processes x 14 PEs, lambb.00500).
run "80M CPU only"  8 0 0    -f $BIG -d oct -u serial
run "80M suppress"  8 1 0    -f $BIG -d oct -u serial
run "80M grid 4"    8 1 4    -f $BIG -d oct -u serial
run "80M grid 1"    8 1 1    -f $BIG -d oct -u serial
run "80M grid .25"  8 1 0.25 -f $BIG -d oct -u serial
