#!/bin/bash
# One SWIFT stand-alone FoF point. Env: DSET, RPN, THREADS, TOPCELLS,
# MINGROUP, LINKLEN; --nodes on submit.
#
# SWIFT's FoF is CPU-only (there is no GPU port of fof.c), so this arm is a
# CPU measurement against FoF3's GPU-assisted phase 1 and ArborX's HIP
# DBSCAN. That asymmetry is REAL and has to be stated, not hidden: the
# campaign therefore also runs a CPU-only FoF3 arm (FOF_GPU_PHASE1 unset) so
# there is a like-for-like CPU comparison as well as a best-vs-best one.
#
# THE TIME TO QUOTE is "Complete FOF search took", printed by engine_fof.
# It covers local FoF + foreign-cell links + fragment linking + group-id
# assignment, i.e. the group FINDING. It is the analogue of FoF3's
# "Iteration 0 time". SWIFT's IC read and space_init/engine_split
# (its domain decomposition) are reported separately, as FoF3's load and
# decomposition times are.
#SBATCH --account=csc710
#SBATCH --job-name=cmpswift
#SBATCH --time=02:00:00
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err

source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
module load cray-hdf5-parallel/1.14.3.5 parmetis/4.0.3-mpi metis/5.1.0 \
  gsl/2.8 cray-fftw/3.3.10.11
ulimit -c 0

# libfabric/CXI tuning. engine_do_redistribute posts ONE MPI_Irecv per peer
# per particle type, and on the 24B snapshot at 128 ranks those are 19.2e6
# gparts = 2.1 GB each; registering that much memory per rank exhausted the
# NIC's resources and job 5404839 died with a wall of
#   cxil_map: write error
#   MPIDI_OFI_do_irecv(356): OFI tagged recv failed (No space left on device)
# 80 s in, right after "Running FOF on ... 24461180928 DM particles". These
# are the same settings the FoF3 arm runs with, so the two codes are given
# the same fabric configuration.
# Set UNCONDITIONALLY, for the reason spelled out in env/env.sh for
# MPICH_OFI_NIC_POLICY: Frontier's site modules ALREADY export
# FI_MR_CACHE_MONITOR (=kdreg2), so a `${FI_MR_CACHE_MONITOR:-userfaultfd}`
# default never fires and the fix silently does nothing. Job 5405082 printed
#   ### fabric: rx_match=hybrid mr_monitor=kdreg2 cq=131072
# which is how this was caught -- the resolved values are echoed below
# precisely so a no-op "fix" cannot hide. Override with CMP_MR_MONITOR etc.
export FI_CXI_RX_MATCH_MODE=${CMP_RX_MATCH_MODE:-hybrid}
export FI_MR_CACHE_MONITOR=${CMP_MR_MONITOR:-userfaultfd}
export FI_CXI_DEFAULT_CQ_SIZE=${CMP_CQ_SIZE:-131072}
# `cxil_map: write error` is a MEMORY REGISTRATION failure, not a host OOM.
# Job 5412796 proved that: 228 GB peak against a 256 GB/rank budget (RPN 2),
# so the rank had headroom and the map still failed. CXI has a limited number
# of hardware "optimized MR" slots; an application registering many regions
# exhausts them and every further registration fails. Setting
# FI_CXI_OPTIMIZED_MRS=0 falls back to standard MRs, which are slower per
# region but far more numerous. Left at the provider default unless
# CMP_OPTIMIZED_MRS is given, so this cannot silently change the runs already
# collected.
if [ -n "${CMP_OPTIMIZED_MRS:-}" ]; then
  export FI_CXI_OPTIMIZED_MRS=$CMP_OPTIMIZED_MRS
fi
[ -n "${CMP_MR_CACHE_MAX_COUNT:-}" ] && export FI_MR_CACHE_MAX_COUNT=$CMP_MR_CACHE_MAX_COUNT

