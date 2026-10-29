#!/bin/bash
# Node-count scaling for the two large NChilada snapshots, 8 -> 128 nodes.
# ONE file parameterised by --nodes and DSET, so every point in the sweep
# runs byte-identical code and shape -- the same discipline as
# run_fof3_scale2b_gpu.sbatch, which this is derived from.
#
# Env in, via sbatch --export:  DSET NODES_LABEL PPN LEAF NDEV POLL ITERS
#
# Two questions are being answered at once:
#   1. performance: **Total Decomposition time / Iteration 1 time / the
#      FOF3STAT time_s breakdowns, per node count;
#   2. the minimum node count at which the snapshot FITS. A point that
#      does not fit is a RESULT, not a failure, so an OOM is detected and
#      labelled rather than left to look like a crash.
#
# Shape is the 2026-08-21 swept-best 2B config: 8 processes/node (one per
# GCD), ppn 7 (one PE per physical core, no SMT), 7 LCI devices per
# process, poll thread 1, leaf 128, device phase 1.
#SBATCH --account=csc710
#SBATCH --job-name=cmpfof3sum
#SBATCH --time=02:00:00
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err

module load craype-x86-trento libfabric/2.3.1 craype-network-ofi \
  perftools-base/24.11.0 xpmem/1.0.1-1.5_1_gfb6998056825 cray-pmi/6.1.15 \
  Core/25.03 gcc-native/13.2 craype/2.7.33 cray-dsmml/0.3.0 \
  cray-mpich/8.1.31 cray-libsci/24.11.0 PrgEnv-gnu/8.6.0 rocm/6.2.4
# hwloc/2.11.1, cmake/3.30.5 and python/3.13.0 were RETIRED from Frontier
# after the 2026-09 campaign. Lmod aborts the whole compound load when any
# one member is missing, so leaving them here silently prevents rocm from
# loading and every rank dies at "libamdhip64.so.6: cannot open shared
# object file" (jobs 5512027-34). hwloc is not needed: the binary's RPATH
# still resolves libhwloc.so.15 from the spack tree on disk.
[ -n "${ROCM_PATH:-}" ] || { echo "### ABORT: rocm module did not load"; exit 3; }
echo "### rocm=$ROCM_PATH"

# CHARMB MUST match whichever binary BIN selects. liblci.so is resolved by
# SONAME through LD_LIBRARY_PATH, NOT by the path baked into the executable,
# so pointing BIN at one build while LD_LIBRARY_PATH points at another runs
# the WRONG shared library silently. That is exactly what happened on
# 2026-08-24: FoF3.lciag (patched LCI) ran against the -lcw build's
# unpatched liblci.so and reproduced the unpatched behaviour perfectly,
# with no error of any kind. It was caught only because LCI_LOG_LEVEL=info
# printed "Bootstrap round 0 with LCT PMI" instead of the patched build's
# "Bootstrap allgather round 0 ...".
#   -amd-lcw   : LCIv2 (upstream) + LCW/MPI      -> FoF3.lcw.*
#   -amd-lciag : LCIv2 (allgather patch) + LCW   -> FoF3.lciag
: ${CHARMB:=reconverse-linux-x86_64-amd-cmp}
export CHARM_HOME=/ccs/home/rrao/charm_reconverse/$CHARMB
[ -d "$CHARM_HOME/lib" ] || { echo "FATAL: no such charm build $CHARM_HOME" >&2; exit 1; }
export LD_LIBRARY_PATH=$CHARM_HOME/lib:$LD_LIBRARY_PATH
export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd

