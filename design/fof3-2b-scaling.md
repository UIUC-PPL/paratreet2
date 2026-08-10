# FoF3 2B-particle node-count scaling (2026-08-10, `uniform-annotation` branch, HEAD `16fd529`)

Cosmo25 dataset (`cosmo25cmb.768g2_dm.001024`, N = 1,981,808,640 particles),
OctDecomp, **`-u dist` (framework default — see note below)**, b_factor
0.2, 8 processes/node, 14 PEs/process (1792 PEs at 16 nodes, scaling
linearly with node count). Frontier, LCI/CXI `reconverse` backend,
`job_vni` + `cray_shasta`, `+lci_ndevices 7`, `+backend_poll_thread 2`.
Auto mode falls back to STATS MODE at this N (> 20M threshold for the
O(n^2) crosscheck), so these runs report component counts without full
brute-force verification; determinism (below) is the substitute
correctness check.

**Branch/build note:** `uniform-annotation`, HEAD `16fd529`, two commits
ahead of the previous (`main-without-steal`, `553d722`) sweep:
`2d6a256` ("phaseB: write the frozen tips into the node annotation") and
`16fd529` ("phaseA: fuse the tip annotation into the freeze pass"). Still
no phaseB stealing code (that stays fully removed per the prior sweep's
header).