: ${DSET:?DSET must be cosmo2b, cosmo25, romulus25 or smoke}
: ${RPN:=8}         # MPI ranks per node
: ${THREADS:=7}     # threads per rank
# TOPCELLS (Scheduler:max_top_level_cells) is DERIVED from the rank grid below;
# it must not be defaulted here or the derivation can never fire.
: ${MINGROUP:=32}   # FOF:min_group_size
: ${SPLITSIZE:=400}
: ${PIN:=1}
# Domain decomposition strategy.
#
# "grid" -- an axis-aligned split of the top-level cell grid over the ranks --
# is what this campaign uses EVERYWHERE, not just above 64 ranks. SWIFT's
# default "memory" (ParMETIS-weighted) aborts at >= 128 ranks here with
#   ***Cannot bisect a graph with 0 vertices!
# and the failure is not a simple cells-per-rank threshold: it failed at 16
# cells/rank, worked at 64, and failed again at 72. The decisive probe was
# jobs 5404566-68 at 128 ranks on cosmo2b:
#   initial_type=grid            -> OK,   fof_ms=404158.311
#   initial_type=memory usemetis -> FAIL rc=143
#   initial_type=memory adaptive -> FAIL rc=143
# all three at identical shape and cdim. Since the whole SWIFT scaling curve
# has to be ONE configuration to be quotable, the low-rank points are re-run
# on grid too rather than mixing decomposition strategies along one line.
# Grid also removes ParMETIS from the timed path entirely, which makes the
# SWIFT numbers depend only on SWIFT.
: ${DDTYPE:=grid}
: ${DDADAPT:=1}
: ${DDUSEMETIS:=0}

# ---------------------------------------------------------------------------
# THE PROXY CAP, and why max_top_level_cells is derived from the rank grid.
#
# `cell.mpi.sendto` (src/cell.h) is an `unsigned long long`, one BIT per
# proxy, so a rank may have at most 64 proxies no matter what
# engine_maxproxies says -- raising that macro to 1024 did nothing, and job
# 5404838 (cosmo2b, 64 nodes, 256 ranks) died at t=2 s with
#   engine_proxy.c:engine_add_proxy():159: Created more than 64 proxies.
#
# The proxy REACH is set by gravity, not by FoF. engine_makeproxies computes
#   delta_cells = (int)(2 * r_max / theta_crit / dmin) + 1
# with r_max = sqrt(3)*w for a cubic cell of side w, so at theta_cr = 0.7
# that is 5 top-level cells in every direction -- an 11^3 neighbourhood --
# even though FoF only ever links within one linking length, which is a tiny
# fraction of a cell. Raising theta_cr is not an escape: SWIFT hard-errors on
# theta_crit >= 1 (gravity_properties.c:147), and 0.999 still gives 4.
#
# The number of RANKS inside that reach is what has to be bounded, and it
# depends on how many cells thick each rank's block is:
#   proxies ~ prod_d ( 2*ceil(delta_cells / (cdim/grid_d)) + 1 )
# So if every block is at least delta_cells thick, the reach never crosses
# more than one block per axis and the count is 3^3 = 27, safely under 64,
# at ANY rank count. That needs cdim >= 5 * max(grid_d).
#
# Hence: the rank grid is PINNED here (most-cubic factorisation, descending)
# and cdim is derived from it, instead of the old "64 top-level cells per
# rank" heuristic -- which happened to satisfy the bound at <= 128 ranks and
# violated it at 256 (grid [8 8 4], cdim 26, blocks 3.25 cells thick ->
# 5*5*3 = 75 proxies).
read GX GY GZ <<<"$(python3 -c "
P = $((SLURM_JOB_NUM_NODES * ${RPN}))
best = None
for a in range(1, P + 1):
    if P % a: continue
    for b in range(1, P // a + 1):
        if (P // a) % b: continue
        c = P // a // b
        t = tuple(sorted((a, b, c), reverse=True))
        if best is None or t[0] < best[0] or (t[0] == best[0] and t[1] < best[1]):
            best = t
print(best[0], best[1], best[2])
")"
DDGRID="[$GX, $GY, $GZ]"
: ${TOPCELLS:=$((5 * GX))}
[ "$TOPCELLS" -lt 16 ] && TOPCELLS=16

# FoF3's exact linking length per snapshot, from its
# "FOF3STAT linking_length_exact" line.
case "$DSET" in
  cosmo2b)   IC=$CMP/data/cosmo2b.hdf5   ; LINKLEN=${LINKLEN:-0.00015922463116076563} ;;
  cosmo25)   IC=$CMP/data/cosmo25.hdf5   ; LINKLEN=${LINKLEN:?LINKLEN required until FoF3 has printed it} ;;
  romulus25) IC=$CMP/data/romulus25.hdf5 ; LINKLEN=${LINKLEN:?LINKLEN required until FoF3 has printed it} ;;
  smoke)     IC=$CMP/data/100k.hdf5      ; LINKLEN=${LINKLEN:-0.150205} ;;
  *) echo "FATAL unknown DSET $DSET" >&2; exit 1 ;;
esac
[ -f "$IC" ] || { echo "FATAL: IC file $IC missing -- run convert_swift.sbatch first" >&2; exit 1; }