: ${PPN:=7}
: ${LEAF:=128}
: ${NDEV:=7}
: ${POLL:=1}
: ${ITERS:=1}
: ${DSET:?DSET must be cosmo25 or romulus25}
# Tuning knobs (all unset = the historical shape, so old points reproduce).
#   SCAP  -> -s N  cap the tree-canopy starter pack. Unset, the pack is
#            broadcast to every process at O(P^2) cost. Exact at any value.
#   DEPTH -> -D N  cache share depth (default 3). Lower = smaller reply
#            bundles per node request = less cached particle volume.
#   UFSIZES=0 -> FOF_UF_SIZES=0, skipping union-find size bookkeeping that
#            FoF never reads. Exact. VERIFIED below, not assumed: the var
#            is read per-process by the unionfind library, so if it fails
#            to propagate the run silently ignores it and looks the same.
: ${SCAP:=}
: ${DEPTH:=}
: ${UFSIZES:=}
: ${MPITYPE:=cray_shasta}
# Reconverse comm backend, chosen at RUNTIME (both are compiled in):
#   BACKEND=lci  -> LCIv2 direct on CXI. Bootstraps via PMI, publishing
#                   O(rank_n^2) KVS entries -- the 4096-rank ceiling.
#   BACKEND=lcw  -> LCW, an active-message wrapper over MPI. Bootstraps
#                   via MPI_Init (Cray MPICH), which does NOT use LCI's
#                   quadratic KVS publish, so it should clear 512 nodes.
# LCWBK selects LCW's own transport; this build compiled MPI only
# (LCI-v1 and GASNet-EX were not found), so mpi is the only valid value.
: ${BACKEND:=lci}
: ${LCWBK:=mpi}
EXTRA=
[ -n "$SCAP" ]  && EXTRA="$EXTRA -s $SCAP"
[ -n "$DEPTH" ] && EXTRA="$EXTRA -D $DEPTH"
if [ -n "$UFSIZES" ]; then export FOF_UF_SIZES=$UFSIZES; fi

case "$DSET" in
  cosmo25)   INPUT=/lustre/orion/csc710/proj-shared/cosmo25PLK.2304g_dm.004096
             NPART=24461180928 ;;
  romulus25) INPUT=/lustre/orion/csc710/proj-shared/romulus25.3072g.ad.000352
             NPART=57982058496 ;;
  # REGRESSION GATE, not a scaling point: the 1.98B tipsy set whose answer
  # is known and fixed -- 424897832 components / max_size 185317566 at
  # every node count (design/fof3-2b-scaling.md). It sits BELOW 2^31, so
  # none of the 64-bit defects that bite the 24B/58B snapshots can fire
  # here; that is exactly what makes it the control. If a width fix ever
  # changes THIS number, the fix broke the merge logic itself.
  cosmo2b)  INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
             NPART=1981808640 ;;
  # Shape validator, not a data point: the 100k NChilada snapshot from the
  # reader check. Same code path, same srun line, ~10 s on 2 nodes -- run
  # this once before committing the sweep to 128-node jobs.
  smoke)     INPUT=/lustre/orion/csc710/scratch/rrao/nctest/100k.nchilada
             NPART=100000 ;;
  *) echo "FATAL: unknown DSET '$DSET'" >&2; exit 1 ;;
esac

# PEMAP is DERIVED HERE, never passed in. sbatch --export splits its value
# on commas, so --export=ALL,PEMAP=1-7,9-15,... delivers PEMAP=1-7 and
# silently drops the rest; every process then pins all 7 PEs to cores 1-7
# and the run comes out ~8x slow with nothing in the log to say why.
# (That cost jobs 5321313-17.) Anything with a comma stays on this side.
NOSMT=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
SMT=1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127
if [ "$PPN" = 7 ]; then PEMAP=$NOSMT; else PEMAP=$SMT; fi
nblocks=$(echo "$PEMAP" | tr ',' '\n' | wc -l)
if [ "$nblocks" -ne 8 ] && [ "$nblocks" -ne 16 ]; then
  echo "FATAL: PEMAP has $nblocks blocks, expected 8 (ppn 7) or 16 (ppn 14)" >&2
  exit 1
fi

# No core dumps. A CmiAbort at 1024 processes drops cores into the job's
# cwd, which is the binary's home directory -- the three failed rounds of
# 2026-08-23 wrote 37 GB there and filled the 51 GB home quota outright
# (silently: writes just start failing). Nothing here is debugged from a
# core file; the diagnostics are printf-based.
ulimit -c 0

