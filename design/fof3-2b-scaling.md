# FoF3 2B-particle node-count scaling (2026-08-06, rerun after phaseb-offload.md/agenda.md pull)

Cosmo25 dataset (`cosmo25cmb.768g2_dm.001024`, N = 1,981,808,640 particles),
OctDecomp, `-u dist`, b_factor 0.2, 8 processes/node, 14 PEs/process
(1792 PEs at 16 nodes, scaling linearly with node count). Frontier, LCI/CXI
`reconverse` backend, `job_vni` + `cray_shasta`, `+lci_ndevices 7`,
`+backend_poll_thread 2`. Auto mode falls back to STATS MODE at this N
(> 20M threshold for the O(n^2) crosscheck), so these runs report component
counts without full brute-force verification; determinism (below) is the
substitute correctness check.

Loading/decomposition time is excluded below (I/O-bound, not representative
of the algorithm) — all phases from tree build onward.

This is a rerun of the original 2026-08-06 sweep after pulling
`8f22315`/`a80b8ca` (design/phaseb-offload.md stage-1-3 history,
design/agenda.md). Both pulled commits are docs-only; no source changed.
Checked as part of this rerun: `FOF_STEAL_GROUP` (must match tasks/node for
same-node steal targeting, per phaseb-offload.md section 7) defaults to 8
in `fof/FoFPhase1.h`, already matching this machine's 8-procs/node layout —
so the original sweep's phaseB numbers were not a group-size artifact.
Numbers below are within run-to-run noise of the original sweep; the
phaseB-flat finding reproduces.

All runs used `+traceoff` except the 16-node point, which carries full
projections tracing (`+traceroot fof3_projections +logsize 100000000`,
1794 files, 1.2 GB) — kept as the one detailed-trace reference point per
run instructions.

## Correctness

**424,897,832 components, identical at every node count (8/16/32/64/128)**
— unchanged from the original sweep. Bit-for-bit determinism across a 16x
range of parallelism is the crosscheck substitute that STATS MODE calls
for at this N.

## Top-line scaling

| Nodes | PEs | Tree build (ms) | phase1 total (s) | TreeCanopy cache loading (ms) | Tree traversal (ms) | Iteration 0 total (ms) |
|------:|-----:|------:|------:|------:|------:|------:|
| 8   | 896   | 2295.3 | 11.36 | 12347.0 | 2035.9 | 17186.2 |
| 16  | 1792  | 971.7  | 8.96  | 9489.8  | 1649.0 | 12414.1 |
| 32  | 3584  | 407.5  | 8.37  | 8736.3  | 1245.7 | 10589.7 |
| 64  | 7168  | 211.5  | 6.98  | 7421.1  | 1351.5 | 9139.0  |
| 128 | 14336 | 206.8  | 6.24  | 7475.6  | 1432.7 | 9247.2  |

Same shape as the original sweep: tree build scales cleanly (~11x over a
16x node range); phase1 total and TreeCanopy cache loading flatten hard
past 32 nodes (phaseB breakdown below is why); tree traversal and
iteration-0 total bottom out at 64 nodes and tick back up at 128, i.e. the
scaling ceiling for this problem size is still around 64 nodes.

## phase1_stages breakdown (seconds)

| Nodes | reset | register | phaseA | phaseB | merge | relabel |
|------:|------:|---------:|-------:|-------:|------:|--------:|
| 8   | 0.001 | 0.000 | 5.288 | 6.301 | 0.014 | 0.694 |
| 16  | 0.001 | 0.001 | 2.598 | 6.544 | 0.011 | 0.309 |
| 32  | 0.002 | 0.001 | 1.377 | 7.562 | 0.008 | 0.145 |
| 64  | 0.003 | 0.001 | 0.635 | 6.664 | 0.006 | 0.075 |
| 128 | 0.006 | 0.002 | 0.308 | 6.074 | 0.004 | 0.032 |

