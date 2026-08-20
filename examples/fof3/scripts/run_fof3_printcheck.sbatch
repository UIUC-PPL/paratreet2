#!/bin/bash
# Post-removal check: FoF3 still runs with the GPU arm ON and neither the
# [FOF3GPU] nor the [CCDSTATS] prints appear.
#
# Both env switches that used to make those prints appear are set on
# purpose. RECONVERSE_CCD_STATS=1 with a 0 ms threshold would previously
# have dumped three lines per PE from ConverseExit(); PARATREET_DEVICE_TREE
# + FOF_GPU_PHASE1 select the device arm, which is the only path that
# printed [FOF3GPU] at all. A CPU-only run would prove nothing.
#
# Single PHYSICAL node, so --network=single_node_vni is required (and must
# be omitted for multi-node runs).
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --job-name=fof3_printcheck
#SBATCH --nodes=1
#SBATCH --time=00:15:00
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=28
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=fof3_printcheck.%j.out
#SBATCH --error=fof3_printcheck.%j.err

set -x

module load craype-x86-trento libfabric/2.3.1 craype-network-ofi \
  perftools-base/24.11.0 xpmem/1.0.1-1.5_1_gfb6998056825 cray-pmi/6.1.15 \
  Core/25.03 tmux/3.4 hsi/default lfs-wrapper/0.0.1 DefApps hwloc/2.11.1 \
  gcc-native/13.2 craype/2.7.33 cray-dsmml/0.3.0 cray-mpich/8.1.31 \
  cray-libsci/24.11.0 PrgEnv-gnu/8.6.0 rocm/6.2.4 cmake/3.30.5 \
  python/3.13.0 darshan-runtime/3.4.6-mpi

export LD_LIBRARY_PATH=/ccs/home/rrao/charm_reconverse/lib:/sw/frontier/spack-envs/cpe24.11-cpu/opt/gcc-13.2/darshan-runtime-3.4.6-evo63a663bvwadj3dgv5vwqgwupxnh43/lib:/sw/frontier/spack-envs/core-25.03/opt/gcc-13.2/python-3.13.0-z6cvwh43maa5kvglquodiadny7ohzirp/lib:/opt/rocm-6.2.4/lib/roctracer:/opt/rocm-6.2.4/lib/rocprofiler:/opt/rocm-6.2.4/lib:/sw/frontier/spack-envs/core-25.03/opt/gcc-13.2/hwloc-2.11.1-daogniiyv5buyimip5lfgv6i6zawodef/lib:/opt/cray/pe/papi/7.1.0.4/lib64:/opt/cray/libfabric/2.3.1/lib64

export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd

# The instrumentation switch whose dump was removed. 0 ms threshold, i.e.
# report everything, so a surviving print cannot hide under a filter.
export RECONVERSE_CCD_STATS=1
export RECONVERSE_CCD_STATS_MS=0

# The device arm. Both are needed: the flat tree is emitted at TREE BUILD,
# so FOF_GPU_PHASE1 alone would abort in deviceLaunch.
export PARATREET_DEVICE_TREE=1
export FOF_GPU_PHASE1=1

# One GCD for the single process, so HAPI's device_count is 1 and the
# n_visible == 1 branch of checkMapping applies.
export ROCR_VISIBLE_DEVICES=4

cd /ccs/home/rrao/paratreet2/examples/fof3

srun --mpi=cray_shasta --network=single_node_vni --unbuffered \
  --cpu-bind=none --ntasks=1 --gpus-per-node=8 --export=ALL \
  ./FoF3 -f ../../inputs/10k.tipsy -d oct -u dist -c stats -l 32 -i 1 \
  +ppn 14 +pemap 1-7,9-15 +traceoff
echo "exit: $?"
