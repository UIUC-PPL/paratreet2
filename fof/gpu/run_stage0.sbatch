#!/bin/bash
# Stage 0 gate (design/phase1-gpu.md section 8): the standalone device
# round trip, then the Charm++/Kokkos/HAPI integration spike at the
# production geometry (8 processes/node, one per GCD, +ppn 14).
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:20:00
#SBATCH --job-name=fof_gpu_stage0
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=stage0.%j.out
#SBATCH --error=stage0.%j.err

set -x
cd "$SLURM_SUBMIT_DIR"

# The module set this stack is built and run against. rocm/6.2.4 is the
# one Kokkos was built with; note charm's HIP support was configured
# against /opt/rocm-default (6.4.2) — see the ALLOW_UNDEF comment in
# tests/kokkos-spike/Makefile.
module load craype-x86-trento libfabric/2.3.1 craype-network-ofi \
  perftools-base/24.11.0 xpmem/1.0.1-1.5_1_gfb6998056825 cray-pmi/6.1.15 \
  Core/25.03 tmux/3.4 hsi/default lfs-wrapper/0.0.1 DefApps hwloc/2.11.1 \
  gcc-native/13.2 craype/2.7.33 cray-dsmml/0.3.0 cray-mpich/8.1.31 \
  cray-libsci/24.11.0 PrgEnv-gnu/8.6.0 rocm/6.2.4 cmake/3.30.5 \
  python/3.13.0 darshan-runtime/3.4.6-mpi

export LD_LIBRARY_PATH=/ccs/home/rrao/charm_reconverse/lib:$LD_LIBRARY_PATH
export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
export PMI_MAX_KVS_ENTRIES=4194304

echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"

echo "=== 1. standalone device gate (no Charm, no HAPI) ==="
srun --ntasks=1 --gpus-per-task=1 ./fof-device-test 4000000
echo "standalone exit: $?"

echo "=== 2. Charm++ / Kokkos / HAPI integration spike ==="
SPIKE=../../tests/kokkos-spike/kokkos-spike
if [ -x "$SPIKE" ]; then
  srun --mpi=cray_shasta --network=single_node_vni --unbuffered \
       --cpu-bind=none --distribution=block:block \
       --ntasks=8 --gpus-per-node=8 \
       $SPIKE 4000000 +ppn 14 \
       +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
      
  echo "spike exit: $?"
else
  echo "SKIP: $SPIKE not built"
fi