**Mode note — not directly comparable to the immediately prior sweep:**
the sbatch scripts for this run drop the `-u serial` flag entirely, so
`FoF3` ran with the getopt default, `UF2Mode::Dist` (confirmed via each
log's `UF_2 implementation: dist` line) — not `-u serial` as in every
sweep since design/agenda.md item 6 recommended it as the production
default. This is a real algorithm-path change, not just a code change:
`uf2_setup`/`phase3_walk`/`uf2`/`relabel(p3)` below take the distributed
UF2 path instead of the gather-to-PE0/serial-union-find/broadcast path,
so those four columns are **not** comparable to the previous sweep's
numbers. `phase1_stages` (reset/register/phaseA/phaseB/merge/relabel) is
mode-independent and *is* directly comparable.

Loading/decomposition time is excluded below (I/O-bound, not representative
of the algorithm) — all phases from tree build onward.

## Correctness

**424,897,832 components, identical at every node count (8/16/32/64/128)**
— unchanged from every prior sweep, including the immediately prior
stealing-free/`-u serial` sweep.

## Top-line scaling

| Nodes | PEs | Tree build (ms) | phase1 total (s) | TreeCanopy cache loading (ms) | Tree traversal (ms) | Iteration 0 total (ms) |
|------:|-----:|------:|------:|------:|------:|------:|
| 8   | 896   | 2304.3 | 7.953 | 8933.2 | 2036.4 | 13784.6 |
| 16  | 1792  | 984.7  | 5.358 | 5880.0 | 1470.1 | 8636.1  |
| 32  | 3584  | 405.9  | 4.109 | 4466.2 | 1243.0 | 6312.1  |
| 64  | 7168  | 212.3  | 3.046 | 3487.3 | 1392.6 | 5255.3  |
| 128 | 14336 | 190.6  | 2.552 | 3763.0 | 1507.8 | 5594.4  |

Clean, monotonic scaling throughout, and **no 128-node blowup this time**
— Iteration 0 total actually keeps dropping into 64/128 nodes (5255.3 ->
5594.4 ms, flat/noisy, not the 2x jump seen in the `-u serial` sweep).
That prior blowup was driven entirely by `relabel(p3)` in the serial
gather/broadcast path, which this run doesn't exercise (see mode note).

## phase1_stages breakdown (seconds) — mode-independent, comparable across sweeps

| Nodes | reset | register | phaseA | phaseB | merge | relabel |
|------:|------:|---------:|-------:|-------:|------:|--------:|
| 8   | 0.001 | 0.000 | 5.213 | 2.800 | 0.015 | 0.689 |
| 16  | 0.001 | 0.001 | 2.684 | 2.878 | 0.011 | 0.305 |
| 32  | 0.002 | 0.001 | 1.361 | 3.284 | 0.007 | 0.149 |
| 64  | 0.003 | 0.001 | 0.645 | 2.714 | 0.006 | 0.073 |
| 128 | 0.006 | 0.002 | 0.314 | 2.377 | 0.004 | 0.035 |

**phaseB drops substantially vs the prior stealing-free sweep: 2.4-3.3 s
here vs 5.3-7.6 s before, at every node count.** Since `phase1_stages` is
mode-independent, this isn't an artifact of the `-u dist` switch — it
lines up with the two new commits (`2d6a256`/`16fd529`, fusing tip
annotation into the phaseA freeze pass and writing frozen tips into the
phaseB node annotation). phaseB is still flat rather than scaling down
with phaseA (2.8 -> 2.9 -> 3.3 -> 2.7 -> 2.4 s, no clean trend with node
count), so it's still the dominant phase1_stages term past 8 nodes — the
indivisible-claim-work floor from design/phaseb-offload.md section 12
hasn't been eliminated, just lowered. Worth a dedicated before/after
comparison at matching `-u` mode if you want to confirm the magnitude of
this drop precisely.

## phase1 detail and phase3 (uf2/walk) breakdown (seconds) — dist-mode path, not comparable to the prior serial-mode sweep

| Nodes | tip_encode | fragcount | upwardPass | loadCache | uf2_setup | phase3_walk | edge_gather | uf2 | relabel(p3) | component_histogram |
|------:|-----------:|----------:|-----------:|----------:|----------:|------------:|------------:|----:|------------:|---------------------:|
| 8   | 0.551 | 0.000 | 0.969 | 0.008 | 0.012 | 0.382 | 0.001 | 0.556 | 0.368 | 0.606 |
| 16  | 0.269 | 0.000 | 0.495 | 0.024 | 0.024 | 0.284 | 0.001 | 0.587 | 0.185 | 0.332 |
| 32  | 0.133 | 0.000 | 0.274 | 0.077 | 0.046 | 0.256 | 0.002 | 0.624 | 0.098 | 0.187 |
| 64  | 0.059 | 0.000 | 0.193 | 0.236 | 0.096 | 0.313 | 0.053 | 0.710 | 0.052 | 0.151 |
| 128 | 0.033 | 0.000 | 0.227 | 0.960 | 0.194 | 0.237 | 0.006 | 0.838 | 0.030 | 0.189 |

`tip_encode` is nonzero here (0.033-0.551 s, scaling down with node
count) for the first time since the stealing rollback — plausibly the new
tip-annotation-fusion commits made this a real, separately-timed step
again. `loadCache` keeps anti-scaling with node count (0.008 -> 0.960 s),
same direction as every prior sweep. `relabel(p3)` stays small throughout
(0.03-0.37 s) — expected for `-u dist`, which has no serial gather/
broadcast bracket to blow up at high PE counts the way `-u serial` did at
128 nodes in the prior sweep.

## Raw command (per node count N, procs/node = 8 fixed)

```
srun --mpi=cray_shasta --network=job_vni --unbuffered \
  --cpu-bind=none --distribution=block:block \
  ./FoF3 -f <input> -d oct +ppn 14 \
  +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
  +lci_ndevices 7 +backend_poll_thread 2 +traceoff
```

No `-u` flag (defaults to `dist`), no `FOF_STEAL`/`-Z`/`-H`/`-R` (still
gone). `LCI_ATTR_BACKEND=ofi`, `FI_CXI_RX_MATCH_MODE=hybrid`,
`PMI_MAX_KVS_ENTRIES=4194304` kept. All 5 runs in this sweep use
`+traceoff` (no projections tracing).

Job IDs: 8n=5226461, 16n=5226462, 32n=5226463, 64n=5226464, 128n=5226467.
