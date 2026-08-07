# FoF3 2B-particle node-count scaling (2026-08-07, rerun against v5 phaseB stealing rebuild)

Cosmo25 dataset (`cosmo25cmb.768g2_dm.001024`, N = 1,981,808,640 particles),
OctDecomp, `-u dist`, b_factor 0.2, 8 processes/node, 14 PEs/process
(1792 PEs at 16 nodes, scaling linearly with node count). Frontier, LCI/CXI
`reconverse` backend, `job_vni` + `cray_shasta`, `+lci_ndevices 7`,
`+backend_poll_thread 2`, `FOF_STEAL=1 FOF_STEAL_GROUP=8` explicit. Auto
mode falls back to STATS MODE at this N (> 20M threshold for the O(n^2)
crosscheck), so these runs report component counts without full
brute-force verification; determinism (below) is the substitute
correctness check.

Loading/decomposition time is excluded below (I/O-bound, not representative
of the algorithm) — all phases from tree build onward.

This is a rerun after a further paratreet2 rebuild: HEAD moved to
`43ef016`, seven source commits ahead of the previous rerun's HEAD
(`7a1b8c5`): `9656038` (v5 ship-once-use-many, deduplicated subtree blobs),
`76e9f90` (full-flow accounting, every process/denial reason), `25df4d1`
(serialize probe rounds), `4a964c9` (FOF_POOL_DEPTH), `d584de8` (helpers
ask directly instead of probing first), `d0332ae`/`5a916fc` (FOF_POOL_SPLIT_SIZE,
default off), `589c91b` (parallel pool build). design/phaseb-offload.md
sections 11-12 (added in this same rebuild) already contain the punchline:
round 4 found the donor's own flatten/ship cost was the real ceiling (v5
fixes that specific waste), and round 8 (direct-ask helpers, no probing)
found the deeper issue — the phaseB wall is dominated by *indivisible*
work a handful of threads already claimed and are grinding through, not
work stuck on the wrong process. Stealing protocol refinements including
v5 were expected NOT to move this sweep's phaseB wall, and that is exactly
what's measured below. Same 6-job-equivalent sweep as prior reruns:
`+traceoff` except 16 nodes, which keeps projections tracing. Prior
projections directory moved aside (not deleted): `fof3_projections.pre_v5_*`.

## Correctness

**424,897,832 components, identical at every node count (8/16/32/64/128)**
— unchanged from all four prior sweeps.

## Top-line scaling

| Nodes | PEs | Tree build (ms) | phase1 total (s) | TreeCanopy cache loading (ms) | Tree traversal (ms) | Iteration 0 total (ms) |
|------:|-----:|------:|------:|------:|------:|------:|
| 8   | 896   | 2306.9 | 11.10 | 12084.9 | 2031.1 | 16929.6 |
| 16  | 1792  | 988.8  | 8.82  | 9343.6  | 1580.5 | 12217.1 |
| 32  | 3584  | 408.7  | 8.05  | 8408.4  | 1289.4 | 10307.4 |
| 64  | 7168  | 208.0  | 6.52  | 6986.7  | 1270.5 | 8616.0  |
| 128 | 14336 | 212.8  | 5.86  | 7098.1  | 1438.7 | 8882.1  |

Same shape as all four prior sweeps, within run-to-run noise throughout.
No regression, no improvement from the v5 rebuild at the top line.

## phase1_stages breakdown (seconds)

| Nodes | reset | register | phaseA | phaseB | merge | relabel |
|------:|------:|---------:|-------:|-------:|------:|--------:|
| 8   | 0.001 | 0.000 | 5.247 | 6.057 | 0.014 | 0.675 |
| 16  | 0.001 | 0.001 | 2.638 | 6.422 | 0.011 | 0.296 |
| 32  | 0.002 | 0.001 | 1.343 | 7.216 | 0.008 | 0.146 |
| 64  | 0.003 | 0.001 | 0.612 | 6.203 | 0.006 | 0.068 |
| 128 | 0.006 | 0.002 | 0.301 | 5.675 | 0.004 | 0.031 |

**phaseB is still flat (5.7-7.2 s), still the dominant term past 8 nodes,
and within noise of every prior sweep** (original implicit default
6.1-7.6s; v3-explicit 5.8-7.3s; v4 rebuild 6.1-7.7s; v5 here 5.7-7.2s).
Four independent stealing-protocol generations, same floor. This matches
design/phaseb-offload.md section 12's diagnosis exactly: the wall is
indivisible work a process's own threads claim and grind through in the
first ~200ms after its pool exists, not misplaced work — no amount of
protocol refinement (targeting, batching, ship-once-dedup) touches that.
The doc's stated next lever is making units divisible (FOF_POOL_SPLIT_SIZE
/ FOF_POOL_DEPTH, design/agenda.md item 4), not further steal tuning.

