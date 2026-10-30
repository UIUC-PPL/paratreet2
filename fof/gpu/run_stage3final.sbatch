#!/bin/bash
# Stage 3 final gate (design/phase1-gpu.md section 15): the shipped
# configuration -- connectivity suppression on, cell grid off by default
# -- plus one grid-on arm to keep that path exercised and verified.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:30:00
#SBATCH --job-name=fof3_gpu_s3f
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=stage3final.%j.out
#SBATCH --error=stage3final.%j.err
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
run() { local label=$1 nt=$2 gpu=$3; shift 3
  echo "=== $label (ntasks=$nt gpu=$gpu grid=${FOF_GPU_GRID:-default}) ==="
  FOF_GPU_STAGE0=$gpu srun --mpi=cray_shasta --network=single_node_vni \
    --unbuffered --cpu-bind=none --distribution=block:block \
    --ntasks=$nt --gpus-per-node=8 $APP "$@" +noisomalloc +ppn 14 +pemap $PEMAP
  echo "exit: $?"; }
run "1k default"    2 1 -f ../../inputs/1k.tipsy   -d oct -u serial
run "100k default"  2 1 -f ../../inputs/100k.tipsy -d oct -u serial
FOF_GPU_GRID=0.25 run "100k grid .25" 2 1 -f ../../inputs/100k.tipsy -d oct -u serial
run "80M CPU only"  8 0 -f $BIG -d oct -u serial
run "80M default"   8 1 -f $BIG -d oct -u serial
FOF_GPU_GRID=1 run "80M grid 1" 8 1 -f $BIG -d oct -u serial
