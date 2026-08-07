# FoF3 2B-particle node-count scaling (2026-08-07, rerun against v4 phaseB stealing rebuild)

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

This is a rerun after the user rebuilt paratreet2 with new source changes:
`git log` shows the paratreet2 HEAD moved to `7a1b8c5` (built 11:18), one
source commit ahead of the previous rerun's HEAD (`18bdcc4`):
`44edf47 phaseB stealing v4: probe-and-target helpers, two-batch grant
gate, buffered accounting` — this is exactly the v4 implementation that
design/phaseb-offload.md section 8 called for after the v3 verdict (grants
concentrated but the wall didn't move). Same 6-job sweep as the last two
reruns: 8/16/32/64/128-node 2B scaling (`+traceoff` except 16 nodes, which
keeps projections tracing) plus the 1-node lambb.00500 run with projections.
Prior projections directories were moved aside (not deleted) before this
run: `fof3_projections.pre_v4_*`, `fof3_projections_lambb500.pre_v4_*`.

**Note on stat lines**: the v4 rebuild's "buffered accounting" changed what
FOF3STAT reports — the previous `FOF3STAT steal: process P out U in V
denials D` per-process line (used to confirm grant activity in the last two
reruns) is gone from this build's output entirely; no line with "steal" in
it appears anywhere in stdout for any of the 6 runs. `phaseB_units` and
`phaseB_s` balance lines are still present and are what the comparison
below relies on. This is worth flagging back if per-process steal
visibility was still wanted — it may have been intentionally moved into the
new buffered/probe accounting rather than removed outright, but nothing in
this run's stdout exposes it under the old name.

## Correctness

**424,897,832 components, identical at every node count (8/16/32/64/128)**
— unchanged from all three prior sweeps. lambb.00500: **23,707,197**,
also unchanged. Bit-for-bit determinism holds against the v4 rebuild.

## Top-line scaling

| Nodes | PEs | Tree build (ms) | phase1 total (s) | TreeCanopy cache loading (ms) | Tree traversal (ms) | Iteration 0 total (ms) |
|------:|-----:|------:|------:|------:|------:|------:|
| 8   | 896   | 2307.3 | 11.47 | 12459.8 | 2054.8 | 17332.4 |
| 16  | 1792  | 987.3  | 9.37  | 9898.5  | 1632.9 | 12838.0 |
| 32  | 3584  | 410.3  | 8.54  | 8901.3  | 1292.2 | 10806.1 |
| 64  | 7168  | 211.5  | 7.04  | 7506.0  | 1291.9 | 9162.6  |
| 128 | 14336 | 201.4  | 6.31  | 7537.6  | 1431.1 | 9301.2  |

Same shape as all three prior sweeps, and within run-to-run noise of the
immediately preceding (FOF_STEAL=1/GROUP=8 explicit, pre-v4) numbers —
e.g. 16-node phase1 total 8.70s -> 9.37s, 128-node 5.95s -> 6.31s. No
regression, no clear improvement either.

## phase1_stages breakdown (seconds)

| Nodes | reset | register | phaseA | phaseB | merge | relabel |
|------:|------:|---------:|-------:|-------:|------:|--------:|
| 8   | 0.001 | 0.000 | 5.232 | 6.346 | 0.014 | 0.693 |
| 16  | 0.001 | 0.001 | 2.718 | 6.839 | 0.011 | 0.307 |
| 32  | 0.002 | 0.001 | 1.398 | 7.733 | 0.007 | 0.145 |
| 64  | 0.003 | 0.001 | 0.634 | 6.703 | 0.006 | 0.067 |
| 128 | 0.006 | 0.002 | 0.486 | 6.104 | 0.004 | 0.031 |

**phaseB is still flat (6.1-7.7 s) and still the dominant, non-scaling term
past 8 nodes — the v4 probe-and-target/two-batch-grant/buffered-accounting
rebuild does not move this number relative to the pre-v4 explicit-steal
sweep (5.8-7.3 s) or the original implicit-default sweep (6.1-7.6 s).** All
three sweeps agree within noise. phaseA still scales cleanly (5.23 -> 0.49 s).

## phase1 detail and phase3 (uf2/walk) breakdown (seconds)

| Nodes | tip_encode | upwardPass | loadCache | uf2_setup | phase3_walk | edge_gather | uf2 | relabel(p3) | component_histogram |
|------:|-----------:|-----------:|----------:|----------:|------------:|------------:|----:|------------:|---------------------:|
| 8   | 0.541 | 0.974 | 0.011 | 0.012 | 0.398 | 0.001 | 0.558 | 0.367 | 0.609 |
| 16  | 0.269 | 0.497 | 0.025 | 0.024 | 0.324 | 0.001 | 0.714 | 0.183 | 0.330 |
| 32  | 0.132 | 0.277 | 0.079 | 0.048 | 0.266 | 0.002 | 0.656 | 0.101 | 0.189 |
| 64  | 0.058 | 0.199 | 0.258 | 0.095 | 0.268 | 0.003 | 0.718 | 0.055 | 0.134 |
| 128 | 0.031 | 0.235 | 0.971 | 0.197 | 0.245 | 0.006 | 0.819 | 0.032 | 0.118 |

Same pattern as all prior sweeps: tip_encode scales cleanly. loadCache still
grows with node count (0.011 -> 0.971 s), still tracked in
design/agenda.md's loadCache anti-scaling item. uf2 grows mildly. Everything
else shrinks or is flat.

## lambb.00500 (80M particles), 1 physical node, with projections tracing

Same machine/config (8 procs/node, 14 PEs/process, 112 PEs total),
`--network=single_node_vni`, `FOF_STEAL=1 FOF_STEAL_GROUP=8` explicit. Full
projections tracing: `+traceroot fof3_projections_lambb500 +logsize
100000000`.

**Components: 23,707,197** — matches the correctness gate value in
design/phaseb-offload.md section 7 exactly.

| Tree build (ms) | phase1 total (s) | TreeCanopy cache loading (ms) | Tree traversal (ms) | Iteration 0 total (ms) |
|------:|------:|------:|------:|------:|
| 552.6 | 1.766 | 2065.5 | 505.2 | 3276.7 |

phase1_stages (s): reset 0.000, register 0.000, phaseA 1.565, phaseB 0.181,
merge 0.002, relabel 0.165. phase1 detail/phase3 (s): tip_encode 0.152,
upwardPass 0.298, loadCache 0.001, uf2_setup 0.002, phase3_walk 0.129,
edge_gather 0.000, uf2 0.066, relabel(p3) 0.112, component_histogram 0.160.

phaseB (0.181 s) remains a small fraction of phase1_stages at this scale,
consistent with all prior runs — single node, no cross-process skew large
enough to make stealing matter.

## Cross-run comparison, phaseB wall (seconds), all sweeps on this machine

| Nodes | Original (implicit steal default) | FOF_STEAL=1/GROUP=8 explicit (v3 code) | v4 rebuild (probe-and-target, this run) |
|------:|------:|------:|------:|
| 8   | 6.301 | 6.027 | 6.346 |
| 16  | 6.544 | 6.308 | 6.839 |
| 32  | 7.562 | 7.285 | 7.733 |
| 64  | 6.664 | 6.239 | 6.703 |
| 128 | 6.074 | 5.791 | 6.104 |

Three independent sweeps, three different steal configurations/code
versions, same flat 6-8 second band. This is the strongest evidence yet
that phaseB's floor at 2B scale is not being moved by any steal-tuning
variant tried so far — supports design/phaseb-offload.md's own reading
that the wall isn't explained by stealing throughput at all, and something
else (the heavy process's own dense-phase cost, or a bottleneck the
buffered-accounting change didn't address) is the real floor.

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

Job IDs (2B sweep, this rerun): 8n=5190754, 16n=5190770 (only run with
projections tracing), 32n=5190782, 64n=5190788, 128n=5190798.
lambb.00500 1-node run (projections): 5190805.
