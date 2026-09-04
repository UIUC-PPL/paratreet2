#!/bin/bash
# FoF3 at 2B particles, 16 nodes, one PE per physical core, EIGHT
# processes per node -- one process per GCD.
#
# VARIANT of run_fof3_nosmt_8p.sbatch: +lci_ndevices 7 (== ppn) and NO
# +backend_poll_thread, against that script's 4 devices + stride 2.
# Everything else is byte-identical, so the delta is attributable.
# See the NDEVICES block below for why the two changes are a pair.
#
# WHY THIS SHAPE, vs job 5295318 (4 proc x 14 PE, also no SMT). Same 896
# PEs on the same 896 physical cores; the only thing that moves is where
# the process boundaries fall. That matters for two reasons:
#
#  (1) GPUs. Frontier has 8 GCDs per node. At 4 processes HAPI computes
#      device_count = visible / procs_per_physical_node = 8/4 = 2
#      (hapi_impl.cpp ~line 571) and round-robins PEs across BOTH, while
#      FoF initializes Kokkos on exactly one -- the home PE's
#      (FoFPhase1.h deviceInitOnHome). Job 5295318 only avoided that by
#      pinning ROCR_VISIBLE_DEVICES to one GCD per process, which left
#      four of the node's eight GCDs idle. Eight processes is the shape
#      where one process : one GCD is the natural arrangement rather
#      than something the wrapper has to force.
#  (2) It matches the production 8-proc/node shape every earlier 2B
#      result was taken in (run_fof3_2B.sbatch, run_fof3_scale_gpu.sbatch),
#      so the process count is no longer a difference to explain away --
#      only SMT is.
#
# Two arms, in this order:
#   notrace   +traceoff. The timing number. Tracing is linked in
#             (PROJECTIONS=1) but off, exactly as the scaling sweep runs
#             it, so the wall clock here is comparable to that table.
#   traced    full Projections logs. Perturbed by definition -- read it
#             for WHERE time goes, never for HOW MUCH.
# Untraced first so a trace-writing failure cannot cost the timing.
#
# -i 1, which is fof3's own default (Main.C: conf.num_iterations = 1).
# FoF is static analysis -- perturb_particles is false, nothing moves, and
# a second iteration rebuilds the same tree over the same particles and
# recomputes the same labels (verified: 3549 components both iterations on
# job 5295675). It bought one thing, a warm pass to subtract from a cold
# one, and the phase1_devinit line added to FoF3.C now measures that
# directly. So the second pass is pure cost, and iteration 0 is what a
# one-shot analysis run actually pays.
#SBATCH --account=csc710
# debug QOS turns a multi-hour queue wait into minutes. The cost is that
# only ONE debug job can be active at a time, so these must be submitted
# serially -- and both arms live in this one script for exactly that
# reason.
#SBATCH --qos=debug
#SBATCH --job-name=fof3_nosmt_8p_nd7
#SBATCH --nodes=16
#SBATCH --time=01:30:00
#SBATCH --ntasks-per-node=8
# 8 x 14 = 112 = every PU in the job cpuset. With --threads-per-core=2
# Slurm hands each task 7 cores plus their 7 SMT siblings, and 56 usable
# cores x 2 = 112, so the allocation is exactly cores 1-7,9-15,...,57-63
# and PUs 65-71,...,121-127. Two consequences, both wanted:
#  - every pemap entry below 64 is certainly inside the cpuset (a pemap
#    entry outside it is a hard abort, not a warning);
#  - the SMT siblings are in the cpuset with NO PE on them, which is
#    where the unpinned ROCm helper threads (ROCr signal/interrupt, the
#    stream callback thread) land. Kokkos here is HIP + Serial with no
#    OpenMP pool, so that is a handful of threads, not a storm.
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=fof3_nosmt_8p_nd7.%j.out
#SBATCH --error=fof3_nosmt_8p_nd7.%j.err

set -x

