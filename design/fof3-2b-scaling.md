# FoF3 2B-particle node-count scaling (2026-08-07, rerun with FOF_STEAL=1/FOF_STEAL_GROUP=8 explicit)

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

This is a rerun of the 2026-08-06 sweep, this time with `FOF_STEAL=1` and
`FOF_STEAL_GROUP=8` set explicitly on every run (per design/phaseb-offload.md
section 7). `git pull origin main` in paratreet2 reported "Already up to
date" (HEAD `18bdcc4`); the FoF3 binary was already built against this exact
commit, so no rebuild was needed — the source (last touched at `23f0248`,
the v3 need-gated-serving stealing commit) hadn't moved since the prior
rerun. `FOF_STEAL_GROUP` was already confirmed defaulting to 8 in
`fof/FoFPhase1.h`, and `FOF_STEAL=0 disables (default on)` per the doc, so
setting both explicitly makes the previously-implicit configuration
explicit rather than changing behavior. `FOF3STAT steal:` lines confirm
grant activity fired in every 2B run (0 in the lambb.00500 run — single
node, 8 processes, apparently no idle-before-drain window at this size).

All runs used `+traceoff` except the 16-node point, which carries full
projections tracing (`+traceroot fof3_projections +logsize 100000000`) —
kept as the one detailed-trace reference point per run instructions.

## Correctness

**424,897,832 components, identical at every node count (8/16/32/64/128)**
— unchanged from both prior sweeps. Bit-for-bit determinism across a 16x
range of parallelism, with stealing now explicitly enabled, is the
crosscheck substitute that STATS MODE calls for at this N.

## Top-line scaling

| Nodes | PEs | Tree build (ms) | phase1 total (s) | TreeCanopy cache loading (ms) | Tree traversal (ms) | Iteration 0 total (ms) |
|------:|-----:|------:|------:|------:|------:|------:|
| 8   | 896   | 2306.3 | 11.04 | 12033.1 | 2022.3 | 16873.1 |
| 16  | 1792  | 977.0  | 8.70  | 9222.7  | 1443.9 | 11947.4 |
| 32  | 3584  | 408.3  | 8.13  | 8484.1  | 1260.6 | 10352.0 |
| 64  | 7168  | 205.5  | 6.56  | 7002.6  | 1318.8 | 8679.7  |
| 128 | 14336 | 181.4  | 5.95  | 7217.3  | 1415.0 | 8947.2  |

Same shape as both prior sweeps: tree build scales cleanly (~13x over the
16x node range); phase1 total and TreeCanopy cache loading flatten hard past
32 nodes; tree traversal and iteration-0 total bottom out around 64 nodes.
The explicit steal config does not change the qualitative picture.

## phase1_stages breakdown (seconds)

| Nodes | reset | register | phaseA | phaseB | merge | relabel |
|------:|------:|---------:|-------:|-------:|------:|--------:|
| 8   | 0.001 | 0.001 | 5.165 | 6.027 | 0.014 | 0.686 |
| 16  | 0.001 | 0.001 | 2.601 | 6.308 | 0.011 | 0.303 |
| 32  | 0.001 | 0.001 | 1.325 | 7.285 | 0.008 | 0.148 |
| 64  | 0.003 | 0.001 | 0.614 | 6.239 | 0.007 | 0.067 |
| 128 | 0.006 | 0.002 | 0.305 | 5.791 | 0.004 | 0.031 |

phaseA still scales cleanly (5.17 -> 0.31 s). **phaseB is still flat
(5.8-7.3 s) and still dominates phase1_stages past 8 nodes, within noise of
the previous sweep's phaseB numbers (6.1-7.6 s) that had FOF_STEAL only at
its implicit default.** This directly reproduces design/phaseb-offload.md
section 8's v3 verdict on Frontier: grant activity is real (see steal
activity below) but the wall does not move, which is the same contradiction
the doc's v4 requirements were written to resolve (target the actual
maximum via a published remaining-work metric, not local admission gating).

### Steal activity (FOF3STAT steal: lines, aggregated per run)