B=/lustre/orion/csc710/scratch/rrao/compare
cd /ccs/home/rrao/paratreet2/examples/fof3 || exit 1
# BIN selects WHICH binary to run. The source tree's ./FoF3 is whatever was
# built last, which is a hazard: a queued job picks up the binary present when
# it STARTS, not when it was submitted. Point BIN at a saved copy to pin a run
# to a specific build.
#   (unset)          -> ./FoF3 from the tree (last build; the historical default)
#   $B/FoF3.lciag    -> patched LCI: bootstrap allgather, rank_n KVS entries
#   $B/FoF3.lcw...   -> LCI+LCW binary, unpatched LCI
#   $B/FoF3.lci-only -> the pre-LCW binary
: ${BIN:=./FoF3}
[ -x "$BIN" ] || { echo "FATAL: BIN '$BIN' not executable" >&2; exit 1; }
echo "### binary=$BIN  md5=$(md5sum "$BIN" | cut -c1-12)"
echo "### charm=$CHARMB  liblci=$(ldd "$BIN" 2>/dev/null | awk '/liblci.so/{print $3}')"
N=$SLURM_JOB_NUM_NODES
NT=$((N * 8))
NPE=$((NT * PPN))

# PMI KVS sizing is QUADRATIC in rank count, so it MUST be derived, not
# hardcoded. LCI's bootstrap alltoall publishes one KVS key per (rank,
# peer) pair -- see lci-src/src/bootstrap/bootstrap.cpp, the
# "LCI_BOOTSTRAP_%d_%d_%d" loop over rank_n -- so the KVS holds rank_n^2
# entries per PMI bootstrap round.
#
# The value carried in every script in this project since the 2B campaign
# was 4194304, which is EXACTLY 1024^2: it was sized for 128 nodes x 8
# processes and worked only because nothing had ever run wider. At 256
# nodes (2048 ranks) the requirement is 2048^2 = 4194304 -- already at the
# limit -- and the run dies during bootstrap with
#   _pmi2_add_kvs:ERROR: The KVS data segment of 4194304 entries is not
#   large enough
# before a single particle is read (jobs 5333502 at 256n, 5333507 at 512n).
# That reads like a crash but is pure launch-side sizing.
#
# Sizing has a CEILING as well as a floor, and they are close together.
# _pmi2_kvs_fence mallocs a gather buffer of PMI_MAX_KVS_ENTRIES * ~69 B
# into a SIGNED 32-BIT size, so above ~31.0M entries it wraps negative and
# the launch dies with
#   _pmi2_kvs_fence:malloc of full_kvs_recvstr failed (-1972027392 bytes)
# (job 5333628, 512 nodes, where a 2x margin asked for 33.5M entries =
# 2.16 GB > 2^31). So the usable window is
#   rank_n^2  <  PMI_MAX_KVS_ENTRIES  <  ~31.0M
# 1.25x rank_n^2 sits inside it at every scale this harness supports:
# 512 nodes wants 16.8M and gets 21.0M (1.35 GB), comfortably under.
#
# NOTE the entry cost is ~69 B, NOT the kilobytes a first reading of the
# 256-node memory jump suggested -- the KVS buffer is well under 1 GB at
# every point here and is NOT what moves MaxRSS between 128 and 256 nodes.
KVS=$(( NT * NT * 5 / 4 ))
[ "$KVS" -lt 4194304 ] && KVS=4194304
# Above ~1024 nodes rank_n^2 alone exceeds the int32 ceiling and Cray PMI2
# cannot bootstrap LCI's quadratic alltoall at all. Say so here rather
# than let it present as a malloc failure in the launcher.
if [ "$KVS" -gt 31000000 ]; then
  echo "FATAL: PMI_MAX_KVS_ENTRIES would be $KVS; _pmi2_kvs_fence overflows" >&2
  echo "       int32 above ~31.0M entries. $NT ranks needs $((NT*NT)) minimum." >&2
  echo "       This bootstrap cannot go wider -- use a non-PMI2 LCT backend." >&2
  exit 1
fi
# Probe override: set KVS_FORCE to pin the value regardless of rank count,
# so the MEMORY cost of a given PMI_MAX_KVS_ENTRIES can be measured at a
# cheap node count instead of inferred from two expensive points that also
# differ in peer count.
[ -n "$KVS_FORCE" ] && KVS=$KVS_FORCE
export PMI_MAX_KVS_ENTRIES=$KVS
echo "### PMI_MAX_KVS_ENTRIES=$KVS (1.25 x ${NT}^2; LCI bootstrap KVS is rank_n^2 per round)"