# LCI PROVENANCE. As of 2026-08-19 this stack runs on AUTOFETCHED upstream
# LCI (uiuc-hpc/lci) pinned at ca88ce2c, NOT the local fork in
# charm_reconverse/lci. The bump from a892d473 brings in two OFI/CXI
# commits -- #187 abortive teardown after network failure, #190 CXI
# write-with-immediate -- so any delta against job 5303289 belongs to
# those two or to nothing. charm_reconverse/lci/lib64 was removed from
# LD_LIBRARY_PATH below: it held a differently-built liblci/liblct that
# sat ahead of the ones this binary was compiled against.
echo "LCI revision built into charm:"
git -C /ccs/home/rrao/charm_reconverse/reconverse-linux-x86_64-amd/_deps/lci-src \
  log -1 --format='  %H %ci %s' 2>&1

module load craype-x86-trento libfabric/2.3.1 craype-network-ofi \
  perftools-base/24.11.0 xpmem/1.0.1-1.5_1_gfb6998056825 cray-pmi/6.1.15 \
  Core/25.03 tmux/3.4 hsi/default lfs-wrapper/0.0.1 DefApps hwloc/2.11.1 \
  gcc-native/13.2 craype/2.7.33 cray-dsmml/0.3.0 cray-mpich/8.1.31 \
  cray-libsci/24.11.0 PrgEnv-gnu/8.6.0 rocm/6.2.4 cmake/3.30.5 \
  python/3.13.0 darshan-runtime/3.4.6-mpi

export LD_LIBRARY_PATH=/ccs/home/rrao/charm_reconverse/lib:/sw/frontier/spack-envs/cpe24.11-cpu/opt/gcc-13.2/darshan-runtime-3.4.6-evo63a663bvwadj3dgv5vwqgwupxnh43/lib:/sw/frontier/spack-envs/core-25.03/opt/gcc-13.2/python-3.13.0-z6cvwh43maa5kvglquodiadny7ohzirp/lib:/opt/rocm-6.2.4/lib/roctracer:/opt/rocm-6.2.4/lib/rocprofiler:/opt/rocm-6.2.4/lib:/sw/frontier/spack-envs/core-25.03/opt/gcc-13.2/hwloc-2.11.1-daogniiyv5buyimip5lfgv6i6zawodef/lib:/opt/cray/pe/papi/7.1.0.4/lib64:/opt/cray/libfabric/2.3.1/lib64

# job_vni (NOT single_node_vni, which is 1-physical-node only) + the
# cray_shasta PMI plugin; FI_CXI_RX_MATCH_MODE=hybrid because at 8
# procs/node the CXI hardware match table is exhausted without it;
# PMI_MAX_KVS_ENTRIES bumped because +lci_ndevices publishes a per-device
# address for every rank.
export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
export PMI_MAX_KVS_ENTRIES=4194304

# The device arm. Both are needed: the flat tree is emitted at TREE BUILD,
# so FOF_GPU_PHASE1 alone aborts in deviceLaunch.
export PARATREET_DEVICE_TREE=1
export FOF_GPU_PHASE1=1

cd /ccs/home/rrao/paratreet2/examples/fof3

INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
PPN=7
# 7 == PPN: one LCI device per PE, the maximum this shape can ask for.
# 8 procs x 7 devices = 56 contexts/node, the ceiling that is known to fit
# on the NIC.
#
# +backend_poll_thread is DELIBERATELY UNSET here, and the two go together.
# CommBackendLCI2::initThread (comm_backend_lci2.cpp:259) assigns thread t
# to device t / ceil(ppn/ndevices); at ppn == ndevices that is one thread
# per device, thread t owning device t. scheduler.cpp:189 then silences
# every rank that is not a multiple of +backend_poll_thread, so passing 2
# here would leave devices 1, 3 and 5 with no poller at all -- the exact
# permanent-hole failure the comment above progress() names, and the way
# job 5302111 hung in init for 8 minutes with no error. Unset,
# backend_poll_thread defaults to 1 (convcore.cpp:360, "default to every
# thread"), every rank polls, and all 7 devices are covered by their own
# thread. So this arm is only safe *because* the stride is gone.
#
# vs the 4-device baseline (jobs 5303289 / 5307458): that one had to give
# up devices to keep 2 threads per device so an even-rank stride still
# reached all of them. This trades the stride away for maximum device
# parallelism instead. Both cover every device; the question is whether 7
# private devices beat 4 shared ones, or whether the extra 24
# contexts/node cost more in NIC resources than the parallelism returns.
NDEVICES=7
# 56 entries = 8 processes x 7 PEs, no PU above 63, no multiple of 8
# (core 0 of each L3 group is outside the job cpuset on Frontier).
# search_pemap() in reconverse/src/cpuaffinity.cpp indexes this by
# CmiMyPeGlobal() % 56, and block:block gives the eight processes on a
# node consecutive global PE blocks, so local rank r takes entries
# 7r..7r+6 -- one contiguous 7-core slice each, inside a single L3 group.
# The entry count MUST equal procs_per_node x ppn or processes silently
# overlap onto the same cores.
PEMAP=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63

TRACEBASE=/lustre/orion/csc710/scratch/rrao/fof3_traces/nosmt8p_nd7_$SLURM_JOB_ID
mkdir -p "$TRACEBASE" || exit 1

# One GCD per process, NUMA-local to that process's 7 cores.
#
# Without this, HAPI's RoundRobin gives local rank r the device with
# global id (device_count * r) % 8 = r (hapi_impl.cpp ~line 602), i.e.
# rank 0 -> GCD 0. But Frontier's core-to-GCD affinity is
#   cores 0-7 -> 4, 8-15 -> 5, 16-23 -> 2, 24-31 -> 3,
#   cores 32-39 -> 6, 40-47 -> 7, 48-55 -> 0, 56-63 -> 1
# so rank 0's cores 1-7 are local to GCD 4, not 0. Assigning by rank
# would put every host-device copy on this node across the Infinity
# Fabric for no reason. Restricting visibility to one GCD also puts HAPI
# on the visible_device_count == 1 path, so device_count == 1 and
# Device::checkMapping takes its n_visible == 1 branch -- the same
# configuration every prior trace was taken under.
#
# This MUST be a wrapper script, not an export above: --gpus-per-node=8
# makes Slurm set ROCR_VISIBLE_DEVICES in the task environment itself,
# which overrides anything the batch script exported. The wrapper runs
# inside the task, after that, so it wins.
WRAP=$TRACEBASE/gcd_wrap.sh
cat > "$WRAP" <<'EOF'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOF
chmod +x "$WRAP"

# run <label> <trace: 0|1>
run() {
  local label=$1 trace=$2
  local targs="+traceoff"
  if [ "$trace" = "1" ]; then
    local troot="$TRACEBASE/$label"
    # Projections does NOT create traceroot and a missing one loses the
    # whole run's traces silently.
    mkdir -p "$troot" || exit 1
    # +logsize is a per-PE entry ceiling LogPool reserve()s up front; it
    # changes buffering, never trace content. Measured usage is ~1.5 MB
    # compressed per PE, so 20M is ample headroom.
    targs="+traceroot $troot +logsize 20000000"
  fi
  echo "=== $label (16n x 8proc x 7PE = 896 PEs, 7 LCI devices/proc, no poll stride, trace=$trace) ==="
  date
  srun --mpi=cray_shasta --network=job_vni --unbuffered \
    --cpu-bind=none --distribution=block:block \
    --ntasks=128 --gpus-per-node=8 --export=ALL \
    "$WRAP" ./FoF3 -f "$INPUT" -d oct -u dist -c stats -l 128 -i 1 \
    +ppn $PPN +pemap $PEMAP \
    +lci_ndevices $NDEVICES $targs
  echo "exit: $?"
  date
  [ "$trace" = "1" ] && du -sh "$TRACEBASE/$label" 2>/dev/null
  return 0
}

run notrace 0
run traced  1

ls "$TRACEBASE"/traced/*.sts 2>/dev/null
echo "traces in $TRACEBASE/traced"
