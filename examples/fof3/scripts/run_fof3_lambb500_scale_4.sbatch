#!/bin/bash
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=02:00:00
#SBATCH --job-name=fof3_lambb500_scale_4
#SBATCH --nodes=4
#SBATCH --ntasks=32
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --exclusive
#SBATCH --output=fof3_lambb500_scale_4.%j.out
#SBATCH --error=fof3_lambb500_scale_4.%j.err

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh


export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export PMI_MAX_KVS_ENTRIES=4194304
export FOF_COUNT_VERIFY=1

INPUT=/lustre/orion/csc710/scratch/rrao/lambb.00500
PPN=14
NDEVICES=7

cd /ccs/home/rrao/paratreet2/examples/fof3

srun --mpi=cray_shasta --network=job_vni --unbuffered \
  --cpu-bind=none --distribution=block:block \
  ./FoF3 -f "$INPUT" -d oct -u serial +ppn $PPN \
  +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
  +lci_ndevices $NDEVICES \
  +backend_poll_thread 2 \
  +traceoff