# One GCD per process, NUMA-local to that process's 7 cores. Frontier's
# core-to-GCD affinity is cores 0-7 -> 4, 8-15 -> 5, 16-23 -> 2, 24-31 -> 3,
# 32-39 -> 6, 40-47 -> 7, 48-55 -> 0, 56-63 -> 1, so assigning by rank
# would put every host-device copy across the Infinity Fabric for nothing.
# MUST be a wrapper, not an export: --gpus-per-node=8 makes Slurm set
# ROCR_VISIBLE_DEVICES in the task environment, which beats anything the
# batch script exports; the wrapper runs inside the task, so it wins.
# BOOTSTRAP=tcp routes LCI's bootstrap through its OWN TCP rendezvous
# instead of Cray PMI2. Needed above ~3900 ranks: LCI's bootstrap alltoall
# publishes rank_n^2 KVS entries and _pmi2_kvs_fence gathers ALL of them
# into one buffer whose size is computed in a SIGNED 32-BIT int. At 4096
# ranks that buffer is 2.32 GB and wraps negative --
#   _pmi2_kvs_fence:malloc of full_kvs_recvstr failed (-1972027392 bytes)
# -- and the byte count is IDENTICAL whatever PMI_MAX_KVS_ENTRIES says
# (jobs 5333628 at 33.5M entries and 5333839 at 21.0M entries printed the
# same number), because the fence is sized by the KVS CONTENT, not by the
# cap. So it is not tunable; the backend has to change.
# The tcp backend needs RANK/WORLD_SIZE (per task, hence set in the
# wrapper) and a master endpoint. LCT_MASTER_* is its preferred pair.
BOOT_ENV=
if [ "${BOOTSTRAP:-pmi2}" = tcp ]; then
  export LCT_PMI_BACKEND=tcp
  export LCT_MASTER_ADDR=$(scontrol show hostnames "$SLURM_JOB_NODELIST" | head -1)
  # Derive the port from the job id so two concurrent sweeps cannot collide.
  export LCT_MASTER_PORT=$(( 30000 + SLURM_JOB_ID % 20000 ))
  # One master accepts a connection from EVERY rank, so the rendezvous
  # barrier takes longer the wider the job. The default timeout is 60 s,
  # which 4096 ranks blow through ("failed waiting for TCP rendezvous
  # barrier: socket read timed out after 60 seconds", job 5333888).
  export LCT_PMI_TCP_TIMEOUT_SEC=${LCT_PMI_TCP_TIMEOUT_SEC:-900}
  echo "### bootstrap=tcp master=$LCT_MASTER_ADDR:$LCT_MASTER_PORT timeout=${LCT_PMI_TCP_TIMEOUT_SEC}s"
else
  echo "### bootstrap=pmi2 (default)"
fi

# ---- Projections SUMMARY tracing -----------------------------------------
# Full projections logs are not feasible at this scale (one .log.gz per PE,
# every event); summary mode writes ONE small text file per PE holding a
# run-length-encoded utilization histogram plus three per-entry-method
# vectors -- EPExeTime (total us), EPCallTime (invocations), MaxEPTime
# (longest single invocation) -- and an IdlePercent histogram. That is
# exactly the "where did the traversal bracket go" instrument, at ~1/1000
# the volume.
#
# BINSIZE is in SECONDS (charm's +binsize default is 0.001). 32 nodes runs
# romulus25 in ~250 s wall, so 0.01 gives 25k bins/PE, run-length encoded.
# The pool auto-doubles binSize if it overflows, so this cannot blow up.
#
# Tracing must be LINKED IN: BIN must be a binary built with
# `make SUMMARY=1` (-tracemode summary). Without it +traceroot is accepted
# in SILENCE and the run produces no trace at all -- so assert on the
# symbol here rather than discover it from an empty directory afterwards.
: ${BINSIZE:=0.01}
TRACEDIR=$B/traces/${SLURM_JOB_NAME}_${DSET}_${N}n.$SLURM_JOB_ID
mkdir -p "$TRACEDIR"
if ! nm -C "$BIN" 2>/dev/null | grep -q initTraceSummaryBOC; then
  echo "FATAL: '$BIN' has no summary tracing linked in (no initTraceSummaryBOC)." >&2
  echo "       Rebuild with: make CHARM_HOME=... GPU=1 SUMMARY=1 FoF3" >&2
  exit 1
