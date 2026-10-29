#!/bin/bash
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=02:00:00
#SBATCH --job-name=fof3_labeling_ab
#SBATCH --nodes=16
#SBATCH --ntasks=128
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --exclusive
#SBATCH --output=fof3_labeling_ab.%j.out
#SBATCH --error=fof3_labeling_ab.%j.err

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh


export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export PMI_MAX_KVS_ENTRIES=4194304

INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
PPN=14
NDEVICES=7
PEMAP=1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127

cd /ccs/home/rrao/paratreet2/examples/fof3

run_rep() {
  local arm=$1 bin=$2 rep=$3
  echo "=== ARM=$arm REP=$rep BIN=$bin ==="
  srun --mpi=cray_shasta --network=job_vni --unbuffered \
    --cpu-bind=none --distribution=block:block \
    "$bin" -f "$INPUT" -d oct -u dist +ppn $PPN \
    +pemap $PEMAP \
    +lci_ndevices $NDEVICES \
    +backend_poll_thread 2 \
    +traceoff
  echo "=== END ARM=$arm REP=$rep ==="
}

# Interleaved, 3 reps each arm.
for rep in 1 2 3; do
  run_rep labelbase  ab_bin/FoF3.labelbase  $rep
  run_rep batchlabel ab_bin/FoF3.batchlabel $rep
done
