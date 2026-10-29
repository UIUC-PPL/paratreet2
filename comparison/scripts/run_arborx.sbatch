#!/bin/bash
# One ArborX distributed-DBSCAN (= FoF) point. Env: DSET, and --nodes on submit.
#
# SHAPE: 8 ranks/node, one per MI250X GCD, NUMA-local via the wrapper -- the
# same 8-processes-per-node shape FoF3 uses, so the two GPU codes are given
# identical hardware per node.
#
# THREE TIMES ARE REPORTED SEPARATELY and they mean different things:
#   read_s    slab read of the snapshot off Lustre (compare to FoF3's
#             "Loading input data")
#   redist_s  Morton-order partition into spatially compact, equal-count
#             regions (compare to FoF3's "Total Decomposition time").
#             ArborX does NOT do this itself and cannot run without it -- see
#             the header comment of benchmarks/cosmo_fof/cosmo_fof.cpp.
#   dbscan_s  ArborX::Experimental::dbscan, i.e. ghost exchange + local
#             DBSCAN + label reconciliation (compare to FoF3's "Iteration 0")
#SBATCH --account=csc710
#SBATCH --job-name=cmpax
#SBATCH --time=02:00:00
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err

source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
ulimit -c 0

: ${DSET:?DSET must be cosmo2b, cosmo25 or romulus25}
: ${IMPL:=fdbscan}
# VERIFY=1 adds ArborX's own Details::verifyDBSCAN. It recomputes the whole
# eps-neighbourhood relation, so it is NOT free and is not run at every point;
# run it at the smallest node count of each dataset, where it is affordable,
# and rely on the component/max_size gate at the larger ones.
: ${VERIFY:=0}
: ${MINPTS:=2}
# FoF3's own linking length, printed at full precision by its
# "FOF3STAT linking_length_exact" line. Hardcoded per dataset so every point
# in the sweep links at exactly the same distance; recomputing 0.2*cbrt(V/N)
# from the raw particle bbox gives a DIFFERENT number, because FoF3's
# universe box is a cube of side (1+1.91e-6)*max_extent, not the bbox.
case "$DSET" in
  cosmo2b)   INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
             EPS=0.00015922463116076563 ;;
  cosmo25)   INPUT=/lustre/orion/csc710/proj-shared/cosmo25PLK.2304g_dm.004096
             EPS=${EPS:?EPS must be supplied for cosmo25 until FoF3 has printed it} ;;
  romulus25) INPUT=/lustre/orion/csc710/proj-shared/romulus25.3072g.ad.000352
             EPS=${EPS:?EPS must be supplied for romulus25 until FoF3 has printed it} ;;
  smoke)     INPUT=/lustre/orion/csc710/scratch/rrao/nctest/100k.nchilada
             EPS=0.150205 ;;
  *) echo "FATAL unknown DSET $DSET" >&2; exit 1 ;;
esac

BIN=$CMP/arborx-build/benchmarks/cosmo_fof/ArborX_CosmoFoF.exe
[ -x "$BIN" ] || { echo "FATAL: $BIN not executable" >&2; exit 1; }
N=$SLURM_JOB_NUM_NODES
NT=$((N * 8))

WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
# One GCD per rank, NUMA-local. Frontier's core->GCD affinity is
# 0-7 -> 4, 8-15 -> 5, 16-23 -> 2, 24-31 -> 3, 32-39 -> 6, 40-47 -> 7,
# 48-55 -> 0, 56-63 -> 1, so assigning by rank index would put every
# host-device copy across the Infinity Fabric for nothing.
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"

