#!/bin/bash
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=02:00:00
#SBATCH --job-name=fof3_lambb500_scale_16
#SBATCH --nodes=16
#SBATCH --ntasks=128
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --exclusive
#SBATCH --output=fof3_lambb500_scale_16.%j.out
#SBATCH --error=fof3_lambb500_scale_16.%j.err

module load craype-x86-trento libfabric/2.3.1 craype-network-ofi perftools-base/24.11.0 \
  xpmem/1.0.1-1.5_1_gfb6998056825 cray-pmi/6.1.15 Core/25.03 tmux/3.4 hsi/default \
  lfs-wrapper/0.0.1 DefApps cmake/3.30.5 hwloc/2.11.1 gcc-native/13.2 craype/2.7.33 \
  cray-dsmml/0.3.0 cray-mpich/8.1.31 cray-libsci/24.11.0 PrgEnv-gnu/8.6.0 \
  darshan-runtime/3.4.6-mpi

export LD_LIBRARY_PATH=/ccs/home/rrao/charm_reconverse/lib:/ccs/home/rrao/charm_reconverse/lci/lib64:/sw/frontier/spack-envs/cpe24.11-cpu/opt/gcc-13.2/darshan-runtime-3.4.6-evo63a663bvwadj3dgv5vwqgwupxnh43/lib:/sw/frontier/spack-envs/core-25.03/opt/gcc-13.2/hwloc-2.11.1-daogniiyv5buyimip5lfgv6i6zawodef/lib:/opt/cray/pe/papi/7.1.0.4/lib64:/opt/cray/libfabric/2.3.1/lib64

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
