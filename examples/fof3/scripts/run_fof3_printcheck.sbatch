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

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh


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