N=$SLURM_JOB_NUM_NODES
NT=$((N * RPN))
RUN=$CMP/runs/swift_${DSET}_${N}n_${RPN}x${THREADS}_$SLURM_JOB_ID
mkdir -p "$RUN"; cd "$RUN" || exit 1

# The softening only has to be finite and small; FoF never uses it, but
# SWIFT's gravity properties are initialised regardless.
SOFT=$(python3 -c "print(${LINKLEN}/10.0)")
sed -e "s|@@ICFILE@@|$IC|" -e "s|@@OUTDIR@@|$RUN|" -e "s|@@MINGROUP@@|$MINGROUP|" \
    -e "s|@@LINKLEN@@|$LINKLEN|" -e "s|@@TOPCELLS@@|$TOPCELLS|" \
    -e "s|@@SPLITSIZE@@|$SPLITSIZE|" -e "s|@@SOFT@@|$SOFT|" \
    -e "s|@@DDTYPE@@|$DDTYPE|" -e "s|@@DDADAPT@@|$DDADAPT|" \
    -e "s|@@DDUSEMETIS@@|$DDUSEMETIS|" -e "s|@@DDGRID@@|$DDGRID|" \
    $CMP/scripts/fof.yml.tmpl > fof.yml

: ${BIN:=$CMP/swift/fof_mpi}   # override to pin a variant (e.g. fof_mpi.dbl)
[ -x "$BIN" ] || { echo "FATAL: $BIN not executable" >&2; exit 1; }
ALLOC_CPT=${SLURM_CPUS_PER_TASK:-1}
if [ "$ALLOC_CPT" -lt "$THREADS" ]; then
  echo "FATAL: --threads=$THREADS but only $ALLOC_CPT cpus/task were allocated." >&2
  echo "       Submit with --cpus-per-task=$THREADS (one core per SWIFT thread," >&2
  echo "       no SMT, matching FoF3's shape)." >&2
  exit 1
fi
echo "### swift dset=$DSET nodes=$N ranks=$NT rpn=$RPN threads=$THREADS cpus_per_task=$ALLOC_CPT"
echo "### topcells=$TOPCELLS min_group_size=$MINGROUP linklen=$LINKLEN"
echo "### domain_decomp type=$DDTYPE grid=$DDGRID adaptive=$DDADAPT usemetis=$DDUSEMETIS"
echo "### proxy bound: blocks are $((TOPCELLS / GX)) cells thick, need >= 5"
echo "### fabric: rx_match=$FI_CXI_RX_MATCH_MODE mr_monitor=$FI_MR_CACHE_MONITOR cq=$FI_CXI_DEFAULT_CQ_SIZE opt_mrs=${FI_CXI_OPTIMIZED_MRS:-<default>} mr_cache_max_count=${FI_MR_CACHE_MAX_COUNT:-<default>}"
echo "### binary=$BIN md5=$(md5sum $BIN | cut -c1-12)"
echo "### ic=$IC ($(stat -c %s "$IC") bytes)"
echo "### run dir=$RUN"
echo "### jobid=$SLURM_JOB_ID"

# Neither output is part of any FoF timing; both are ruinous at these particle
# counts (see the note in swift_fof.c). SWIFT still prints the group census.
export SWIFT_FOF_NO_CATALOGUE=1
export SWIFT_FOF_NO_SNAPSHOT=1

