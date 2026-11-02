# Phase-3 walk imbalance, per piece (issue #5)

2026-11-02. FoF3 on the 2B cosmo25 snapshot, shipping GPU config (device
phase 1, leaf 128), 8 processes x 7 PEs per node, weighted tie-break and
union spread (the defaults), paratreet2 3d44061 plus the instrument below.

## Why look per piece

At 128 nodes the phase-3 walk is 0.25 s of a 0.60 s iteration and flat in
node count. The per-PE stats (job 5834408) already showed it is not
latency-bound: the average PE is busy 18 ms in the walk, the busiest 126-175
ms, with leaf interactions 6-8x the average on the hottest PE. The weighted
tie-break (iteration 1) only moves each cross-piece pair between its two
owning pieces, and same-process pairs are pruned, so it cannot move work off
a PE whose neighbours are hot too. Which fix applies depends on where the
busiest PE's time sits, which the per-PE stats cannot show.

## Instrument

`PARATREET_WALK_PIECES` (off by default; README knob table).
`DualTraverser::doTrav` charges its busy time, its fetches and the visitor's
work units (`ThreadStateHolder::walk_work`; FoF adds n_src x n_tgt per leaf
visit) to the TreePiece it is walking. After the walk FoF3 gathers one
48-byte record per piece (concat reduction), prints a summary and the
what-ifs below, and optionally writes every record to a CSV.