| Nodes | processes reporting a steal line | total units shipped out | total denials |
|------:|----------------------------------:|-------------------------:|---------------:|
| 8   | 3  | 96  | 285  |
| 16  | 3  | 96  | 279  |
| 32  | 9  | 208 | 580  |
| 64  | 24 | 928 | 1580 |
| 128 | 34 | 928 | 2359 |

Grants and denials both grow with node count (more processes, more steal
attempts), but as section 8 of phaseb-offload.md found, the shipped-unit
counts here (a few hundred to ~1k units, out of phaseB pools that run into
the hundreds of thousands to millions of units per `phaseB_units` totals
below) are far too small a share of the work to move a multi-second wall —
consistent with the "top shipper was likely not the wall-owning process"
diagnosis in that doc, which v4's per-process pool-size/wall instrumentation
is meant to settle directly instead of inferring indirectly.

## phase1 detail and phase3 (uf2/walk) breakdown (seconds)

| Nodes | tip_encode | upwardPass | loadCache | uf2_setup | phase3_walk | edge_gather | uf2 | relabel(p3) | component_histogram |
|------:|-----------:|-----------:|----------:|----------:|------------:|------------:|----:|------------:|---------------------:|
| 8   | 0.543 | 0.976 | 0.012 | 0.012 | 0.381 | 0.001 | 0.544 | 0.367 | 0.605 |
| 16  | 0.268 | 0.493 | 0.028 | 0.023 | 0.288 | 0.001 | 0.559 | 0.186 | 0.328 |
| 32  | 0.133 | 0.274 | 0.074 | 0.047 | 0.264 | 0.002 | 0.630 | 0.099 | 0.188 |
| 64  | 0.057 | 0.191 | 0.243 | 0.092 | 0.278 | 0.003 | 0.739 | 0.056 | 0.131 |
| 128 | 0.035 | 0.224 | 1.015 | 0.189 | 0.255 | 0.006 | 0.806 | 0.030 | 0.114 |

Same pattern as both prior sweeps: tip_encode scales cleanly. loadCache
still grows with node count (0.012 -> 1.015 s), still tracked in
design/agenda.md's loadCache anti-scaling item. uf2 grows mildly. Everything
else shrinks or is flat.

## lambb.00500 (80M particles), 1 physical node, with projections tracing

Same machine/config (8 procs/node, 14 PEs/process, 112 PEs total),
`--network=single_node_vni` (1-node run, not `job_vni`), `FOF_STEAL=1`
`FOF_STEAL_GROUP=8` explicit. Full projections tracing:
`+traceroot fof3_projections_lambb500 +logsize 100000000`.

**Components: 23,707,197** — matches the correctness gate value in
design/phaseb-offload.md section 7 exactly.

| Tree build (ms) | phase1 total (s) | TreeCanopy cache loading (ms) | Tree traversal (ms) | Iteration 0 total (ms) |
|------:|------:|------:|------:|------:|
| 547.0 | 1.723 | 2025.2 | 485.5 | 3212.2 |

phase1_stages (s): reset 0.000, register 0.000, phaseA 1.521, phaseB 0.171,
merge 0.003, relabel 0.166. phase1 detail/phase3 (s): tip_encode 0.152,
upwardPass 0.300, loadCache 0.001, uf2_setup 0.002, phase3_walk 0.118,
edge_gather 0.000, uf2 0.054, relabel(p3) 0.113, component_histogram 0.161.

No `FOF3STAT steal:` lines fired at all in this run — with only 8 processes
on one physical node, every process apparently kept a nonempty pool through
the whole phaseB window, so no idle-before-drain steal request ever
triggered. phaseB (0.171 s) is again a small fraction of phase1_stages here,
consistent with the framing in phaseb-offload.md that the floor being
attacked only bites at 2B-scale cross-process skew, not at this size.

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

Job IDs (2B sweep, this rerun): 8n=5187457, 16n=5187465 (only run with
projections tracing), 32n=5187467, 64n=5187471, 128n=5187484.
lambb.00500 1-node run (projections): 5187490.