AFF=""
[ "$PIN" = 1 ] && AFF="--pin"
LOG=$CMP/logs/swift_${DSET}_${N}n_${RPN}x${THREADS}.$SLURM_JOB_ID.log
date +"### start %s.%N %F %T"
# --cpus-per-task is NOT passed here. It has to be requested at the JOB level
# (sbatch --cpus-per-task), and asking srun for more than the allocation gives
#   srun: error: Unable to create step for job N: More processors requested
#   than permitted
# which is what killed the first shape sweep (jobs 5404080-83). submit_sweep.sh
# and the shape sweep both pass --ntasks-per-node and --cpus-per-task on the
# sbatch line; srun inherits them.
# NEVER put a comment inside a line continuation. Doing so silently SPLIT
# this into two commands -- an `srun` with no program, then SWIFT executed
# DIRECTLY ON THE BATCH NODE with a single rank. It read the ICs and would
# have printed a perfectly plausible "Complete FOF search took", on 1 rank of
# an 8-node reservation. Caught only because the log said "with 1 ranks"
# (jobs 5404133-36, cancelled). This is the same class of silent
# non-parallelism as the `--mpi=pmi2` trap in fof3-bigscale-2026-08-23.md, so
# the rank count is now ASSERTED below rather than trusted.
#
# --verbose takes an INTEGER in SWIFT (1 = per-step, 2 = per-rank); a bare
# --verbose is rejected with "option `--verbose` expects an integer value".
# Level 1 prints the "(FOF SCALING)" stage timers this campaign reads.
#
# Watchdog: "running on:" is printed right after MPI_Init, so it proves the
# job formed; SWIFT then reads up to a 2 TB IC, hence the long stall timeout.
. $CMP/scripts/watchdog.sh
# STALL_TO is deliberately generous. SWIFT goes SILENT for long stretches
# inside fof_find_foreign_links / fragment linking -- there is no periodic
# progress print in that phase -- and the old 1500 s stall limit KILLED a
# legitimately-running 4-node cosmo2b point (job 5404516: last log line at
# SWIFT t=210 s, killed at t=1726 s, 74 GB/rank resident and no error). The
# BOOT timeout stays tight, because that is the one the user asked to be
# aggressive about: it catches a job that never forms, before it has burned
# any real node-hours.
if   [ "$N" -ge 256 ]; then BOOT_TO=900;  STALL_TO=4200
elif [ "$N" -ge 64 ];  then BOOT_TO=600;  STALL_TO=3600
else                        BOOT_TO=420;  STALL_TO=3000; fi
echo "### watchdog boot=${BOOT_TO}s stall=${STALL_TO}s"
run_with_watchdog "$LOG" "$BOOT_TO" "$STALL_TO" "running on:" -- \
  srun --ntasks=$NT --ntasks-per-node=$RPN --export=ALL \
    "$BIN" $AFF --threads=$THREADS --verbose=1 fof.yml
rc=$?
cat "$LOG"

# ASSERT the job actually formed with the ranks we paid for. SWIFT prints
# "... with %d ranks, %d threads / rank ...". A mismatch means the step did
# not launch as intended and every timing below is meaningless.
got_ranks=$(grep -m1 -oE "with [0-9]+ ranks" "$LOG" | awk '{print $2}')
if [ -n "$got_ranks" ] && [ "$got_ranks" != "$NT" ]; then
  echo "FATAL RANK MISMATCH: SWIFT reports $got_ranks ranks but $NT were requested." >&2
  echo "       The srun step did not form as intended; timings are invalid." >&2
  rc=99
fi
echo "### ranks_asserted=${got_ranks:-NA} expected=$NT"
date +"### end %s.%N %F %T"
echo "### exit: $rc"

status=UNKNOWN
if [ "$rc" = 99 ]; then
  status=RANK_MISMATCH
elif grep -q "^WATCHDOG " "$LOG"; then
  status=HANG
elif grep -qE "oom-kill|Out Of Memory|out of memory|Killed process|bad_alloc|Failed to allocate" "$LOG"; then
  status=OOM
elif [ "$rc" -eq 0 ] && grep -q "Complete FOF search took" "$LOG"; then
  status=OK
elif [ "$rc" -ne 0 ]; then
  status=FAIL
fi
fofms=$(grep -m1 -o "Complete FOF search took: [0-9.]* ms" "$LOG" | awk '{print $5}')
ngrp=$(grep -m1 -o "No. of groups: [0-9]*" "$LOG" | awk '{print $4}')
maxg=$(grep -m1 -o "Largest group (linkables only) by size: [0-9]*" "$LOG" | awk '{print $NF}')
icms=$(grep -m1 -o "Reading initial conditions took [0-9.]* ms" "$LOG" | awk '{print $5}')
spms=$(grep -m1 -o "space_init took [0-9.]* ms" "$LOG" | awk '{print $3}')
echo "VERDICT_SW dset=$DSET nodes=$N ranks=$NT rpn=$RPN threads=$THREADS status=$status rc=$rc" \
     "read_ms=${icms:-NA} spaceinit_ms=${spms:-NA} fof_ms=${fofms:-NA}" \
     "groups=${ngrp:-NA} max_size=${maxg:-NA} min_group_size=$MINGROUP"
echo "### FOF SCALING stages:"
grep -E "\(FOF SCALING\)" "$LOG" | head -20
sleep 45
echo "### sacct:"
sacct -j "$SLURM_JOB_ID" --format=JobID%22,JobName%12,State%12,ExitCode,MaxRSS,MaxRSSNode%14,Elapsed 2>&1
echo "=== SWIFT POINT COMPLETE ==="
