#!/bin/bash
# 2B-particle node-count scaling for the DEVICE phase-1 path
# (design/phase1-gpu.md section 18, results in design/fof3-2b-scaling.md).
#
# The companion of run_fof3_scale_{8,16,32,64,128}.sbatch, which measured
# the CPU chain over the same sweep on 2026-08-10. Those five scripts are
# five near-identical files; this is ONE file parameterised by the node
# count, because it has to stay identical across the sweep for the sweep
# to mean anything -- five copies is five chances for one point to drift.
# Submit it with scripts/submit_scale_gpu.sh, which is the record of what
# was run.
#
# Every node count runs three arms so the comparison is same-build,
# same-day, same-input:
#   cpu_l12       the CPU chain (PARATREET_DEVICE_TREE=0: no flat-tree
#                 emit either, so it is a genuinely device-free build path)
#   replace_l12   the device path on the SAME tree -- apples to apples
#   replace_l128  the device path on the tree it actually wants
#                 (design/phase1-gpu.md section 16: the device prefers the
#                 coarse tree the CPU is punished by)
#
# -i 2 EVERYWHERE, and both iterations are reported. Iteration 0 of a
# device arm additionally pays one-time Kokkos/HIP init and the first
# pinned allocation inside phase 1 -- measured at 1.365 s vs 0.657 s
# steady state at 2B/16 nodes -- so a single-iteration sweep would report
# the device path's startup as its per-iteration cost. The prior CPU sweep
# reported iteration 0; its rows are reproduced here for that reason.
#
# TRACING: the binary is linked with -tracemode projections (make
# PROJECTIONS=1) at every node count, and every arm runs +traceoff EXCEPT
# the 16-node job, which adds two traced arms. That is exactly the
# convention the CPU sweep used, so the tracing-linked-but-off overhead is
# the same on both sides of the comparison. It is NOT the same as the
# untraced binary section 18.7 measured with, so small differences against
# that table are expected and are the tracing hooks, not a regression.
#SBATCH --account=csc710
#SBATCH --job-name=fof3_scale_gpu
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=fof3_scale_gpu.%j.out
#SBATCH --error=fof3_scale_gpu.%j.err

set -x

module load craype-x86-trento libfabric/2.3.1 craype-network-ofi \
  perftools-base/24.11.0 xpmem/1.0.1-1.5_1_gfb6998056825 cray-pmi/6.1.15 \
  Core/25.03 tmux/3.4 hsi/default lfs-wrapper/0.0.1 DefApps hwloc/2.11.1 \
  gcc-native/13.2 craype/2.7.33 cray-dsmml/0.3.0 cray-mpich/8.1.31 \
  cray-libsci/24.11.0 PrgEnv-gnu/8.6.0 rocm/6.2.4 cmake/3.30.5 \
  python/3.13.0 darshan-runtime/3.4.6-mpi

# rocm on the path is the GPU arm's addition to the CPU sweep's
# environment; charm and Kokkos are both built against 6.2.4 and a
# mismatch shows up as an undefined hsa_* symbol at load
# (design/phase1-gpu.md section 11.3 trap 8).
export LD_LIBRARY_PATH=/ccs/home/rrao/charm_reconverse/lib:/sw/frontier/spack-envs/cpe24.11-cpu/opt/gcc-13.2/darshan-runtime-3.4.6-evo63a663bvwadj3dgv5vwqgwupxnh43/lib:/sw/frontier/spack-envs/core-25.03/opt/gcc-13.2/python-3.13.0-z6cvwh43maa5kvglquodiadny7ohzirp/lib:/opt/rocm-6.2.4/lib/roctracer:/opt/rocm-6.2.4/lib/rocprofiler:/opt/rocm-6.2.4/lib:/sw/frontier/spack-envs/core-25.03/opt/gcc-13.2/hwloc-2.11.1-daogniiyv5buyimip5lfgv6i6zawodef/lib:/opt/cray/pe/papi/7.1.0.4/lib64:/opt/cray/libfabric/2.3.1/lib64