phaseA still scales (5.29 -> 0.31 s, ~17x over the sweep). phaseB is still
flat (6.1-7.6 s, no trend with node count) and still dominates
phase1_stages past 8 nodes — the phaseB-offload doc's stage-3+ (buddy-node
tier, affinity re-steals) is the next lever, since stage-1/2 (same-node
steals, already on main and confirmed correctly configured here) do not
move this number.

## phase1 detail and phase3 (uf2/walk) breakdown (seconds)

| Nodes | tip_encode | upwardPass | loadCache | uf2_setup | phase3_walk | edge_gather | uf2 | relabel(p3) | component_histogram |
|------:|-----------:|-----------:|----------:|----------:|------------:|------------:|----:|------------:|---------------------:|
| 8   | 0.548 | 0.979 | 0.009 | 0.012 | 0.372 | 0.001 | 0.554 | 0.371 | 0.615 |
| 16  | 0.270 | 0.499 | 0.024 | 0.023 | 0.317 | 0.001 | 0.723 | 0.190 | 0.337 |
| 32  | 0.134 | 0.277 | 0.082 | 0.049 | 0.262 | 0.002 | 0.599 | 0.105 | 0.198 |
| 64  | 0.059 | 0.196 | 0.235 | 0.092 | 0.303 | 0.003 | 0.744 | 0.055 | 0.136 |
| 128 | 0.034 | 0.233 | 0.976 | 0.189 | 0.311 | 0.006 | 0.767 | 0.030 | 0.114 |

Same pattern as before: tip_encode scales cleanly (~16x). loadCache still
grows with node count (0.009 -> 0.976 s, ~108x this run), still the
odd one out — see design/agenda.md's "loadCache anti-scaling" item, now
tracked there directly. uf2 grows mildly (0.55 -> 0.77 s). Everything else
shrinks or is flat.

## lambb.00500 (80M particles), 1 physical node, with projections tracing

Same machine/config (8 procs/node, 14 PEs/process, 112 PEs total),
`--network=single_node_vni` (1-node run, not `job_vni`). Full projections
tracing: `+traceroot fof3_projections_lambb500 +logsize 100000000` — 114
files, 26 MB.

**Components: 23,707,197** — matches the correctness gate value in
design/phaseb-offload.md section 7 exactly (`80M lambb.00500: 23,707,197`).

| Tree build (ms) | phase1 total (s) | TreeCanopy cache loading (ms) | Tree traversal (ms) | Iteration 0 total (ms) |
|------:|------:|------:|------:|------:|
| 546.2 | 2.098 | 2395.6 | 502.4 | 3596.3 |

phase1_stages (s): reset 0.000, register 0.000, phaseA 1.513, phaseB
0.265, merge 0.003, relabel 0.159. phase1 detail/phase3 (s): tip_encode
0.150, upwardPass 0.296, loadCache 0.001, uf2_setup 0.002, phase3_walk
0.128, edge_gather 0.001, uf2 0.064, relabel(p3) 0.108,
component_histogram 0.164.

At this scale (112 PEs, 1 physical node — every steal is same-node/tier-1)
phaseB (0.265 s) is a small fraction of phase1_stages, unlike the 2B sweep
where it's the dominant, non-scaling term — consistent with
phaseb-offload.md's framing that the floor being attacked only bites at
2B-scale cross-process/cross-node skew, not at this size.

## Raw command (per node count N, procs/node = 8 fixed)

```
srun --mpi=cray_shasta --network=<job_vni | single_node_vni for N=1> --unbuffered \
  --cpu-bind=none --distribution=block:block \
  ./FoF3 -f <input> -d oct -u dist -b 0.2 +ppn 14 \
  +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
  +lci_ndevices 7 +backend_poll_thread 2 [+traceroot <dir> +logsize 100000000 | +traceoff]
```

Job IDs (2B sweep, this rerun): 8n=5184363, 16n=5184372 (only run with
projections tracing), 32n=5184422, 64n=5184423, 128n=5184426.
lambb.00500 1-node run (projections): 5184433.
