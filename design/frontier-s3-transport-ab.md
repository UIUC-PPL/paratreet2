# Frontier instructions: S3 transport A/B at 2B (arena rebuild vs per-node malloc)

For the Frontier Claude session, 2026-08-13 afternoon. Follows your
s3-reserve-2b report — all its recommendations were adopted the same
morning (defaults changed at 1996ebd; reservation default-off), and the
sum-detail trace of 5250364 was analyzed on the laptop
(design/phaseab-balancing.md section 31 + addenda): the serial
s3Shipment rebuild (473 calls, mean 59 ms, max 423 ms, one entry
method, per-node malloc) dominates each shipment's makespan — the
drain itself is only ~8 ms of wall once fanned out.

## What changed (b797e73, Kale's design)

StealTree wire nodes now carry parent-index + child-slot integer
offsets (absent-slot key-0 records dropped — messages shrink); the
helper rebuilds each tree with ONE arena allocation and a single
linear placement-new pass (child/parent pointers = arena + index).
Laptop gates green both runtimes, including loopback zero-mismatch at
100k and 1M. This job measures whether killing the serial head moves
shipped volume and the straggler at 2B.

## Build — TWO binaries, transport is the only delta

Both commits already carry the new defaults (GRANT_M2=1e10,
GRANT_UNITS_PER_PE=128, RESERVE off), so the A/B isolates transport:

1. `git fetch && git checkout phaseab-campaign && git pull` → tip
   (4600a60 or later). Clean rebuild (production charm), stage as
   `FoF3.2b.wire`.
2. `git checkout d5c24f4` (same everything, old transport). Clean
   rebuild, stage as `FoF3.2b.prewire`. Then return the tree to the
   branch tip.
Verify both untraced (TraceSummary syms = 0). 10k gate each binary
(3549) before 2B.

## Arms — 16 nodes, 2B, -u serial, best cell

Env on every arm: your standard base set + `FOF_PB_PARTS=16`
`FOF_PHASEB_SLICE_MS=2` (GRANT knobs now default right; no FOF_S3_GRANT
overrides). Interleave:

| arm | binary | extra env | reps |
|---|---|---|---|
| base | wire | — | 1 |
| s3-prewire | prewire | FOF_S3=1 | 2 |
| s3-wire | wire | FOF_S3=1 | 2 |

Expected reference (your 5253475 best-noreserve = functionally the
prewire arm): phaseB_s max 1.572/1.637, Iter0 7.138/7.182, 566/551
units-per-ship, 38% of pool m2 moved.

## Optional second job (if the traced stack is convenient): sum-detail
pair — one s3-prewire + one s3-wire run with traced binaries — the
DIRECT readout of s3Shipment entry time (5250364 baseline: 473 calls /
27.8 s total, but at GRANT=32; your prewire trace is the matched
baseline at GRANT=128). This also serves the standing
one-sumdetail-per-allocation policy.

## Readout (per run)

- EXACTNESS FIRST: 424897832 every run; 10k gates at 3549.
- phaseB_s min/avg/max — does the straggler max drop below 1.57
  toward the 0.25 s floor?
- Wall rows (Pre-traversal, Iteration 0) and phase1_stages.
- s3 totals: out_ships, out_units, units/ship, out_m2/tot_m2,
  declines. HYPOTHESIS: if the serial rebuild was throttling grant
  turnaround, s3-wire ships MORE volume (higher out_m2/tot_m2, lower
  declines) at similar or better wall; if numbers are flat, the
  binding constraint is elsewhere (order->return serialization,
  section 26 lever 3) and that is the next lever.
- Sum-detail pair, if run: s3Shipment total ms / calls / mean —
  prewire vs wire; also drainForeign start times (staggering).

## Reporting

Single markdown in ~/software/reports/ with commit hashes, job ids,
srun line, per-arm tables, and the pre/post comparison against
5253475. Anomalies verbatim.