fi
TRACE_ARGS="+traceroot $TRACEDIR +binsize $BINSIZE"
echo "### tracing=summary dir=$TRACEDIR binsize=${BINSIZE}s"

WRAP=$B/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
# RANK/WORLD_SIZE are per-task, so they can only be set here. Harmless
# under the pmi2 default: nothing reads them unless LCT_PMI_BACKEND=tcp.
export RANK=$SLURM_PROCID
export WORLD_SIZE=$SLURM_NTASKS
exec "$@"
EOW
chmod +x "$WRAP"

if [ "$BACKEND" = lcw ]; then
  BK_ARGS="+backend lcw +lcw_backend $LCWBK"
  # DO NOT set MPICH_GPU_SUPPORT_ENABLED=1 blindly. Cray MPICH aborts at
  # MPI_Init if it is requested without the GTL library linked:
  #   MPIDI_CRAY_init: GPU_SUPPORT_ENABLED is requested, but GTL library
  #   is not linked
  # (job 5339398, every rank SIGSEGV/255 in under a second). FoF3 is not
  # linked against -lmpi_gtl_hsa. Enabling GPU-aware MPI therefore needs a
  # RELINK (module craype-accel-amd-gfx90a), not an env var, so it is
  # opt-in here and off by default.
  if [ "${MPICH_GPU:-0}" = 1 ]; then export MPICH_GPU_SUPPORT_ENABLED=1; fi
  # Cray MPICH picks a NIC per rank by NUMA node by default, which REQUIRES
  # each rank to be confined to one NUMA node:
  #   MPICH ERROR: Unable to use a NIC_POLICY of 'NUMA'. Rank 0 is not
  #   confined to a single NUMA node. There are 4 numa_nodes
  # (job 5339579). This harness runs --cpu-bind=none on purpose and lets
  # charm's +pemap place the PEs, so ranks span NUMA nodes by design.
  # Changing the binding would perturb the LCI comparison, so change the
  # NIC policy instead. Frontier has 4 NICs and 8 ranks/node.
  export MPICH_OFI_NIC_POLICY=${MPICH_NIC_POLICY:-ROUND-ROBIN}
else
  BK_ARGS="+backend lci +lci_ndevices $NDEV"
fi
echo "### backend=$BACKEND bk_args='$BK_ARGS' mpich_gpu=${MPICH_GPU_SUPPORT_ENABLED:-unset}"
echo "### bigscale dset=$DSET nodes=$N tasks=$NT pes=$NPE ppn=$PPN leaf=$LEAF ndev=$NDEV poll=$POLL iters=$ITERS"
echo "### extra_args='$EXTRA'  FOF_UF_SIZES=${FOF_UF_SIZES:-unset}  mpi=$MPITYPE"
echo "### input=$INPUT"
echo "### particles=$NPART  -> $((NPART / NT)) per process, $((NPART / NPE)) per PE"
echo "### jobid=$SLURM_JOB_ID"
# A missing shared library shows up at scale as thousands of identical
# loader errors; catch it here in one line instead.
echo "### unresolved libs:"; ldd "$BIN" 2>&1 | grep "not found" || echo "###   (none)"
date +"### submit-side start %s.%N %F %T"

LOG=$B/logs/fof3_${DSET}_${N}n.$SLURM_JOB_ID.log

# Never put a filter at the end of an srun pipe: grep block-buffers when
# stdout is not a tty and a stall then shows as an empty log with no clue
# where it stopped. Stream raw through tee, analyse the file afterwards.
# NOGPU=1 selects the CPU-ONLY arm: device phase 1 off, everything else
# byte-identical. SWIFT's FoF has no GPU port, so a like-for-like CPU
# comparison needs this arm alongside the GPU one; quoting only the GPU
# number against a CPU-only code would not be a fair reading.
ARM=$([ "${NOGPU:-0}" = 1 ] && echo cpu || echo gpu)
if [ "${NOGPU:-0}" = 1 ]; then
  GPU_ENV=""
  echo "### arm=CPU-only (device phase 1 disabled)"
