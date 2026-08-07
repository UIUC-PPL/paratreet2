# FoF3 2B-particle node-count scaling (2026-08-06)

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

## Correctness

**424,897,832 components, identical at every node count (8/16/32/64/128).**
Bit-for-bit determinism across a 16x range of parallelism is the
crosscheck substitute that STATS MODE calls for at this N.

## Top-line scaling

| Nodes | PEs | Tree build (ms) | phase1 total (s) | TreeCanopy cache loading (ms) | Tree traversal (ms) | Iteration 0 total (ms) |
|------:|-----:|------:|------:|------:|------:|------:|
| 8   | 896   | 2304.9 | 12.18 | 13171.0 | 2057.7 | 18059.8 |
| 16  | 1792  | 990.7  | 9.30  | 9845.7  | 1615.0 | 12760.8 |
| 32  | 3584  | 416.1  | 8.88  | 9269.9  | 1338.5 | 11232.0 |
| 64  | 7168  | 214.4  | 7.15  | 7679.0  | 1296.3 | 9351.3 |
| 128 | 14336 | 209.5  | 6.26  | 7581.0  | 1517.2 | 9455.6 |

Tree build scales cleanly (~11x over a 16x node range). phase1 total and
TreeCanopy cache loading both flatten hard past 32 nodes — see phase1
breakdown below for why. Tree traversal and the iteration-0 total bottom
out at 64 nodes and tick back up at 128 (1296.3 -> 1517.2 ms), i.e. this
sweep's scaling ceiling is around 64 nodes for this problem size.

## phase1_stages breakdown (seconds)

| Nodes | reset | register | phaseA | phaseB | merge | relabel |
|------:|------:|---------:|-------:|-------:|------:|--------:|
| 8   | 0.001 | 0.001 | 5.672 | 6.371 | 0.014 | 0.657 |
| 16  | 0.001 | 0.001 | 2.425 | 6.810 | 0.011 | 0.304 |
| 32  | 0.002 | 0.001 | 1.222 | 7.862 | 0.008 | 0.154 |
| 64  | 0.003 | 0.001 | 0.570 | 6.730 | 0.006 | 0.067 |
| 128 | 0.006 | 0.002 | 0.286 | 6.060 | 0.004 | 0.031 |

phaseA halves each time node count doubles (5.67 -> 0.29 s, ~20x over the
sweep) — this is the part of phase1 that scales. **phaseB does not scale
at all** (6.4-7.9 s, flat, no trend with node count) and dominates
phase1_stages everywhere past 8 nodes; it is the reason phase1 total and
TreeCanopy cache loading flatten in the top-line table. Whatever phaseB is
bottlenecked on (a fixed cost, not partitioned by PE count) is the next
thing worth instrumenting if phase1 needs to keep scaling past 32 nodes.

## phase1 detail and phase3 (uf2/walk) breakdown (seconds)

| Nodes | tip_encode | upwardPass | loadCache | uf2_setup | phase3_walk | edge_gather | uf2 | relabel(p3) | component_histogram |
|------:|-----------:|-----------:|----------:|----------:|------------:|------------:|----:|------------:|---------------------:|
| 8   | 0.533 | 0.966 | 0.022 | 0.012 | 0.423 | 0.001 | 0.552 | 0.349 | 0.611 |
| 16  | 0.270 | 0.497 | 0.045 | 0.023 | 0.335 | 0.001 | 0.671 | 0.189 | 0.338 |
| 32  | 0.130 | 0.273 | 0.110 | 0.046 | 0.287 | 0.002 | 0.676 | 0.099 | 0.197 |
| 64  | 0.058 | 0.199 | 0.315 | 0.092 | 0.275 | 0.003 | 0.714 | 0.053 | 0.138 |
| 128 | 0.034 | 0.224 | 1.073 | 0.191 | 0.335 | 0.006 | 0.826 | 0.029 | 0.117 |

tip_encode scales cleanly (~16x over the sweep, tracks node count almost
exactly). **loadCache is the opposite of everything else here: it grows
with node count** (0.022 -> 1.073 s, ~49x), consistent with more,
smaller-granularity cache fetches as PE count increases (see `starter
pack size` in the raw logs: 4629 -> 68430 across the sweep, scaling with
subtree count). uf2 grows mildly (0.55 -> 0.83 s) for the same
redundancy/fan-in reason. Everything else in this table shrinks or is flat.

## Raw command (per node count N, procs/node = 8 fixed)

```
srun --mpi=cray_shasta --network=job_vni --unbuffered \
  --cpu-bind=none --distribution=block:block \
  ./FoF3 -f cosmo25cmb.768g2_dm.001024 -d oct -u dist -b 0.2 +ppn 14 \
  +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
  +lci_ndevices 7 +backend_poll_thread 2 [+traceroot fof3_projections +logsize 100000000 | +traceoff]
```

Job IDs: 8n=5183659, 16n=5183698 (only run with projections tracing),
32n=5183706, 64n=5183716, 128n=5183718.