What-ifs, computed from the measured piece times (they bound the busy
time of the busiest PE, not the walk's wall time):

- `lpt_in_process`: pieces reassigned among their own process's 7 PEs,
  longest first to the least-loaded PE. Those PEs share the process's cache,
  so this needs no extra fetches and leaves phase-1 ownership unchanged.
- `round_robin_in_process` (offline script only): each process deals its
  pieces over its PEs in index order. Static, needs no measurement.
- `lpt_global`: pieces reassigned across all PEs; its floor is the largest
  piece.

Checks: per-piece busy sums to the existing per-PE `walk_timing` busy max
exactly (2-node lambb, jobs 5836083 and 5836132), walk times unchanged with
the instrument on, every run on the gold digest.

## Measured, 2B cosmo25 (jobs 5836309 at 128 nodes, 5836310 at 64)

Two instrumented runs per node count, all gold (label_digest
2e8eaba371ff1d31). Iteration 1, the weighted tie-break, busy time in ms:

| nodes | PE avg | busiest PE | round-robin in process | LPT in process | LPT global (= largest piece) |
|---:|---:|---:|---:|---:|---:|
| 128 | 19.6 | 150 / 173 | 86 | 64 | 35 |
| 64 | 30.0 | 248 / 301 | 152 | 95 / 107 | 65 / 79 |

Iteration 0 (parity) at 128 nodes: busiest PE 222 ms, LPT in process 70 ms.
The walk's wall time in these runs is 0.25-0.27 s at 128 nodes and 0.31-0.39
s at 64 (iteration 1).

What the per-piece data shows:

- **The busiest PE's time is spread over many pieces, not one.** At 128
  nodes the busiest PEs hold 20-44 pieces, and the largest is 8-20% of
  the PE's walk time.
- **The hot pieces are many separate hot spots.** The 64 busiest pieces
  sit on 58 different processes in 58 index clusters (piece index is SFC
  order). Half of all walk time is in 5.8% of the pieces.
- **Within a process, granularity is not the limit.** LPT in process equals
  the hottest process's total divided by 7 to the printed digits. The
  hottest process's busiest PE is 2.0-2.2x its process average.
- **Leaf interactions are a good cost proxy and particle count is not:**
  over pieces, r(busy, leaf_ints) = 0.93-0.94, r(busy, fetches) 0.77,
  r(busy, particles) 0.32.

## What this says to do

1. Reassign walk work among each process's own PEs. That brings the
   busiest PE from 150-173 ms to about 64 ms at 128 nodes (2.3-2.7x), and
   from 248-301 to 95-107 ms at 64 nodes, with no extra communication.
   A static round-robin placement within the process gets about half that
   reduction (86 ms at 128 nodes, 152 at 64) with no measurement at all.
   Placement is `DecompArrayMap` (contiguous particle-balanced intervals
   per PE); `FOF_PE_SETS` is off by default, so placement within a process
   does not change the walk's work.
2. Going below the hottest process's average needs cross-process movement
   (LPT global: 35 ms at 128 nodes), with the largest piece as the floor.

The walk's wall time is the busiest PE's busy time plus ~0.1 s of waiting
at 128 nodes, so the iteration gains less than the busy what-ifs suggest.
Measure before quoting.

Files: CSVs, logs and `wpan.py` in
`/lustre/orion/csc710/scratch/rrao/walkpieces-2026-11-02/` (scratch, purged
eventually); the analysis script is also `examples/fof3/scripts/walkpieces.py`.

## Tried: spreading pieces within the process (PARATREET_TP_PLACEMENT)

`DecompArrayMap` now takes `PARATREET_TP_PLACEMENT` (default `block`, the
contiguous particle-balanced intervals). Both alternatives keep every piece
on its block PE's process and change only the PE within it:

- `rr`: the process's pieces dealt round-robin in index order. Per-PE
  particle counts become 1.37-1.60x the average (block: 1.09-1.12x), and
  tree build, which is per-PE work, slows from 90-95 to 109-115 ms at 2
  nodes (lambb, jobs 5836727 and 5836833).
- `spread`: in index order, each piece to the PE holding the fewest
  particles so far. Neighbouring pieces still land on different PEs, and
  the per-PE particle balance is as good as block's (1.06-1.10x). Offline
  it matches or beats `rr` on the walk (128 nodes: 86 ms busiest PE, both).

2B cosmo25 at 128 nodes, arms interleaved, 2 reps each, means (job 5836861,
all gold):

| | iteration 1 (weighted), block -> spread | iteration 0 (parity), block -> spread |
|---|---|---|
| walk busiest PE | 146 -> 95 ms | 192 -> 106 ms |
| walk end_max (last PE out) | 0.215 -> 0.206 s | 0.286 -> 0.227 s |
| qd_tail (union drain after it) | 0.040 -> 0.052 s | 0.001 -> 0.045 s |
| phase3_walk | 0.255 -> 0.258 s | 0.287 -> 0.272 s |
| walk + uf2 | 0.284 -> 0.284 s | 0.317 -> 0.299 s |
| tree build | 55 -> 56 ms | 66 -> 67 ms |
| iteration | 615 -> 622 ms (0.99x) | 703 -> 686 ms (1.03x) |

The placement did what the what-ifs said it would to compute: the busiest
PE's walk time falls 1.5x in iteration 1 and 1.8x in iteration 0 (predicted
86 ms; measured 93-97). The walk's wall time barely follows, and in the
production iteration not at all:

- **The last PE out of the walk is set by waiting, not compute.** With the
  busiest PE at 95 ms, PEs still leave the walk at ~0.2 s. Spans (first
  entry to last exit) stay near 0.2 s, so the critical path is the time
  pieces spend waiting on remote fetches and resumes, not their busy time.
- **What the walk gained in iteration 0 moved into the union drain.** The
  last PE left 59 ms earlier, but the streamed UF_2 cascades were still
  running, so qd_tail grew from 1 to 45 ms.

So `spread` stays opt-in. It does no harm (tree build unchanged) and may
matter once the waiting is shorter, but it is not a lever on its own.

What to look at next: what keeps a piece in the walk for ~0.2 s when its
busy time is a fraction of that. Candidates, in order of cost: per-piece
first-entry/last-exit times and resume counts (extend the instrument), to
find the chain of dependent fetches on the critical path; then fetch
granularity (`-D`, swept only at 16 nodes, where requests were already
the expensive term at 14.9 us each).
