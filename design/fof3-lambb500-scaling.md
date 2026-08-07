# FoF3 lambb.00500 (80M-particle) node-count scaling, 1-16 physical nodes (2026-08-07, rerun against v5 rebuild)

lambb.00500 dataset (N = 80,621,568 particles), OctDecomp, `-u dist`,
b_factor 0.2, 8 processes/node, 14 PEs/process (112 PEs at 1 node, scaling
linearly with node count up to 1792 PEs at 16 nodes). Frontier, LCI/CXI
`reconverse` backend, `--network=single_node_vni` at 1 node /
`--network=job_vni` at 2+ nodes, `cray_shasta`, `+lci_ndevices 7`,
`+backend_poll_thread 2`, `FOF_STEAL=1 FOF_STEAL_GROUP=8` explicit. No
projections tracing on any run in this sweep (`+traceoff` throughout).

Rerun after a further paratreet2 rebuild (HEAD `43ef016`, v5 ship-once/
full-flow-accounting phaseB stealing — see design/fof3-2b-scaling.md's
header for the full commit list and design/phaseb-offload.md sections
11-12 for the underlying finding). Same binary as the concurrent 2B v5
rerun.

Loading/decomposition time is excluded below (I/O-bound, not representative
of the algorithm) — all phases from tree build onward, same convention as
design/fof3-2b-scaling.md.

At 80M particles this dataset is still above the STATS MODE threshold
(> 20M for the O(n^2) crosscheck), so these runs also report component
counts without full brute-force verification; determinism (below) is the
correctness check.

## Correctness

**23,707,197 components, identical at every node count (1/2/4/8/16)** —
matches the correctness gate value in design/phaseb-offload.md section 7
(`80M lambb.00500: 23,707,197`) exactly, at every point in the sweep, and
unchanged from the previous (v4) rerun.

## Top-line scaling

| Nodes | PEs | Tree build (ms) | phase1 total (s) | TreeCanopy cache loading (ms) | Tree traversal (ms) | Iteration 0 total (ms) |
|------:|-----:|------:|------:|------:|------:|------:|
| 1  | 112  | 547.9 | 1.765 | 2062.9 | 490.1 | 3256.3 |
| 2  | 224  | 218.3 | 1.025 | 1183.7 | 286.6 | 1784.2 |
| 4  | 448  | 91.6  | 0.644 | 727.1  | 195.8 | 1081.4 |
| 8  | 896  | 47.9  | 0.311 | 376.9  | 157.1 | 635.2  |
| 16 | 1792 | 37.3  | 0.235 | 310.5  | 138.9 | 531.9  |

Same clean, monotonic scaling as the previous (v4) rerun across the full
1-16 node range, within run-to-run noise throughout (e.g. 4-node phase1
total 0.554s -> 0.644s, still well below phaseA's own scaling floor).
Nothing has flattened yet at this problem size, unlike the 2B sweep.

## phase1_stages breakdown (seconds)

| Nodes | reset | register | phaseA | phaseB | merge | relabel |
|------:|------:|---------:|-------:|-------:|------:|--------:|
| 1  | 0.000 | 0.000 | 1.556 | 0.176 | 0.003 | 0.165 |
| 2  | 0.000 | 0.000 | 0.787 | 0.215 | 0.002 | 0.086 |
| 4  | 0.000 | 0.000 | 0.394 | 0.109 | 0.002 | 0.037 |
| 8  | 0.001 | 0.000 | 0.187 | 0.125 | 0.001 | 0.018 |
| 16 | 0.001 | 0.001 | 0.090 | 0.145 | 0.001 | 0.010 |

phaseA scales cleanly (1.556 -> 0.090 s, ~17x). **phaseB again does not
scale down with it** — 0.176 -> 0.215 -> 0.109 -> 0.125 -> 0.145 s,
flat/noisy across the whole range and, by 16 nodes, again the single
largest phase1_stages term (0.145s vs phaseA's 0.090s) — the same
crossover as the previous (v4) rerun and the 2B sweep's own signature.

## phase1 detail and phase3 (uf2/walk) breakdown (seconds)

| Nodes | tip_encode | upwardPass | loadCache | uf2_setup | phase3_walk | edge_gather | uf2 | relabel(p3) | component_histogram |
|------:|-----------:|-----------:|----------:|----------:|------------:|------------:|----:|------------:|---------------------:|
| 1  | 0.154 | 0.296 | 0.001 | 0.002 | 0.122 | 0.001 | 0.055 | 0.113 | 0.161 |
| 2  | 0.072 | 0.156 | 0.002 | 0.003 | 0.094 | 0.001 | 0.045 | 0.053 | 0.071 |
| 4  | 0.035 | 0.076 | 0.006 | 0.006 | 0.081 | 0.001 | 0.039 | 0.030 | 0.029 |
| 8  | 0.019 | 0.050 | 0.014 | 0.012 | 0.073 | 0.001 | 0.030 | 0.017 | 0.018 |
| 16 | 0.009 | 0.041 | 0.031 | 0.024 | 0.063 | 0.001 | 0.027 | 0.009 | 0.011 |

Same pattern as the previous rerun: tip_encode/upwardPass scale cleanly.
loadCache still grows with node count (0.001 -> 0.031 s), same anti-scaling
direction as the 2B sweep and design/agenda.md's item.

## Steal denial breakdown (v5 full-flow accounting, `FOF3STAT stealacct:` lines)

Unlike the previous (v4) rerun — where no `stealacct` line appeared at
all in this sweep — every process now prints one (commit `76e9f90`,
full-flow accounting is unconditional in v5). Aggregated denial reasons:

| Nodes | `ready` denials (donor pool not built yet) | `needy` denials | `drained` denials (donor already claimed everything) | grants |
|------:|------:|------:|------:|------:|
| 1  | 2,281 | 0 | 780    | 0 |
| 2  | 645   | 0 | 1,568  | 0 |
| 4  | 2,442 | 0 | 3,136  | 2 |
| 8  | 2,038 | 0 | 6,272  | 5 |
| 16 | 2,134 | 0 | 12,544 | 0 |

Same shape as the 2B sweep's breakdown (design/fof3-2b-scaling.md): `ready`
and `drained` dominate, `needy` is zero throughout, and grants stay
essentially nonexistent even at 16 nodes. This confirms
design/phaseb-offload.md section 12's round-8 finding holds at 80M scale
too, not just at 2B — the phaseB floor here is the same
already-claimed/not-yet-born phenomenon, just with a much smaller absolute
cost.

## Raw command (per node count N, procs/node = 8 fixed)

```
export FOF_STEAL=1
export FOF_STEAL_GROUP=8
srun --mpi=cray_shasta --network=<single_node_vni for N=1 | job_vni for N>1> --unbuffered \
  --cpu-bind=none --distribution=block:block \
  ./FoF3 -f /lustre/orion/csc710/scratch/rrao/lambb.00500 -d oct -u dist -b 0.2 +ppn 14 \
  +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
  +lci_ndevices 7 +backend_poll_thread 2 +traceoff
```

Job IDs: 1n=5194667, 2n=5194668, 4n=5194670, 8n=5194673, 16n=5194674.
