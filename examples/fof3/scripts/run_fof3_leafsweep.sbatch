#!/bin/bash
# Whole-APPLICATION leaf-size sweep on the device phase-1 path, 2B
# particles (design/fof3-2b-scaling.md section 10).
#
# Why this exists. design/phase1-gpu.md section 16.2 swept leaf size
# against the DEVICE PASS and found a broad 96-128 basin; section 16.3
# then noted, in as many words, that "the whole-application cost of a
# leaf-128 tree (build time and phase 3 behaviour) has not been
# measured, only phase 1's". It still has not. Every measurement since
# has compared exactly two points, l12 and l128, and those two points
# disagree about which is better depending on node count
# (design/fof3-2b-scaling.md section 4: l128 wins the iteration at 8, 16,
# 32 and 128 nodes and loses at 64). Two points cannot show a basin, a
# crossover, or an interior optimum, and the phase-3 walk penalty is not
# monotonic in leaf size across the node counts already measured
# (+0.40 s at 32 nodes, +0.11 s at 128).
#
# So: sweep `-l` across the whole application, at the two node counts
# that bracket the interesting behaviour (16 = where the downstream
# regression is measurable and the traces exist, 128 = where the
# application speedup collapsed to parity).
#
# This is deliberately the CHEAP experiment. It costs one job per node
# count and NO CODE CHANGE. The alternative it is meant to inform --
# decoupling the device's leaf size from the live tree's, by truncating
# the flat emit in src/DeviceTree.h -- is only worth building if the
# whole-iteration optimum really does sit at a different leaf size from
# the device pass's optimum. If some interior `-l` beats both l12 and
# l128 end to end, the decoupling buys nothing and this job says so.
#
# ONE PARAMETERISED SCRIPT, node count from --nodes on the command line,
# same convention as run_fof3_scale_gpu.sbatch. Copies drift; a sweep
# whose points drift measures nothing.
#
# ARMS. `cpu_l12` first as the anchor: it is the configuration
# design/fof3-2b-scaling.md section 9.1 reports for this exact build, so
# a reader can tie every number here back to that table and know the
# stack did not move underneath. Then the device path at eight leaf
# sizes, ascending.
#
# THE TWO REPEAT ARMS AT THE END ARE NOT PADDING. Section 9.2 found that
# at 128 nodes the between-run scatter is larger than the effect being
# measured -- two arms of one job moved in OPPOSITE directions by large
# margins -- and drew no conclusion, which was the right call. A sweep is
# a comparison of arms, so it inherits exactly that hazard. Re-running
# l128 and l12 at the END of the same job measures the drift directly:
# if the repeat matches its twin, differences across the sweep are the
# leaf size; if it does not, the spread between the repeats is the error
# bar and no interior point smaller than it can be believed. Running the
# check is cheap (2 arms); asserting the sweep is clean without it is
# not.
#
# `-i 2` and iteration 1 is the number, everywhere. Iteration 0 of a
# device arm pays one-time Kokkos/HIP init plus the first pinned
# allocation inside phase 1 (0.7-0.9 s at every node count), so reporting
# it would report startup as per-iteration cost.
#
# FOF_COUNT_VERIFY is deliberately OFF: it rehashes every particle inside
# depositLabelCounts, which lands in the component_histogram column this
# sweep reads. Correctness across leaf sizes is carried by the component
# count, which has been identical in every arm of every sweep including
# the l128/l12 pairs -- a different tree, different leaf sets, different
# kernel shape (section 16 auto-selection), same 424,897,832 components.
#
# Binary: the GPU=1 PROJECTIONS=1 build of 2026-08-14 11:46, the same one
# that produced jobs 5265073 (16n) and 5265322 (128n) post-`gate-nodelock`.
# Tracing is LINKED but every arm runs +traceoff, matching the sweep it
# is compared against so the linked-but-disabled overhead is identical on
# both sides.
#SBATCH --account=csc710
#SBATCH --job-name=fof3_leafsweep
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=fof3_leafsweep.%j.out
#SBATCH --error=fof3_leafsweep.%j.err

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

INPUT=/lustre/orion/csc710/scratch/rrao/cosmo25cmb.768g2_dm.001024
PPN=14
NDEVICES=7
PEMAP=1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127

N=$SLURM_JOB_NUM_NODES
NT=$((N * 8))

cd /ccs/home/rrao/paratreet2/examples/fof3

# Walltime guard (design/phase1-gpu.md trap 18). Measured arm cost on
# this build is ~22 s at 16 nodes and ~70 s at 128, so the requests below
# are several times the work -- but a queued job that gets killed in its
# last arm loses the repeat arms, which are the ones that make the rest
# interpretable. Skipping an arm cleanly beats being killed mid-arm.
JOB_START=$(date +%s)
LIMIT_S=$(( $(squeue -h -j "$SLURM_JOB_ID" -o "%l" | awk -F: '{if (NF==3) print ($1*3600)+($2*60)+$3; else print ($1*60)+$2}') ))
[ -n "$LIMIT_S" ] && [ "$LIMIT_S" -gt 0 ] || LIMIT_S=$((30*60))
NEED=$(( N >= 64 ? 180 : 90 ))
have_time() {
  local left=$(( LIMIT_S - ( $(date +%s) - JOB_START ) ))
  echo "walltime remaining: ${left}s (arm wants ${NEED}s)"
  [ "$left" -ge "$NEED" ]
}

# run <label> <mode: off|replace> <leaf>
run() {
  local label=$1 mode=$2 leaf=$3
  have_time || { echo "=== SKIP $label (insufficient walltime) ==="; return 0; }
  local e=""
  case "$mode" in
    replace) e="PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1" ;;
    off)     e="PARATREET_DEVICE_TREE=0" ;;
  esac
  echo "=== $label (nodes=$N ntasks=$NT mode=$mode leaf=$leaf) ==="
  date
  env $e \
    srun --mpi=cray_shasta --network=job_vni --unbuffered \
    --cpu-bind=none --distribution=block:block \
    --ntasks=$NT --gpus-per-node=8 \
    ./FoF3 -f "$INPUT" -d oct -u dist -c stats -l $leaf -i 2 \
    +ppn $PPN +pemap $PEMAP \
    +lci_ndevices $NDEVICES +backend_poll_thread 2 +traceoff
  echo "exit: $?"
  date
  return 0
}

# The anchor: ties this job's numbers to section 9.1's table.
run cpu_l12 off 12

# The sweep. Ascending, so a monotone trend is visible by eye in the log
# and a non-monotone one is not hidden by the ordering.
for L in 12 24 32 48 64 96 128 192; do
  run replace_l$L replace $L
done

# Drift check (see header). Same two configurations, same job, last.
run replace_l128_repeat replace 128
run replace_l12_repeat  replace 12
