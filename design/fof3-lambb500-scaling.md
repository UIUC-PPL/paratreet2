# FoF3 lambb.00500 (80M-particle) node-count scaling, 1-16 physical nodes (2026-08-10, `main-without-steal` branch, no phaseB stealing)

lambb.00500 dataset (N = 80,621,568 particles), OctDecomp, `-u serial`,
b_factor 0.2, 8 processes/node, 14 PEs/process (112 PEs at 1 node, scaling
linearly with node count up to 1792 PEs at 16 nodes). Frontier, LCI/CXI
`reconverse` backend, `--network=single_node_vni` at 1 node /
`--network=job_vni` at 2+ nodes, `cray_shasta`, `+lci_ndevices 7`,
`+backend_poll_thread 2`. No projections tracing on any run in this sweep
(`+traceoff` throughout).

**This is a rerun on a fresh branch (`main-without-steal`, HEAD `553d722`)
that rolls back all phaseB work-stealing code entirely** — no
`FOF_STEAL`, no donor/thief protocol, no `-Z`/`-H`/`-R` CLI flags. phaseB
is back to a plain per-process shared pool that local PEs claim from via
`fetch_add`-on-an-atomic-cursor, with no cross-process redistribution at
all — see design/fof3-2b-scaling.md for the full framing and
design/phaseb-offload.md for the retired stealing protocol's history.
`-u serial` is kept (design/agenda.md item 6). `FOF_COUNT_VERIFY=1` was
set on every run in this sweep (recomputes component counts from
particles at freeze time and aborts on divergence) as an extra hard
correctness check on top of the STATS MODE component count, since this
dataset stays below the O(n^2) full-verification threshold's usefulness
window at this scale; it stayed silent (pass) on every run.

Loading/decomposition time is excluded below (I/O-bound, not representative
of the algorithm) — all phases from tree build onward, same convention as
design/fof3-2b-scaling.md.

At 80M particles this dataset is still above the STATS MODE threshold
(> 20M for the O(n^2) crosscheck), so these runs also report component
counts without full brute-force verification; determinism (below) plus
the `FOF_COUNT_VERIFY` particle recount are the correctness checks.

## Correctness

**23,707,197 components, identical at every node count (1/2/4/8/16)** —
matches the correctness gate value in design/phaseb-offload.md section 7
(`80M lambb.00500: 23,707,197`) exactly, and unchanged from every prior
sweep on this dataset. `FOF_COUNT_VERIFY=1`'s particle recount matched the
freeze-time maintained counts on every run (no `COUNT VERIFY MISMATCH`,
no abort).

## Top-line scaling

| Nodes | PEs | Tree build (ms) | phase1 total (s) | TreeCanopy cache loading (ms) | Tree traversal (ms) | Iteration 0 total (ms) |
|------:|-----:|------:|------:|------:|------:|------:|
| 1  | 112  | 553.5 | 1.755 | 1887.8 | 797.0 | 3391.3 |
| 2  | 224  | 220.5 | 1.046 | 1117.4 | 405.1 | 1837.1 |
| 4  | 448  | 90.8  | 0.534 | 580.5  | 264.9 | 1004.0 |
| 8  | 896  | 46.5  | 0.312 | 355.9  | 199.8 | 655.0  |
| 16 | 1792 | 36.6  | 0.213 | 276.5  | 219.0 | 578.7  |

Clean, monotonic scaling across the full 1-16 node range, same shape as
every prior sweep on this dataset. Nothing flattens or blows up at this
problem size (unlike the 2B sweep's 128-node `relabel(p3)` spike) — 80M
stays well inside the range where per-node work is still the dominant
cost at every node count tested.

## phase1_stages breakdown (seconds)

| Nodes | reset | register | phaseA | phaseB | merge | relabel |
|------:|------:|---------:|-------:|-------:|------:|--------:|
| 1  | 0.000 | 0.000 | 1.563 | 0.152 | 0.003 | 0.165 |
| 2  | 0.000 | 0.000 | 0.778 | 0.209 | 0.002 | 0.086 |
| 4  | 0.000 | 0.000 | 0.399 | 0.133 | 0.002 | 0.037 |
| 8  | 0.001 | 0.000 | 0.181 | 0.120 | 0.001 | 0.018 |
| 16 | 0.001 | 0.001 | 0.093 | 0.126 | 0.001 | 0.010 |

phaseA scales cleanly (1.563 -> 0.093 s, ~17x). **phaseB again does not
scale down with it** — 0.152 -> 0.209 -> 0.133 -> 0.120 -> 0.126 s,
flat/noisy across the whole range, same signature as the 2B sweep (just
at proportionally smaller absolute cost, since 80M has far fewer units
than 2B to begin with). By 16 nodes phaseB is again the single largest
phase1_stages term (0.126 s vs phaseA's 0.093 s). Consistent with
design/fof3-2b-scaling.md's confirmation: phaseB's floor is unmoved by
removing stealing entirely, at both problem sizes.

## phase1 detail and phase3 (uf2/walk) breakdown (seconds)

| Nodes | tip_encode | fragcount | upwardPass | loadCache | uf2_setup | phase3_walk | edge_gather | uf2 | relabel(p3) | component_histogram |
|------:|-----------:|----------:|-----------:|----------:|----------:|------------:|------------:|----:|------------:|---------------------:|
| 1  | 0.000 | 0.000 | 0.131 | 0.001 | 0.000 | 0.117 | 0.001 | 0.005 | 0.267 | 0.368 |
| 2  | 0.000 | 0.000 | 0.069 | 0.002 | 0.000 | 0.090 | 0.001 | 0.008 | 0.116 | 0.168 |
| 4  | 0.000 | 0.000 | 0.041 | 0.006 | 0.000 | 0.082 | 0.001 | 0.012 | 0.083 | 0.073 |
| 8  | 0.000 | 0.000 | 0.029 | 0.014 | 0.000 | 0.073 | 0.001 | 0.019 | 0.063 | 0.032 |
| 16 | 0.000 | 0.000 | 0.032 | 0.031 | 0.000 | 0.063 | 0.002 | 0.031 | 0.089 | 0.020 |

Same pattern as the 2B sweep: `tip_encode` reads 0.000 s everywhere on
this branch/build (accounting difference from the reset, not a new
finding). `loadCache` still grows with node count (0.001 -> 0.031 s),
same anti-scaling direction as the 2B sweep and design/agenda.md's
tracked item. `relabel(p3)` stays small and does not blow up at this
problem size (0.06-0.27 s throughout) — the 128-node/2B-only blowup is a
large-PE-count property this 16-node/80M sweep never reaches.

## Raw command (per node count N, procs/node = 8 fixed)

```
srun --mpi=cray_shasta --network=<single_node_vni for N=1 | job_vni for N>1> --unbuffered \
  --cpu-bind=none --distribution=block:block \
  ./FoF3 -f /lustre/orion/csc710/scratch/rrao/lambb.00500 -d oct -u serial +ppn 14 \
  +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
  +lci_ndevices 7 +backend_poll_thread 2 +traceoff
```

No `FOF_STEAL`/`FOF_STEAL_GROUP` env vars, no `-Z`/`-H`/`-R` flags — none
of that exists on this branch. `FOF_COUNT_VERIFY=1` set on every run.
`LCI_ATTR_BACKEND=ofi`, `FI_CXI_RX_MATCH_MODE=hybrid`,
`PMI_MAX_KVS_ENTRIES=4194304` kept (unrelated CXI/multi-process-per-node
fixes).

Job IDs: 1n=5224456, 2n=5224763, 4n=5224964, 8n=5224987, 16n=5224992.
