# run_with_watchdog <log> <boot_timeout_s> <stall_timeout_s> <first_milestone_regex> -- cmd...
#
# WHY: a 512-node job at a 2 h walltime that hangs in bootstrap burns ~1024
# node-hours for zero data. LCI's bootstrap is exactly where that risk lives
# at 4096 ranks (design/fof3-bigscale-2026-08-23.md: the unpatched bootstrap
# publishes O(rank_n^2) PMI KVS entries and Cray PMI's fence overflows an
# int32 there), and a bootstrap hang produces NO output at all, so it is
# indistinguishable from a slow start unless something is watching.
#
# Two phases:
#   1. BOOT: the application must print <first_milestone_regex> within
#      boot_timeout seconds. Anything longer is treated as a bootstrap hang.
#   2. STALL: after that, the log must keep growing; no growth for
#      stall_timeout seconds is treated as a hang.
# Either way the step is killed and the caller gets rc=124 plus a WATCHDOG
# line in the log, so the point is recorded as a hang rather than silently
# eating the reservation.
#
# Chosen generously on purpose -- a false kill costs a rerun, and phases at
# 512 nodes legitimately take minutes (decomposition splitter setup measured
# 116 s at 4096 ranks).
run_with_watchdog() {
  local log="$1" boot_to="$2" stall_to="$3" milestone="$4"
  shift 4
  [ "$1" = "--" ] && shift

  "$@" > "$log" 2>&1 &
  local child=$!

  (
    local t0 now sz last lastchange
    t0=$(date +%s)
    # Phase 1: wait for the first application output.
    while kill -0 "$child" 2>/dev/null; do
      grep -qE "$milestone" "$log" 2>/dev/null && break
      now=$(date +%s)
      if [ $((now - t0)) -ge "$boot_to" ]; then
        echo "WATCHDOG BOOTSTRAP_HANG: no line matching '$milestone' after ${boot_to}s" | tee -a "$log"
        kill -TERM "$child" 2>/dev/null; sleep 20; kill -KILL "$child" 2>/dev/null
        exit 0
      fi
      sleep 10
    done
    # Phase 2: the log must keep growing.
    last=-1; lastchange=$(date +%s)
    while kill -0 "$child" 2>/dev/null; do
      sz=$(stat -c %s "$log" 2>/dev/null || echo 0)
      now=$(date +%s)
      if [ "$sz" != "$last" ]; then last="$sz"; lastchange="$now"; fi
      if [ $((now - lastchange)) -ge "$stall_to" ]; then
        echo "WATCHDOG STALL: log unchanged for ${stall_to}s" | tee -a "$log"
        kill -TERM "$child" 2>/dev/null; sleep 20; kill -KILL "$child" 2>/dev/null
        exit 0
      fi
      sleep 30
    done
  ) &
  local wd=$!

  wait "$child"; local rc=$?
  kill "$wd" 2>/dev/null; wait "$wd" 2>/dev/null
  if grep -q "^WATCHDOG " "$log" 2>/dev/null; then return 124; fi
  return $rc
}
