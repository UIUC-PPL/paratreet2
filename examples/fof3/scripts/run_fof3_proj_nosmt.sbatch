#!/bin/bash
# Projections trace of FoF3 with every PE on its OWN PHYSICAL CORE.
#
# WHY THIS SHAPE. Every FoF3 trace so far was taken in the production
# shape: 8 processes x 14 PEs = 112 PE threads per node pinned across
#   +pemap 1-7,65-71,9-15,73-79,...
# where 65-71 are the SMT siblings of 1-7. Two PEs spinning in
# CsdScheduler on one core's two hyperthreads is the exact condition job
# 5294355 showed quiescence detection punishes out of all proportion:
# oversubscription cost raw message draining 20x but cost QD 1700x,
# because draining pays descheduling once per batch while QD needs EVERY
# PE to reach a still-idle iteration and pays a timeslice on each of
# ~6 x tree-depth sequential hops.
#
# So this run removes SMT from the picture entirely. 4 processes x 14 PEs
# = 56 PE threads per node, one per physical core, over PUs 0-63 only.
# Cores that are multiples of 8 (0, 8, 16, ...) are outside the job
# cpuset on Frontier, which is why the map reads 1-7,9-15,... and why 56
# is the whole supply.
#
# The question the trace answers is narrow: with SMT contention gone, do
# the long idle gaps before each quiescence callback still appear on the
# timeline, and if so what are the PEs doing instead of going idle? A
# wall-clock number cannot answer that; the timeline can.
#SBATCH --account=csc710
#SBATCH --job-name=fof3_proj_nosmt
#SBATCH --nodes=16
#SBATCH --time=00:50:00
#SBATCH --ntasks-per-node=4
# 28 = the 14 physical cores this process pins PEs to, PLUS their 14 SMT
# siblings, which no PE ever uses. That is deliberate on two counts.
# (1) It makes 4 x 28 = 112 = the whole node, so the job cgroup covers
#     every PU and a pemap entry as high as 63 is certain to be inside
#     it. Requesting only 4 x 14 = 56 would leave which 56 up to Slurm's
#     distribution, and a pemap referencing a PU outside the cpuset is a
#     hard abort.
# (2) It is where the ROCm threads go. HIP spawns its own helper threads
#     (ROCr signal/interrupt handlers, the stream callback thread) and
#     Kokkos here is HIP + Serial -- no OpenMP pool, checked in
#     ~/kokkos/include/KokkosCore_config.h -- so it is a handful of
#     threads, not a storm. They are unpinned, so leaving PUs 64-127 in
#     the cpuset and empty of PEs gives them somewhere to run that is not
#     a core a PE is spinning on.
#SBATCH --cpus-per-task=28
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=fof3_proj_nosmt.%j.out
#SBATCH --error=fof3_proj_nosmt.%j.err

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
export PMI_MAX_KVS_ENTRIES=4194304

# The runtime's own view of the same phenomenon, so the timeline has a
# number to be checked against: per-PE STILL_IDLE park delay, raise gap,
# and messages-arriving-during-park, dumped from ConverseExit(). Reports
# only parks over 10 ms. This costs a timestamp per CcdCallOnCondition
# registration -- a few thousand per run -- so it does not perturb what
# Projections is recording.
export RECONVERSE_CCD_STATS=1
export RECONVERSE_CCD_STATS_MS=10

export PARATREET_DEVICE_TREE=1
export FOF_GPU_PHASE1=1

cd /ccs/home/rrao/paratreet2/examples/fof3

INPUT=/lustre/orion/csc710/scratch/rrao/cosmo25cmb.768g2_dm.001024
PPN=14
NDEVICES=7
# 56 entries = 4 processes x 14 PEs, no PU above 63, no multiple of 8.
# search_pemap() in reconverse/src/cpuaffinity.cpp indexes this by
# CmiMyPeGlobal() % 56, and with block:block the four processes on a node
# own consecutive global PE blocks, so process local rank r takes entries
# 14r..14r+13 -- exactly one contiguous 14-core slice each. This only
# works because the entry count matches procs-per-node x ppn; a map sized
# for a different shape silently overlaps processes onto the same cores,
# which is a fabricated 350 ms "QD stall" all by itself.
PEMAP=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63

TROOT=/lustre/orion/csc710/scratch/rrao/fof3_traces/proj_nosmt_$SLURM_JOB_ID
# Projections does not create traceroot and a missing one loses the whole
# run's traces silently.
mkdir -p "$TROOT" || exit 1

# One GCD per process, chosen NUMA-local to that process's cores.
#
# This is NOT cosmetic. HAPI computes device_count as
# visible_devices / procs_per_physical_node (hapi_impl.cpp ~line 571), so
# 8 GCDs / 4 processes hands each process TWO devices and round-robins
# PEs across both -- while FoF initializes Kokkos on exactly one device,
# the home PE's (FoFPhase1.h deviceInitOnHome). Every previous run had
# 8 processes and therefore device_count == 1; this shape would be the
# first to take the two-device path, with PEs whose current HIP device is
# not the one Kokkos was initialized on. Pinning visibility to a single
# GCD restores device_count == 1 and the n_visible == 1 branch of
# Device::checkMapping, i.e. exactly the configuration every prior trace
# was taken under. Ratio is preserved too: 14 PEs per GCD, same as the
# 8-process shape.
#
# The GCD choice follows Frontier's core-to-GCD affinity table:
#   cores 0-7 -> 4, 8-15 -> 5, 16-23 -> 2, 24-31 -> 3,
#   cores 32-39 -> 6, 40-47 -> 7, 48-55 -> 0, 56-63 -> 1.
# Local rank 0 holds cores 1-15 (GCDs 4,5) -> 4; rank 1 holds 17-31
# (2,3) -> 2; rank 2 holds 33-47 (6,7) -> 6; rank 3 holds 49-63
# (0,1) -> 0.
WRAP=$TROOT/gcd_wrap.sh
cat > "$WRAP" <<'EOF'
#!/bin/bash
GCDS=(4 2 6 0)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOF
chmod +x "$WRAP"

echo "=== fof3 projections, 16n x 4proc x 14PE = 896 PEs, one PE per physical core ==="
date

# +logsize is a per-PE entry ceiling LogPool reserve()s up front; it
# changes buffering, never trace content. Measured usage is ~1.5 MB
# compressed per PE, so 20M entries is ample.
srun --mpi=cray_shasta --network=job_vni --unbuffered \
  --cpu-bind=none --distribution=block:block \
  --ntasks=64 --gpus-per-node=8 --export=ALL \
  "$WRAP" ./FoF3 -f "$INPUT" -d oct -u dist -c stats -l 128 -i 2 \
  +ppn $PPN +pemap $PEMAP \
  +lci_ndevices $NDEVICES +backend_poll_thread 2 \
  +traceroot "$TROOT" +logsize 20000000
echo "exit: $?"
date

ls "$TROOT"/*.sts 2>/dev/null
du -sh "$TROOT"
echo "traces in $TROOT"