# Unchanged from the CPU sweep: job_vni + cray_shasta PMI plugin,
# LCI_ATTR_BACKEND=ofi + FI_CXI_RX_MATCH_MODE=hybrid (CXI hardware
# match-table exhaustion at 8 procs/node), PMI_MAX_KVS_ENTRIES bumped for
# the 128-node point, +backend_poll_thread 2.
export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
export PMI_MAX_KVS_ENTRIES=4194304

# FOF_COUNT_VERIFY is deliberately NOT set. It rehashes every particle in
# depositLabelCounts, which lands squarely in the component-histogram
# column this sweep is measuring. Correctness at each node count is
# carried by the component count being identical to every prior sweep
# (the substitute-for-brute-force check this dataset has always used), and
# the 16-node job adds a verify arm that turns the full per-particle gate
# back on.

INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
PPN=14
NDEVICES=7
PEMAP=1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127

N=$SLURM_JOB_NUM_NODES
NT=$((N * 8))
TRACEBASE=/lustre/orion/csc710/scratch/rrao/fof3_traces/scale_gpu_$SLURM_JOB_ID

cd /ccs/home/rrao/paratreet2/examples/fof3

# run <label> <mode> <leaf> <iters> <trace: 0|1>
run() {
  local label=$1 mode=$2 leaf=$3 iters=$4 trace=$5
  local e=""
  case "$mode" in
    replace) e="PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1" ;;
    verify)  e="PARATREET_DEVICE_TREE=1 FOF_GPU_VERIFY=1 FOF_COUNT_VERIFY=1" ;;
    off)     e="PARATREET_DEVICE_TREE=0" ;;
  esac
  local targs="+traceoff"
  if [ "$trace" = "1" ]; then
    local troot="$TRACEBASE/$label"
    # Projections does NOT create the traceroot and a missing one is
    # silently fatal to the whole arm (design/phase1-gpu.md trap 16's
    # neighbour). Create it before the run, not after.
    mkdir -p "$troot" || exit 1
    # +logsize is a per-PE entry ceiling that LogPool reserve()s up front.
    # The CPU sweep used 100000000; at 112 PEs per node that reserves
    # ~900 GB of address space per node against 512 GB of RAM, which
    # survives only because reserve() commits nothing until written.
    # Measured usage is ~1.5 MB compressed per PE, so 20M entries is two
    # orders of magnitude of headroom. logsize changes buffering, never
    # trace CONTENT.
    targs="+traceroot $troot +logsize 20000000"
  fi
  echo "=== $label (nodes=$N ntasks=$NT mode=$mode leaf=$leaf iters=$iters trace=$trace) ==="
  date
  env $e \
    srun --mpi=cray_shasta --network=job_vni --unbuffered \
    --cpu-bind=none --distribution=block:block \
    --ntasks=$NT --gpus-per-node=8 \
    ./FoF3 -f "$INPUT" -d oct -u dist -c stats -l $leaf -i $iters \
    +ppn $PPN +pemap $PEMAP \
    +lci_ndevices $NDEVICES +backend_poll_thread 2 $targs
  echo "exit: $?"
  date
  [ "$trace" = "1" ] && du -sh "$TRACEBASE/$label" 2>/dev/null
  return 0
}

# The sweep proper: untraced at every node count.
run cpu_l12      off     12  2 0
run replace_l128 replace 128 2 0
run replace_l12  replace 12  2 0

# The 16-node job carries the traces and the gate. Both arms are traced,
# not just the device one: the open question these traces exist to answer
# is section 18.9's phase-3 regression, and a single timeline cannot show
# a difference -- it needs the device-free timeline to diff against.
if [ "$N" = "16" ]; then
  run verify_l12        verify  12  1 0
  run traced_cpu_l12    off     12  1 1
  run traced_replace_l128 replace 128 1 1
fi
