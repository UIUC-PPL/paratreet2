# SCALING to 64 and 128 nodes — the split's benefit GROWS with scale, and
# phase 1 stops being the bottleneck
# Job 5288946, ONE 128-node allocation with srun steps at 16 / 64 / 128 nodes,
# 2 reps each, 24 arms. **ALL EXACT (424897832) at every node count.**
# `bin/FoF3.2b.pesets4`, patches/0010-0012. 2B particles throughout.

## 1. Headline

| N | serial base | dist base | **dist s=14, S3 off** | serial s=14, S3 off |
|---|---|---|---|---|
| 16 | 6409.4 | 6259.1 | **5368.6 (−16.2%)** | 7588.1 |
| 64 | 4384.0 | 3491.1 | **2770.9 (−36.8%)** | 7369.1 |
| 128 | 5446.1 | 4828.7 | **3826.1 (−29.7%)** | 9831.4 |

Iteration 0, ms, against the serial no-split baseline at the same N.

**The benefit grows with scale: −16% at 16 nodes, −37% at 64.** And on phase 1
itself the split removes an increasing share:

| N | phase1 serial base | phase1 dist s=14 | Δ |
|---|---|---|---|
| 16 | 3.325 | 2.069 | −37.8% |
| 64 | 1.753 | 0.557 | −68.2% |
| 128 | 1.249 | 0.281 | −77.5% |

Phase 1 in the recommended configuration scales close to linearly:
2.069 → 0.557 → 0.281 s, i.e. **3.71x on 4x nodes, then 1.98x on 2x nodes.**

## 2. The three predictions, checked

All three were written down in `pe-sets-dist-results.md` §6 before this job ran.

**(a) "Serial-mode union-find should break down further." CONFIRMED, strongly.**

| N | serial s=14 uf2 | dist s=14 uf2 | gap |
|---|---|---|---|
| 16 | 2.062 | 0.655 | 1.41 s |
| 64 | 3.814 | 0.723 | 3.09 s |
| 128 | 5.068 | 1.048 | 4.02 s |

The gather-to-root union-find grows 2.5x from 16 to 128 nodes while the
distributed one grows 1.6x, and the gap nearly triples. `serial s=14` at 128
nodes takes 9831 ms — worse than doing nothing at all. The same trend shows in
the unsplit arms: serial base uf2 0.443 → 0.904 → 1.317, dist base uf2
0.479 → 0.484 → 0.602.

**(b) "dist should improve relative to serial." CONFIRMED.**
On the unsplit baseline, dist is worth −2.3% at 16 nodes, **−20.4% at 64**, and
−11.3% at 128. Its advantage is small where the campaign has been working and
large at the scales it was heading for.

**(c) "phaseA per process should fall; does the plateau survive?" BOTH.**

| N | procs | top-5 pa_max_s | median | max/median |
|---|---|---|---|---|
| 16 | 128 | 1.995 1.898 1.882 1.880 1.879 | 1.224 | 1.63x |
| 64 | 512 | 0.538 0.521 0.519 0.519 0.518 | 0.280 | 1.92x |
| 128 | 1024 | 0.265 0.261 0.258 0.252 0.247 | 0.135 | 1.96x |

The magnitude collapses — 2.0 s → 0.27 s — and phaseA scales essentially
perfectly. **The plateau SHAPE survives** (the top five are still a flat
shoulder at every scale) and the imbalance RATIO even worsens slightly, 1.63x
to 1.96x. But it no longer matters: 0.265 s of a 3826 ms iteration.

## 3. THE MAIN CONCLUSION: phase 1 is no longer the bottleneck

Iteration-0 breakdown for the recommended configuration:

| N | Tree build | Pre-traversal | Tree traversal | phase 1 | phase 3 | Iter0 |
|---|---|---|---|---|---|---|
| 16 | 997 | 2438 | 1644 | 2.069 s | 1.170 s | 5346 |
| 64 | 217 | 1023 | 1346 | 0.557 s | 1.057 s | 2713 |
| 128 | 191 | 1574 | 1987 | 0.281 s | 1.567 s | 3942 |

At 128 nodes phase 1 is **0.281 s of a 3942 ms iteration — 7%.** The campaign's
target has been optimised out of relevance at scale. What is left is
Pre-traversal (which at 128 nodes is 1.29 s of cache load and setup ON TOP of
phase 1) and Tree traversal.

**And that is why 128 nodes is worse than 64.** Iteration 0 goes 2713 → 3942
for the recommended config, but phase 1 goes 0.557 → 0.281 — it is still
improving. The regression is entirely in Pre-traversal (1023 → 1574) and
Tree traversal (1346 → 1987), i.e. framework costs that grow with process
count. The same regression hits the serial baseline (4384 → 5446), so it is
not caused by the split.

**2B particles on 128 nodes is 15.6M particles per node — this problem has
strong-scaled out at 64 nodes.** Going further needs either a larger dataset or
work on the cache-load/traversal path, and neither is a phase-1 question.

## 4. Recommendation

```
-u dist   FOF_PE_SETS=14   FOF_PE_SETS_MODE=1   FOF_S3=0
```
(`FOF_PE_SETS=7 MODE=1` is within noise of it; see `pe-sets-ssweep.md`.)

Best absolute result measured: **64 nodes, 2771 ms**, against 4384 ms for the
serial baseline at the same node count and 6409 ms for the 16-node baseline the
campaign started from.

## 5. Where the campaign now stands

- phaseB: **eliminated** (0.001 s at every scale).
- phaseA: scales near-perfectly and is now 7% of the iteration at 128 nodes.
  The 19-process plateau survives in shape but not in importance.
- The union-find: solved by `-u dist`; the serial path should be considered
  unusable with the split at 64 nodes and above.
- The next bottleneck, if anyone wants one, is **Pre-traversal + Tree
  traversal**, which together are 90% of Iteration 0 at 128 nodes and are
  growing with process count. That is a different subsystem from anything this
  campaign has touched.