# Report the global merge-pair count from ArborX's label reconciliation.
# This is the quantity that bounds whether the run can work at all: every
# rank allocates the whole global set twice (device + host mirror) and the
# MPI_Allgatherv carrying it uses int-typed BYTE offsets, so the product
# global_pairs * sizeof(MergePair) must fit in a signed int.
export ARBORX_REPORT_MERGE_PAIRS=1
echo "### arborx dset=$DSET nodes=$N ranks=$NT impl=$IMPL minpts=$MINPTS"
echo "### binary=$BIN md5=$(md5sum $BIN | cut -c1-12)"
echo "### input=$INPUT"
echo "### eps=$EPS  (FoF3's exact linking length for this snapshot)"
echo "### jobid=$SLURM_JOB_ID"
echo "### unresolved libs:"; ldd "$BIN" 2>&1 | grep "not found" || echo "###   (none)"
VERIFY_ARG=""
[ "$VERIFY" = 1 ] && VERIFY_ARG="--verify"
echo "### verify=$VERIFY"
LOG=$CMP/logs/arborx_${DSET}_${N}n.$SLURM_JOB_ID.log
date +"### start %s.%N %F %T"
# Watchdog. The driver prints "#MPI ranks : N" immediately after MPI_Init
# and Kokkos::initialize, so it is the earliest evidence the job formed, and
# it carries the rank count asserted below.
. $CMP/scripts/watchdog.sh
if   [ "$N" -ge 256 ]; then BOOT_TO=900;  STALL_TO=2400
elif [ "$N" -ge 64 ];  then BOOT_TO=600;  STALL_TO=1800
else                        BOOT_TO=420;  STALL_TO=1500; fi
echo "### watchdog boot=${BOOT_TO}s stall=${STALL_TO}s"
run_with_watchdog "$LOG" "$BOOT_TO" "$STALL_TO" "#MPI ranks" -- \
  srun --ntasks=$NT --gpus-per-node=8 --export=ALL \
    "$WRAP" "$BIN" -f "$INPUT" -e "$EPS" -m "$MINPTS" -i "$IMPL" $VERIFY_ARG
rc=$?
cat "$LOG"

got_ranks=$(grep -m1 -oE "#MPI ranks *: *[0-9]+" "$LOG" | grep -oE "[0-9]+$")
if [ -n "$got_ranks" ] && [ "$got_ranks" != "$NT" ]; then
  echo "FATAL RANK MISMATCH: driver reports $got_ranks ranks, expected $NT." >&2
  rc=99
fi
echo "### ranks_asserted=${got_ranks:-NA}/$NT"
date +"### end %s.%N %F %T"
echo "### exit: $rc"

status=UNKNOWN
if [ "$rc" = 99 ]; then
  status=RANK_MISMATCH
elif grep -q "^WATCHDOG " "$LOG"; then
  status=HANG
elif grep -qE "oom-kill|Out Of Memory|out of memory|Killed process|bad_alloc|hipErrorOutOfMemory|Kokkos.*allocation" "$LOG"; then
  status=OOM
elif [ "$rc" -eq 0 ] && grep -q "ARBORXRESULT" "$LOG"; then
  status=OK
elif [ "$rc" -ne 0 ]; then
  status=FAIL
fi
res=$(grep -m1 "ARBORXRESULT" "$LOG")
comp=$(grep -m1 -o "ARBORXSTAT components: [0-9]* max_size [0-9]*" "$LOG" | sed 's/ARBORXSTAT //')
grp=$(grep -m1 -o "ARBORXSTAT groups_multi: [0-9]*  isolated: [0-9]*" "$LOG")
imb=$(grep -m1 -o "imbalance [0-9.]*" "$LOG")
mp=$(grep -m1 -o "ARBORX_MERGE_PAIRS global [0-9]*  bytes [0-9]*  int32_headroom [0-9.]*" "$LOG")
ver=$(grep -m1 -o "ARBORX_VERIFY [A-Z]*" "$LOG")
echo "VERDICT_AX dset=$DSET nodes=$N ranks=$NT status=$status rc=$rc ${comp:-components: NA} ${imb:-imbalance NA}"
echo "DETAIL_AX ${res:-NA} | ${grp:-NA} | ${mp:-merge_pairs NA} | ${ver:-verify NOT-RUN}"
sleep 45
echo "### sacct:"
sacct -j "$SLURM_JOB_ID" --format=JobID%22,JobName%12,State%12,ExitCode,MaxRSS,MaxRSSNode%14,Elapsed 2>&1
echo "=== ARBORX POINT COMPLETE ==="
