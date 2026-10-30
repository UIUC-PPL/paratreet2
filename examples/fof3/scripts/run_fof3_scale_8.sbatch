#!/bin/bash
#SBATCH --account=csc710
#SBATCH --time=02:00:00
#SBATCH --job-name=fof3_scale_8
#SBATCH --nodes=8
#SBATCH --ntasks=64
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --exclusive
#SBATCH --output=fof3_scale_8.%j.out
#SBATCH --error=fof3_scale_8.%j.err

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh


# CXI multi-process-per-node fixes (see conversation history / run_fof3_2B.sbatch for detail):
#  job_vni + cray_shasta PMI plugin; LCI_ATTR_BACKEND=ofi + FI_CXI_RX_MATCH_MODE=hybrid
#  (CXI hardware match-table exhaustion at 8 procs/node); PMI_MAX_KVS_ENTRIES bumped for
#  the largest sweep point (128 nodes x 8 procs x 7 lci_ndevices); +backend_poll_thread 2
#  (network-completion abort at 8 procs/node); Paratreet.h setConfiguration mutex (data
#  race double-freeing Configuration under group-broadcast receiveConfiguration).
export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export PMI_MAX_KVS_ENTRIES=4194304

INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
PPN=14
NDEVICES=7

cd /ccs/home/rrao/paratreet2/examples/fof3

srun --mpi=cray_shasta --network=job_vni --unbuffered \
  --cpu-bind=none --distribution=block:block \
  ./FoF3 -f "$INPUT" -d oct -u dist +noisomalloc +ppn $PPN \
  +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
  +lci_ndevices $NDEVICES \
  +backend_poll_thread 2 \
  +traceoff