else
  GPU_ENV="PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1"
  echo "### arm=GPU (device phase 1)"
fi
# Watchdog: a hang in LCI's bootstrap at 4096 ranks would otherwise consume
# the whole 2 h reservation (>1000 node-hours at 512 nodes) and produce
# nothing at all. The milestone is reconverse's very FIRST line,
#   Reconverse> Starting Reconverse with 128 processes, 896 PEs ...
# which is printed immediately after the bootstrap completes -- so it is the
# earliest possible evidence that the job formed, and it also carries the
# process count that is asserted below.
# Timeouts scale with the job: bootstrap is the risk that grows with rank
# count, and the phases between prints are longer at width.
. $B/scripts/watchdog.sh
if   [ "$N" -ge 256 ]; then BOOT_TO=900;  STALL_TO=1800
elif [ "$N" -ge 64 ];  then BOOT_TO=600;  STALL_TO=1500
else                        BOOT_TO=420;  STALL_TO=1200; fi
echo "### watchdog boot=${BOOT_TO}s stall=${STALL_TO}s"
run_with_watchdog "$LOG" "$BOOT_TO" "$STALL_TO" "Starting Reconverse with" -- \
  env $GPU_ENV \
  srun --mpi=$MPITYPE --network=job_vni --unbuffered \
    --cpu-bind=none --distribution=block:block \
    --ntasks=$NT --gpus-per-node=8 --export=ALL \
    "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l $LEAF -i $ITERS $EXTRA \
    +ppn $PPN +pemap $PEMAP $BK_ARGS +backend_poll_thread $POLL $TRACE_ARGS
rc=$?
cat "$LOG"

# ASSERT the job formed at the width we paid for. Reconverse prints
#   Reconverse> Starting Reconverse with <P> processes, <PE> PEs ...
# Component counts are process-count INVARIANT in FoF3 by design, so they
# cannot detect a job that silently failed to form -- that is exactly how the
# `--mpi=pmi2` trap (every rank world size 1, correct answer, 16x the work)
# went unnoticed for a whole round of jobs. Check rank identity directly.
got_procs=$(grep -m1 -oE "Starting Reconverse with [0-9]+ processes" "$LOG" | awk '{print $4}')
got_pes=$(grep -m1 -oE "processes, [0-9]+ PEs" "$LOG" | awk '{print $2}')
if [ -n "$got_procs" ] && [ "$got_procs" != "$NT" ]; then
  echo "FATAL RANK MISMATCH: reconverse reports $got_procs processes, expected $NT." >&2
  echo "       The job did not form as intended; every timing here is invalid." >&2
  rc=99
fi
if [ -n "$got_pes" ] && [ "$got_pes" != "$NPE" ]; then
  echo "FATAL PE MISMATCH: reconverse reports $got_pes PEs, expected $NPE." >&2
  rc=99
fi
echo "### ranks_asserted=${got_procs:-NA}/$NT pes_asserted=${got_pes:-NA}/$NPE"
date +"### submit-side end %s.%N %F %T"
echo "### exit: $rc"

# ---- classify the point ---------------------------------------------------
# An OOM is the answer to "what is the minimum scale", so it gets its own
# status rather than being lumped in with a crash. slurmstepd reports the
# kill on the step's stderr, which the 2>&1 above captured.
status=UNKNOWN
if [ "$rc" = 99 ]; then
  status=RANK_MISMATCH
elif grep -q "^WATCHDOG " "$LOG"; then
  status=HANG
elif grep -qE "oom-kill|Out Of Memory|out of memory|Killed process|std::bad_alloc|Cannot allocate memory" "$LOG"; then
  status=OOM
elif [ "$rc" -eq 0 ] && grep -q "FOF3STAT components:" "$LOG"; then
  status=OK
elif [ "$rc" -ne 0 ]; then
  status=FAIL
