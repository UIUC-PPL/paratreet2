# Speedup campaign: 80M (1-16 nodes) and 2B (16 nodes), 2026-08-05

Jobs 19675406-19675410 on Anvil wholenode, 8 processes/node x 15 worker
threads, binary at main commit 4e15fd2, `-u serial` (4 reps, medians
below) plus one `-u dist` rep per point for comparison. Full per-run
logs: `clusterfinding/results-speedup80m/` and `results-speedup2b/` on
Anvil, mirrored at `~/software/clusterFinding/results/speedup-2026-08-05/`
on the laptop.

**Correctness cross-checks passed at every completed point**: serial and
dist agree bit-for-bit on the components line at all five 80M points
(23,707,197 components, max 1,519,203 — the single-process ground truth)
and at 2B/16 nodes (424,897,832 components, max 185,317,566).

## 80M per-stage medians (seconds; serial reps)

| stage | 1 node | 2 | 4 | 8 | 16 | speedup at 16 |
|---|---|---|---|---|---|---|
| read tipsy | 0.481 | 0.140 | 0.181 | 0.105 | 0.066 | 7.3 |
| assign keys + sort | 0.152 | 0.054 | 0.049 | 0.038 | 0.027 | 5.6 |
| decomposition total | 2.227 | 1.309 | 0.832 | 0.782 | 1.653 | 1.3 |
| tree build | 0.388 | 0.214 | 0.192 | 0.195 | 0.227 | 1.7 |
| phase 1 total | 0.931 | 0.517 | 0.297 | 0.244 | 0.238 | 3.9 |
| -- phaseA | 0.846 | 0.426 | 0.233 | 0.122 | 0.108 | 7.8 |
| -- phaseB | 0.097 | 0.119 | 0.067 | 0.073 | 0.074 | 1.3 |
| upward pass | 0.095 | 0.052 | 0.121 | 0.078 | 0.175 | 0.5 |
| load cache | 0.002 | 0.003 | 0.007 | 0.018 | 0.048 | 0.04 |
| uf2 setup | 0.007 | 0.001 | 0.007 | 0.007 | 0.025 | - |
| phase-3 walk | 0.532 | 0.337 | 0.327 | 0.448 | 0.286 | 1.9 |
| edge gather | 0.001 | 0.003 | 0.072 | 0.079 | 0.086 | - |
| serial union-find | 0.004 | 0.006 | 0.010 | 0.014 | 0.023 | - |
| global relabel | 0.048 | 0.038 | 0.105 | 0.098 | 0.184 | 0.3 |
| traversal wall | 2.428 | 1.229 | 1.024 | 0.918 | 0.897 | 2.7 |
| **iteration total** | **4.478** | **2.396** | **1.962** | **1.829** | **2.068** | **2.2** |

Reading: phaseA (the compute) scales at 7.8x/16 and the input stages
scale; the total saturates at 8 nodes and regresses at 16 because the
fixed-cost stages take over — decomposition (worst offender: 1.65 s at
16 nodes, anti-scaling), tree build (~0.2 s flat), the phase-3 walk
(~0.3 s flat: dominated by the local-vs-local certificate sweep that an
ownership prune would remove — the walk enumerates all pairs and prunes
same-process regions by certificates instead of skipping them), and the
broadcast-shaped tails (load cache, global relabel).

## Serial vs distributed union-find at scale (the dist rep per point)

| point | dist uf2_setup | dist uf2 | serial uf2_setup | serial uf2 |
|---|---|---|---|---|
| 80M, 1 node | 0.003 | 0.032 | 0.007 | 0.004 |
| 80M, 4 nodes | 0.125 | 0.027 | 0.007 | 0.010 |
| 80M, 8 nodes | 0.256 | 0.025 | 0.007 | 0.014 |
| 80M, 16 nodes | **1.239** | **3.717** | 0.025 | 0.023 |
| 2B, 16 nodes | 0.421 | 4.919 | 0.026 | 0.336 |

The distributed path's library-creation setup grows with process count
(it is the black-blob region of the projections timelines, now
measured), and its union bracket carries the LCI idle-stall tail (3.7 s
at 80M/16 despite the keep-alive ring being ON in all of these runs —
further evidence the ring does not suppress the stall in the
application's pattern). The serial gather bracket is flat and
stall-free at every point, as designed (no quiescence detection, two
bulk rounds). Its own scale cost is visible at 2B: the tip->label map
broadcast (global relabel 2.84 s) — the next thing to slim there is
sending each process only its own tips' entries.

## 2B at 16 nodes (serial medians, seconds)

read 2.86, sort 0.34, decomposition 8.77, tree build 1.23, phase 1 4.19
(phaseA 2.61, phaseB 2.73), upward 0.72, walk 1.94, serial union-find
0.34, global relabel 2.84, traversal wall 8.83, **iteration total 19.3**.

The phaseB spread (2.73 s here, ~20x the average on the heaviest
process per the earlier milestone) is the standing target of
design/phaseb-offload.md.

## 2B at 4 and 8 nodes: LCI packet-pool exhaustion (NOT an app bug)

Every 2B run at 4 and 8 nodes — serial AND dist — died during the
initial particle flush (before decomposition completed) with millions of
`refill_recvs: Deadlock alert! The device does not have any posted
recvs (packet pool size 0)` warnings and then an assert at
`backend_ibv_inline.hpp:poll_comp_impl:118`. Per-process data volume at
4 nodes is 4x that of the surviving 16-node runs; the flush traffic
exhausts LCI's receive packet pool. This belongs with the LCI handover
(alongside the idle-stall): likely fixable by raising the packet-pool
knobs at launch, untested. The 2B speedup curve therefore has one point
(16 nodes) until that is resolved.

## Follow-ups queued by this data

1. Ownership prune in the phase-3 walk (visitor trait: skip pairs whose
   source node is entirely process-local; safe by the phase-1
   completeness invariant the DEBUG tripwire asserts).
2. Decomposition anti-scaling at 16 nodes: profile the splitter
   computation and flush at 1920 PEs.
3. `-u serial` as the production default (dist stays for future need
   and beyond-gather scales), and with it retire the keep-alive ring.
4. Slim the serial relabel broadcast to per-process slices (2B cost).
5. phaseB cross-process balancing (design/phaseb-offload.md), using
   this campaign's phaseB columns as the baseline.
6. LCI: packet-pool exhaustion at 2B/4-8 nodes; idle-stall still
   visible in dist uf2 at 16 nodes with the ring on.
