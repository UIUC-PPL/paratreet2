# 80M speedup, current build (2026-08-07)

Same protocol as design/speedup-campaign-2026-08-05.md — Anvil
wholenode, 8 processes per node, 15 worker threads each, 4 serial-mode
repetitions plus 1 distributed-mode repetition per node count, lambb.00500
(80,621,568 particles) — on the build carrying every optimization since:
both walk prunes (ownership and symmetry), credit-counter walk
termination in serial mode, aggregation off with mid-walk streaming in
distributed mode, phaseB stealing v5, component counting folded into the
phaseA freeze pass, and the diagnostic passes gated off.

Correctness: every one of the 25 runs reported 23,707,197 components.

Tables and per-run logs: results/speedup-2026-08-07/ (80m-table.txt,
80m-stages.csv, 80m-medians.csv, logs/). Regenerate with
results/make-speedup-tables.py.

## Against the earlier build, same node counts (iteration total, seconds)

| nodes | now | 2026-08-05 | gain |
|---|---|---|---|
| 1 | 2.033 | 4.478 | 2.20 |
| 2 | 1.313 | 2.396 | 1.83 |
| 4 | 0.997 | 1.962 | 1.97 |
| 8 | 1.023 | 1.829 | 1.79 |
| 16 | 1.636 | 2.068 | 1.26 |

Roughly a factor of two at fixed node count, which is what the removed
work predicts: the counting pass, the mirrored-pair walk, the
local-versus-local sweep and the quiescence rounds were per-run costs,
not scaling costs. The 16-node gain is smaller because what dominates
there is the fixed and anti-scaling stages, which this work did not
touch.

## Per-stage medians (seconds; speedup against 1 node of this build)

| stage | 1 | 2 | 4 | 8 | 16 | 16-node speedup |
|---|---|---|---|---|---|---|
| read | 0.257 | 0.148 | 0.116 | 0.093 | 0.074 | 3.5 |
| key sort | 0.106 | 0.068 | 0.040 | 0.039 | 0.028 | 3.9 |
| decomposition | 1.869 | 1.205 | 0.603 | 0.722 | 1.311 | 1.4 |
| tree build | 0.350 | 0.167 | 0.095 | 0.079 | 0.209 | 1.7 |
| phase 1 total | 0.998 | 0.564 | 0.288 | 0.168 | 0.229 | 4.4 |
| — phaseA | 0.839 | 0.424 | 0.212 | 0.103 | 0.079 | **10.6** |
| — phaseB | 0.112 | 0.138 | 0.077 | 0.055 | 0.085 | 1.3 |
| — relabel | 0.121 | 0.069 | 0.039 | 0.034 | 0.068 | 1.8 |
| upward pass | 0.071 | 0.046 | 0.069 | 0.035 | 0.170 | 0.4 |
| load cache | 0.001 | 0.002 | 0.009 | 0.012 | 0.051 | 0.02 |
| phase-3 walk | 0.092 | 0.093 | 0.100 | 0.162 | 0.147 | 0.6 |
| edge gather | 0.001 | 0.007 | 0.001 | 0.042 | 0.067 | 0.01 |
| union-find | 0.004 | 0.006 | 0.009 | 0.014 | 0.023 | 0.2 |
| global relabel | 0.164 | 0.109 | 0.080 | 0.134 | 0.188 | 0.9 |
| **iteration total** | **2.033** | **1.313** | **0.997** | **1.023** | **1.636** | **1.2** |

phaseA now scales 10.6x over a 16x node range (7.8x on the earlier
build). The phase-3 walk holds at 0.09-0.16 s at every scale, an
absolute reduction of 3-4x from the prunes. The union bracket is
0.004-0.023 s everywhere — the quiescence-free serial path.

## Where the remaining time is

The iteration total bottoms out near 1.0 s at 4-8 nodes and rises at 16.
Everything that scales is now small; what is left is fixed or
anti-scaling:

- decomposition, 0.60 s at 4 nodes rising to 1.31 s at 16 — the largest
  single stage at every node count past 2, and the standing agenda item.
- load cache (0.001 -> 0.051 s) and edge gather (0.001 -> 0.067 s),
  both growing with process count; the starter-pack growth is the same
  effect Ritvik's Frontier sweep shows much more strongly at 2B.
- upward pass, flat-to-growing (0.071 -> 0.170 s).
- phaseB, flat at 0.055-0.14 s — the 80M pools are too small for the
  imbalance that dominates at 2B.

## Repetition spread (serial iteration totals, seconds)

    n1:  1.93 2.00 2.07 2.23
    n2:  1.17 1.31 1.32 1.73
    n4:  0.89 0.98 1.01 1.42
    n8:  0.60 0.65 1.39 1.88
    n16: 1.46 1.47 1.80 1.86

The 8-node point is bimodal (two runs near 0.6 s, two near 1.6 s), so
its median is not a stable estimate and the 4-node and 8-node points
should be read as "about one second" rather than ranked against each
other. The bimodality is the same episodic stall signature the
distributed union-find bracket showed at this scale; four repetitions
per point cannot separate it. The single distributed-mode repetitions
(2.03 / 1.43 / 0.80 / 0.59 / 2.05 s) sit inside the same spread and are
not comparable to the serial medians run-for-run.