fi
comp=$(grep -m1 -o "FOF3STAT components: [0-9]* max_size [0-9]*" "$LOG" | sed 's/FOF3STAT //')
bexact=$(grep -m1 "FOF3STAT linking_length_exact:" "$LOG" | awk '{print $3}')
# Anchor on the COMPONENTS line. FoF3 prints a SECOND, unrelated
# log2_histogram on its "redundancy_concentration" line, and that one
# appears FIRST in the log, so an unanchored grep silently reports the
# wrong histogram (0:279 1:161 2:37 3:7 instead of the component sizes).
hist=$(grep -m1 -o "FOF3STAT components:.*" "$LOG" | grep -o "log2_histogram:.*")
it1=$(grep  -m1 "^Iteration 1 time:"        "$LOG" | awk '{print $4}')
it0=$(grep  -m1 "^Iteration 0 time:"        "$LOG" | awk '{print $4}')
dec=$(grep  -m1 "Total Decomposition time:" "$LOG" | awk '{print $4}')
ld=$(grep   -m1 "Loading input data"        "$LOG" | awk '{print $7}')
hwm=$(grep  -m1 "vmhwm_mb after decomposition:" "$LOG" | awk '{print $5}')
echo "VERDICT dset=$DSET arm=$ARM nodes=$N tasks=$NT status=$status rc=$rc load_ms=${ld:-NA} decomp_ms=${dec:-NA} it0_ms=${it0:-NA} it1_ms=${it1:-NA} pe0_vmhwm_mb=${hwm:-NA} ${comp:-components: NA} b_exact=${bexact:-NA}"
echo "HIST $DSET $N ${hist:-NA}"

# Did FOF_UF_SIZES actually reach the processes? The library reads it
# per-process, so a failure to propagate is SILENT -- the run just keeps
# doing the bookkeeping and looks identical apart from being ~2% slower.
# The census makes it checkable: with sizes off, addsize_root must be 0
# and every union must be counted as skipped instead, so
# addsize_SKIPPED == fb2_UNION exactly.
if [ "${FOF_UF_SIZES:-1}" = 0 ]; then
  ar=$(grep -m1 "addsize_root"    "$LOG" | awk '{print $NF}')
  as=$(grep -m1 "addsize_SKIPPED" "$LOG" | awk '{print $NF}')
  fb=$(grep -m1 "fb2_UNION"       "$LOG" | awk '{print $NF}')
  if [ "${ar:-x}" = 0 ] && [ -n "$as" ] && [ "$as" = "$fb" ]; then
    echo "UFSIZES_CHECK PASS addsize_root=$ar addsize_SKIPPED=$as fb2_UNION=$fb"
  else
    echo "UFSIZES_CHECK FAIL addsize_root=${ar:-NA} addsize_SKIPPED=${as:-NA} fb2_UNION=${fb:-NA}" \
         "-- FOF_UF_SIZES did NOT take; treat timings as the sizes-on arm"
  fi
fi
# The stats -s and -D actually move.
grep -m1 "FOF3STAT loadcache_pack:" "$LOG" || true
grep -m1 "FOF3STAT cache: "         "$LOG" || true
grep -m1 "FOF3STAT time_s: iteration" "$LOG" || grep -m1 "loadCache" "$LOG" || true

# PE 0's vmhwm (above) is ONE process and misses the skew that decides
# which rank actually OOMs -- which is the whole question on an imbalanced
# snapshot. sacct's MaxRSS is the max across every task in the step, so it
# is the number to read for headroom.
# MaxRSS only lands in the accounting DB after the step is reaped; asking
# immediately returns an empty column (seen on the smoke job 5332061).
sleep 45
echo "### sacct (MaxRSS is across all tasks in the step):"
sacct -j "$SLURM_JOB_ID" --format=JobID%22,JobName%14,State%12,ExitCode,MaxRSS,MaxRSSNode%14,Elapsed 2>&1
echo "### trace files: $(ls "$TRACEDIR" | wc -l) in $TRACEDIR"
echo "### trace bytes: $(du -sh "$TRACEDIR" 2>/dev/null | cut -f1)"
ls "$TRACEDIR" | sed -n '1,5p'
echo "=== BIGSCALE POINT COMPLETE ==="
