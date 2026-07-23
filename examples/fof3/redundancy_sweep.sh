#!/usr/bin/env bash
# Sweep FoF3 phase-3 redundancy vs PROCESS count at a FIXED number of
# PEs (cores) per process, to characterize the pre-witness redundancy that 3b
# parking would eliminate (design/step3.md §6d). The go/no-go input is the
# CURVE of per-pair redundancy vs process count, plus the per-process skew.
#
# Why this shape: the redundancy 3b targets is counted intra-process (the
# per-process SEEN table) but DRIVEN by across-process cache-fetch latency, so
# the representative topology is one process per node with cores as PEs, run on
# REAL nodes (network fetches), swept over node count at fixed cores/node. A
# fat single node or 1-PE-per-process understates it. Use a clustered input
# (LAMBS 80M), not Plummer — clustered is ~50x hotter (§6c).
#
# NOTE: PROCS/{P} below is a process count, not a node count — node placement
# for the {P} processes is left to the scheduler. If the scheduler packs more
# than one process onto a node, some "across-process" fetches in that run are
# actually same-node (shared-memory-ish), which understates the redundancy
# this sweep is meant to characterize (per the paragraph above). For a run
# that matches the documented rationale, size PROCS to your node count and
# constrain placement to one process per node (e.g. srun -N/--ntasks-per-node).
#
# Usage (local smoke test, loopback):
#   ./redundancy_sweep.sh
# Usage (cluster): set LAUNCH to your scheduler's form, with {NPE} = total PEs,
# {PPN} = PEs/process, {P} = process count; e.g. for srun+charmrun:
#   INPUT=/path/lambs.80M.tipsy PPN=15 PROCS="1 2 4 8 16" BFACTOR=0.2 \
#   LAUNCH='srun --unbuffered --mpi=pmi2 -n {P} ./FoF3 -f $INPUT -d oct -u dist -b $BFACTOR +ppn {PPN}' \
#   ./redundancy_sweep.sh
# (LAUNCH is eval'd, so $INPUT/$BFACTOR expand; keep them single-quoted above.)
set -u

INPUT="${INPUT:-../../inputs/1m.tipsy}"   # clustered LAMBS 80M on the cluster
PPN="${PPN:-2}"                           # PEs (cores) per process (= cores/node)
PROCS="${PROCS:-1 2 4}"                    # process counts to sweep (node placement left to scheduler)
BFACTOR="${BFACTOR:-0.8}"                  # linking-length factor
DECOMP="${DECOMP:-oct}"
SETTLE="${SETTLE:-3}"                      # seconds between runs (frees loopback ports)
# Default launcher (loopback). Set separately, NOT via ${LAUNCH:-...}: literal
# braces in the default break bash's ${..} brace matching. Single-quoted so
# $INPUT/$DECOMP/$BFACTOR and the {NPE}/{PPN} placeholders survive to eval/sed.
if [ -z "${LAUNCH:-}" ]; then
  LAUNCH='./charmrun ++local ./FoF3 -f $INPUT -d $DECOMP -u dist -b $BFACTOR +p{NPE} ++ppn {PPN}'
fi

echo "# input=$INPUT ppn=$PPN b_factor=$BFACTOR decomp=$DECOMP procs='$PROCS'"
printf '%-6s %-6s %-7s %14s %9s %8s %-28s %s\n' \
  procs ppn cores redun_total ratio walk_s "per-proc min/avg/max" status
for P in $PROCS; do
  NPE=$((P * PPN))
  # sed, not ${var//{X}/..}: literal braces confuse bash parameter-expansion.
  cmd="$(printf '%s' "$LAUNCH" | sed -e "s|{NPE}|$NPE|g" -e "s|{PPN}|$PPN|g" -e "s|{P}|$P|g")"
  echo "# launching: $cmd" >&2
  out="$(eval "$cmd" 2>&1)"
  total="$(echo "$out" | awk '/FOF3STAT redundancy:/ {print $4}')"
  ratio="$(echo "$out" | awk '/FOF3STAT redundancy:/ {print $8}')"
  bal="$(  echo "$out" | awk '/balance: redundant_descents/ {print $4}')"
  walk="$( echo "$out" | awk '/time_s: phase3_walk/ {print $4}')"
  # Auto mode falls back to stats-only (no serial reference, no PASSED line)
  # above kAutoFullMaxN (FoF3.C: "full verification SKIPPED"); that's expected
  # for large N, not a failure — only flag !!FAILED when neither completion
  # line shows up at all.
  if echo "$out" | grep -q 'FOF3 TEST PASSED'; then
    status=ok
  elif echo "$out" | grep -q 'FOF3 STATS MODE COMPLETE'; then
    status='ok(skip)'
  else
    status='!!FAILED'
  fi
  printf '%-6s %-6s %-7s %14s %9s %8s %-28s %s\n' \
    "$P" "$PPN" "$NPE" "${total:-ERR}" "${ratio:-ERR}" "${walk:-ERR}" "${bal:-ERR}" "$status"
  # Echo the full stat blocks per run: the tabulated columns are a summary,
  # but the phase division (all time_s lines), the per-PE skew (balance
  # lines), and the concentration histogram have each been needed after the
  # fact (design/step3.md 6h twice) — never make the logs the only copy.
  echo "$out" | grep -E 'FOF3STAT (time_s|balance|redundancy_concentration|edges|fragments|config):' | sed "s/^/# P=$P /"
  sleep "$SETTLE"
done
