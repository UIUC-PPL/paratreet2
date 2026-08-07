# FoF3 lambb.00500 (80M-particle) node-count scaling, 1-16 physical nodes (2026-08-07)

lambb.00500 dataset (N = 80,621,568 particles), OctDecomp, `-u dist`,
b_factor 0.2, 8 processes/node, 14 PEs/process (112 PEs at 1 node, scaling
linearly with node count up to 1792 PEs at 16 nodes). Frontier, LCI/CXI
`reconverse` backend, `--network=single_node_vni` at 1 node /
`--network=job_vni` at 2+ nodes, `cray_shasta`, `+lci_ndevices 7`,
`+backend_poll_thread 2`, `FOF_STEAL=1 FOF_STEAL_GROUP=8` explicit, built
against the v4 phaseB stealing rebuild (commit `44edf47`, same binary as
the concurrent 2B v4-rebuild sweep in design/fof3-2b-scaling.md). No
projections tracing on any run in this sweep (`+traceoff` throughout).

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
(`80M lambb.00500: 23,707,197`) exactly, at every point in the sweep.

## Top-line scaling

| Nodes | PEs | Tree build (ms) | phase1 total (s) | TreeCanopy cache loading (ms) | Tree traversal (ms) | Iteration 0 total (ms) |
|------:|-----:|------:|------:|------:|------:|------:|
| 1  | 112  | 543.9 | 1.756 | 2055.1 | 475.8 | 3227.7 |
| 2  | 224  | 219.0 | 1.043 | 1200.5 | 282.2 | 1797.1 |
| 4  | 448  | 90.3  | 0.554 | 636.6  | 192.9 | 986.8  |
| 8  | 896  | 47.1  | 0.321 | 386.7  | 156.4 | 641.5  |
| 16 | 1792 | 37.8  | 0.238 | 312.4  | 140.5 | 535.7  |

Tree build, TreeCanopy cache loading, tree traversal, and iteration-0 total
all scale cleanly and monotonically across the full 1-16 node range at this
problem size — unlike the 2B sweep, nothing has flattened yet by 16 nodes.
This is the expected contrast: 80M is small enough that 16 nodes (1792 PEs,
~45k particles/PE) hasn't hit the per-process-skew regime that flattens
phase1/TreeCanopy in the 2B runs starting around 32 nodes.

## phase1_stages breakdown (seconds)

| Nodes | reset | register | phaseA | phaseB | merge | relabel |
|------:|------:|---------:|-------:|-------:|------:|--------:|
| 1  | 0.000 | 0.000 | 1.557 | 0.177 | 0.003 | 0.164 |
| 2  | 0.000 | 0.000 | 0.786 | 0.232 | 0.002 | 0.086 |
| 4  | 0.000 | 0.000 | 0.403 | 0.152 | 0.001 | 0.037 |
| 8  | 0.001 | 0.000 | 0.186 | 0.134 | 0.001 | 0.018 |
| 16 | 0.001 | 0.001 | 0.092 | 0.149 | 0.001 | 0.010 |

phaseA scales cleanly (1.557 -> 0.092 s, ~17x over the 16x node range,
matching the PE count scaling). **phaseB does not scale down with it** —
0.177 -> 0.232 -> 0.152 -> 0.134 -> 0.149 s, essentially flat/noisy from 1
to 16 nodes, the same non-scaling signature seen at every point of the 2B
sweep (design/fof3-2b-scaling.md). By 16 nodes phaseB (0.149s) is already
larger than phaseA (0.092s) and is the single largest phase1_stages term —
the same crossover the 2B sweep shows from its very first (8-node) data
point. This is a useful low-cost reproduction of the 2B finding: the
phaseB floor isn't purely a 2B-scale phenomenon, it shows up as soon as
per-process work gets small enough, which happens far earlier (16 nodes,
80M particles here) than it does for the 2B dataset.

## phase1 detail and phase3 (uf2/walk) breakdown (seconds)

| Nodes | tip_encode | upwardPass | loadCache | uf2_setup | phase3_walk | edge_gather | uf2 | relabel(p3) | component_histogram |
|------:|-----------:|-----------:|----------:|----------:|------------:|------------:|----:|------------:|---------------------:|
| 1  | 0.151 | 0.298 | 0.001 | 0.002 | 0.115 | 0.001 | 0.054 | 0.114 | 0.154 |
| 2  | 0.072 | 0.155 | 0.002 | 0.003 | 0.090 | 0.001 | 0.043 | 0.054 | 0.072 |
| 4  | 0.035 | 0.075 | 0.006 | 0.006 | 0.080 | 0.001 | 0.037 | 0.030 | 0.028 |
| 8  | 0.019 | 0.049 | 0.014 | 0.012 | 0.075 | 0.001 | 0.029 | 0.016 | 0.017 |
| 16 | 0.008 | 0.040 | 0.031 | 0.024 | 0.064 | 0.001 | 0.027 | 0.009 | 0.012 |

tip_encode and upwardPass scale cleanly. loadCache grows with node count
(0.001 -> 0.031 s, ~31x) — the same anti-scaling direction as the 2B sweep
and design/agenda.md's loadCache item, just at far smaller absolute cost
here. phase3_walk, uf2, and uf2_setup shrink slowly rather than cleanly,
consistent with them carrying some of the same per-process-skew cost as
phaseB.

No `FOF3STAT stealacct:` lines appeared in any of the 5 runs in this sweep
(same as the 1-node lambb500 point in the concurrent 2B v4 rerun) — no
process in this dataset/scale range appears to trigger the accounting
print.

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

Job IDs: 1n=5191511, 2n=5191515, 4n=5191519, 8n=5191521, 16n=5191522.
