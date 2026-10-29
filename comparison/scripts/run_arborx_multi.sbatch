#!/bin/bash
# SEVERAL ArborX points inside ONE allocation. Env: POINTS="dset:partition ..."
#
# Why this exists: at 512 nodes the queue wait dominates and each point only
# needs a few minutes of the allocation (at 256 nodes read_s was 21-34 s and
# solve_s 1.3-79.5 s). Submitting three separate 512-node jobs pays three
# queue waits and three 512-node reservations; this pays one. Each point runs
# under its own watchdog, so one hang does not cost the others -- the loop
# continues to the next point.
#
# Everything per point is the same as run_arborx_hacc.sbatch (same shape, same
# wrapper, same watchdog thresholds, same VERDICT_AXH/DETAIL_AXH lines) so the
# output is collected by the same greps. Keep the two in step.
#SBATCH --account=csc710
#SBATCH --job-name=cmpaxmulti
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err

source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
ulimit -c 0
: ${POINTS:?POINTS must be a list like "cosmo25:balanced romulus25:balanced"}
: ${IMPL:=fdbscan}
: ${MINPTS:=2}
. $CMP/scripts/linklen.env

BIN=$CMP/arborx-build/benchmarks/cosmo_fof/ArborX_CosmoFoF_HACC.exe
[ -x "$BIN" ] || { echo "FATAL: $BIN not executable" >&2; exit 1; }
N=$SLURM_JOB_NUM_NODES
NT=$((N * 8))

WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"

echo "### arborx MULTI nodes=$N ranks=$NT points='$POINTS'"
echo "### binary=$BIN md5=$(md5sum $BIN | cut -c1-12)"
echo "### jobid=$SLURM_JOB_ID"
echo "### unresolved libs:"; ldd "$BIN" 2>&1 | grep "not found" || echo "###   (none)"
. $CMP/scripts/watchdog.sh

for PT in $POINTS; do
  DSET=${PT%%:*}; PARTITION=${PT##*:}
  case "$DSET" in
    cosmo2b)   INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
               EPS=$LINKLEN_cosmo2b ;;
    cosmo25)   INPUT=/lustre/orion/csc710/proj-shared/cosmo25PLK.2304g_dm.004096
               EPS=$LINKLEN_cosmo25 ;;
    romulus25) INPUT=/lustre/orion/csc710/proj-shared/romulus25.3072g.ad.000352
               EPS=$LINKLEN_romulus25 ;;
    *) echo "FATAL unknown DSET $DSET in point '$PT'" >&2; continue ;;
  esac
  [ -n "$EPS" ] || { echo "FATAL no linking length for $DSET" >&2; continue; }

  echo ""
  echo "################ POINT dset=$DSET partition=$PARTITION nodes=$N ranks=$NT"
  echo "### input=$INPUT"
  echo "### eps=$EPS  (FoF3's exact linking length for this snapshot)"
  LOG=$CMP/logs/arborxhacc_${DSET}_${N}n.$SLURM_JOB_ID.${PARTITION}.log
  date +"### start %s.%N %F %T"
  if   [ "$N" -ge 256 ]; then BOOT_TO=900;  STALL_TO=2400
  elif [ "$N" -ge 64 ];  then BOOT_TO=600;  STALL_TO=1800
  else                        BOOT_TO=420;  STALL_TO=1500; fi
  echo "### watchdog boot=${BOOT_TO}s stall=${STALL_TO}s"
  run_with_watchdog "$LOG" "$BOOT_TO" "$STALL_TO" "#MPI ranks" -- \
    srun --ntasks=$NT --gpus-per-node=8 --export=ALL \
      "$WRAP" "$BIN" -f "$INPUT" -e "$EPS" -m "$MINPTS" -i "$IMPL" -p "$PARTITION"
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
  if [ "$rc" = 99 ]; then status=RANK_MISMATCH
  elif grep -q "^WATCHDOG " "$LOG"; then status=HANG
  elif grep -qE "oom-kill|Out Of Memory|out of memory|Killed process|bad_alloc|hipErrorOutOfMemory|Kokkos.*allocation" "$LOG"; then status=OOM
  elif [ "$rc" -eq 0 ] && grep -q "HACCRESULT" "$LOG"; then status=OK
  elif [ "$rc" -ne 0 ]; then status=FAIL
  fi
  res=$(grep -m1 "HACCRESULT" "$LOG")
  comp=$(grep -m1 -o "HACCSTAT components: [0-9]* max_size [0-9]*" "$LOG")
  cnt=$(grep -m1 -o "HACCCOUNT components: [0-9]*" "$LOG")
  imb=$(grep -m1 -o "imbalance [0-9.]*" "$LOG")
  mp=$(grep -m1 -o "halo *: total [0-9]* ([0-9.]*% of N) max/rank [0-9]*" "$LOG")
  rnd=$(grep -m1 -o "rounds [0-9]*, component-min updates [0-9]*" "$LOG")
  ver=$(grep -m1 -o "CENSUS_CHECK [A-Z]*" "$LOG")
  cen=$(grep -m1 -o "census *: Lmax .*" "$LOG")
  # solve_s survives a census crash -- it is printed inside HACCRESULT, but
  # dbscan_s/stitch_s are printed BEFORE the census runs, so recover the sum
  # from them when HACCRESULT never got printed. Two cosmo25 points were
  # thrown away this way before (jobs 5405685, 5405686).
  d=$(grep -m1 "TIME dbscan_s" "$LOG" | awk '{print $4}')
  st=$(grep -m1 "TIME stitch_s" "$LOG" | awk '{print $4}')
  solve=NA
  [ -n "$d" ] && [ -n "$st" ] && solve=$(python3 -c "print(f'{$d+$st:.3f}')")
  echo "VERDICT_AXH dset=$DSET partition=$PARTITION nodes=$N ranks=$NT status=$status rc=$rc ${comp:-${cnt:-components: NA}} ${imb:-imbalance NA} solve_recovered=$solve"
  echo "DETAIL_AXH ${res:-NA} | ${mp:-halo NA} | ${ver:-census NA} | ${rnd:-rounds NA} | ${cen:-census-line NA}"
done

sleep 45
echo "### sacct:"
sacct -j "$SLURM_JOB_ID" --format=JobID%22,JobName%14,State%12,ExitCode,MaxRSS,MaxRSSNode%14,Elapsed 2>&1
echo "=== ARBORXHACC MULTI COMPLETE ==="