## phase1 detail and phase3 (uf2/walk) breakdown (seconds)

| Nodes | tip_encode | upwardPass | loadCache | uf2_setup | phase3_walk | edge_gather | uf2 | relabel(p3) | component_histogram |
|------:|-----------:|-----------:|----------:|----------:|------------:|------------:|----:|------------:|---------------------:|
| 8   | 0.544 | 0.974 | 0.008 | 0.012 | 0.382 | 0.001 | 0.553 | 0.372 | 0.600 |
| 16  | 0.272 | 0.499 | 0.025 | 0.023 | 0.319 | 0.001 | 0.658 | 0.186 | 0.336 |
| 32  | 0.130 | 0.273 | 0.075 | 0.047 | 0.272 | 0.002 | 0.650 | 0.101 | 0.186 |
| 64  | 0.057 | 0.194 | 0.258 | 0.095 | 0.265 | 0.003 | 0.698 | 0.054 | 0.134 |
| 128 | 0.031 | 0.240 | 0.972 | 0.193 | 0.249 | 0.006 | 0.829 | 0.030 | 0.117 |

Same pattern as all prior sweeps: tip_encode scales cleanly. loadCache
still grows with node count (0.008 -> 0.972 s), still tracked in
design/agenda.md's loadCache anti-scaling item. Everything else shrinks
or is flat.

## Cross-run comparison, phaseB wall (seconds), all five sweeps on this machine

| Nodes | Original (implicit) | v3 explicit | v4 (probe-target) | v5 (ship-once, this run) |
|------:|------:|------:|------:|------:|
| 8   | 6.301 | 6.027 | 6.346 | 6.057 |
| 16  | 6.544 | 6.308 | 6.839 | 6.422 |
| 32  | 7.562 | 7.285 | 7.733 | 7.216 |
| 64  | 6.664 | 6.239 | 6.703 | 6.203 |
| 128 | 6.074 | 5.791 | 6.104 | 5.675 |

Five sweeps across four steal-protocol generations, same 5.7-7.7 second
band throughout. design/phaseb-offload.md's own conclusion (section 12) is
that this is now expected: stealing is correct and cheap but was never the
lever for this floor once the donor-cost issue (section 11) was fixed —
the wall is a small number of indivisible units a process's own threads
grab and grind on before any helper can act.

## Steal denial breakdown (v5 full-flow accounting, `FOF3STAT stealacct:` lines)

Unlike the previous (v4) rerun, every process now prints a `stealacct` line
(commit `76e9f90`, full-flow accounting). Aggregated denial reasons:

| Nodes | `ready` denials (donor pool not built yet) | `needy` denials | `drained` denials (donor already claimed everything) | grants |
|------:|------:|------:|------:|------:|
| 8   | 81,479 | 0 | 6,272   | 8   |
| 16  | 82,682 | 0 | 12,544  | 45  |
| 32  | 84,254 | 0 | 25,088  | 46  |
| 64  | 80,810 | 0 | 50,176  | 79  |
| 128 | 86,854 | 0 | 100,352 | 237 |

This is a direct, first-hand confirmation of design/phaseb-offload.md
section 12's round-8 diagnosis: `ready` (the donor's pool doesn't exist yet
because it's still in phaseA) and `drained` (the donor's own threads
already claimed everything) together account for essentially all denials,
`needy` (the only reason a live scheme could route around) is zero at
every scale, and grants stay a tiny fraction of total requests even at 128
nodes. Stealing has nothing to redistribute in either failure mode — the
work is either not born yet or already spoken for.

## Raw command (per node count N, procs/node = 8 fixed)

```
export FOF_STEAL=1
export FOF_STEAL_GROUP=8
srun --mpi=cray_shasta --network=<job_vni | single_node_vni for N=1> --unbuffered \
  --cpu-bind=none --distribution=block:block \
  ./FoF3 -f <input> -d oct -u dist -b 0.2 +ppn 14 \
  +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
  +lci_ndevices 7 +backend_poll_thread 2 [+traceroot <dir> +logsize 100000000 | +traceoff]
```

Job IDs (2B sweep, this rerun): 8n=5194631, 16n=5194638 (only run with
projections tracing), 32n=5194642, 64n=5194645, 128n=5194648.
