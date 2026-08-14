# FoF3 2B-particle node-count scaling: CPU chain vs device phase 1 (2026-08-14)

Cosmo25 dataset (`cosmo25cmb.768g2_dm.001024`, N = 1,981,808,640
particles), OctDecomp, `-u dist`, `-c stats`, b_factor 0.2, 8
processes/node, 14 PEs/process, **one GPU per process** (8 MI250X GCDs
per node, the enforced invariant of design/phase1-gpu.md section 2).
Frontier, LCI/CXI `reconverse` backend, `job_vni` + `cray_shasta`,
`+lci_ndevices 7`, `+backend_poll_thread 2`.

This is the first node-count sweep of the DEVICE phase-1 path
(design/phase1-gpu.md section 18: `FOF_GPU_PHASE1=1`, which skips
phaseA + phaseB + merge + relabel entirely and produces the labels on the
GPU). Three arms at every node count, same build, same day, same input:

| arm | phase 1 | tree |
|---|---|---|
| `cpu_l12` | the CPU chain; `PARATREET_DEVICE_TREE=0`, so no flat-tree emit either | leaf 12 |
| `replace_l12` | the device path on the SAME tree — apples to apples | leaf 12 |
| `replace_l128` | the device path on the tree it prefers (section 16) | leaf 128 |

Script: `examples/fof3/scripts/run_fof3_scale_gpu.sbatch`, driven by
`submit_scale_gpu.sh`. One parameterised script rather than five copies,
because the body has to stay identical across the sweep for the sweep to
mean anything.

**Every arm is `-i 2` and the tables below report ITERATION 1.** Iteration
0 of a device arm additionally pays one-time Kokkos/HIP initialization
and the first pinned allocation inside phase 1 — 1.986 s vs 1.059 s at 8
nodes, 1.045 s vs 0.260 s at 128 — so a single-iteration sweep reports
the device path's startup as its per-iteration cost. Every device
measurement in design/phase1-gpu.md sections 11-17 was single-iteration
and carries it. Iteration-0 numbers are in section 6 for comparison with
the prior CPU sweep (Appendix A), which reported iteration 0.

Loading/decomposition time is excluded throughout (I/O-bound) — all
phases from tree build onward.

Job IDs: 8n=5264070, 16n=5264095, 32n=5264116, 64n=5264135, 128n=5264075.

## 1. Correctness

**424,897,832 components, max component 185,317,566, identical in every
arm at every node count** — 27 log2 histogram bins agreeing digit for
digit across 26 arm-iterations, and unchanged from every prior sweep
including the 2026-08-10 CPU-only one.

That is a stronger statement than a repeated number. The `replace_l128`
arms use a different tree from the CPU baseline: different leaf sets,
different traversal order, different certificate and suppression
sequences, and — through the section-16 auto-selection — a different
GPU kernel shape. All of it lands on the same answer.

The 16-node job additionally ran a `verify_l12` arm, which runs BOTH the
CPU chain and the device pass and compares every particle's label:
**0 mismatches over 1,981,808,640 particles and 128 processes**, with
`FOF_COUNT_VERIFY=1` also checking the representative structure against a
recount from the particles on every PE.

`FOF_COUNT_VERIFY` is deliberately OFF in the timed arms: it rehashes
every particle inside `depositLabelCounts`, which lands in the
`component_histogram` column this sweep is measuring.

## 2. Top-line scaling (iteration 1, seconds)

| Nodes | PEs | cpu_l12 phase 1 | replace_l128 phase 1 | speedup | cpu_l12 iteration | replace_l128 iteration | speedup |
|------:|------:|------:|------:|------:|------:|------:|------:|
| 8   | 896   | 8.516 | 1.059 | **8.0x** | 13.360 | 6.843 | **1.95x** |
| 16  | 1792  | 5.342 | 0.669 | **8.0x** | 8.268  | 4.984 | **1.66x** |
| 32  | 3584  | 4.419 | 0.391 | **11.3x** | 6.502 | 4.205 | **1.55x** |
| 64  | 7168  | 3.329 | 0.326 | **10.2x** | 5.332 | 4.008 | **1.33x** |
| 128 | 14336 | 2.714 | 0.260 | **10.4x** | 5.849 | 5.883 | **0.99x** |

**Phase 1 is 8-11x faster at every scale. The application speedup shrinks
monotonically from 1.95x to parity as node count grows** (1.95, 1.66,
1.55, 1.33, 0.99). Both facts matter and the second is the one to act on.

## 3. Why phase 1 wins, and why the CPU chain stops scaling

The CPU chain's own breakdown says it (seconds, `cpu_l12`, iteration 1):

| Nodes | phaseA | phaseB | merge | relabel | phaseB as % of CPU phase 1 |
|------:|-------:|-------:|------:|--------:|------:|
| 8   | 5.514 | 2.884 | 0.015 | 0.142 | 34% |
| 16  | 2.345 | 2.935 | 0.011 | 0.111 | 55% |
| 32  | 1.097 | 3.417 | 0.008 | 0.058 | 75% |
| 64  | 0.564 | 2.901 | 0.006 | 0.029 | 83% |
| 128 | 0.268 | 2.513 | 0.004 | 0.014 | **93%** |

**phaseA scales 20.6x over 16x the nodes. phaseB does not scale at all**
— 2.9 s at 8 nodes, 2.5 s at 128 — because it is cross-PE boundary work
whose cost tracks the number of PE pairs, not the number of particles.
By 128 nodes it is 93% of CPU phase 1, and the CPU chain has stopped
scaling with it.

The device path **does not have a phaseB**. Making the process rather
than the PE the unit of union-find deletes the phase outright
(design/phase1-gpu.md section 5), so the device arm carries no term that
grows with concurrency:

| Nodes | device wall | pack | tree | GPU pass | scatter |
|------:|------:|------:|------:|------:|------:|
| 8   | 0.953 | 0.321 | 0.042 | 0.449 | 0.394 |
| 16  | 0.548 | 0.174 | 0.033 | 0.306 | 0.200 |
| 32  | 0.308 | 0.089 | 0.021 | 0.193 | 0.103 |
| 64  | 0.204 | 0.040 | 0.004 | 0.110 | 0.061 |
| 128 | 0.132 | 0.033 | 0.033 | 0.068 | 0.034 |

Every column scales. The device phase-1 wall drops 7.2x over 16x the
nodes (45% parallel efficiency) against the CPU chain's 3.1x (20%). This
is the structural bet of design/phase1-gpu.md section 1, measured across
the whole sweep rather than at a single point.

## 4. Why the application speedup shrinks: a downstream regression that grows

Apples to apples — `cpu_l12` vs `replace_l12`, the SAME tree, the same
labels, provably the same phase-3 work — every communication-bound phase
after phase 1 is slower on the device arm, and the gap grows with node
count (seconds, iteration 1):

| Nodes | upwardPass | loadCache | phase3 walk | uf2 | downstream total | phase-1 saving | net |
|------:|------:|------:|------:|------:|------:|------:|------:|
| 8   | 0.663 -> 0.826 | 0.022 -> 0.036 | 0.513 -> 0.712 | 0.494 -> 1.048 | **+0.93** | 7.14 | +6.21 |
| 16  | 0.353 -> 0.503 | 0.046 -> 0.096 | 0.378 -> 0.569 | 0.542 -> 0.785 | **+0.63** | 4.52 | +3.88 |
| 32  | 0.215 -> 0.332 | 0.118 -> 0.218 | 0.324 -> 0.595 | 0.527 -> 0.793 | **+0.75** | 3.95 | +3.19 |
| 64  | 0.190 -> 0.313 | 0.309 -> 0.452 | 0.231 -> 0.382 | 0.583 -> 0.849 | **+0.68** | 2.98 | +2.29 |
| 128 | 0.242 -> 0.484 | 1.180 -> 1.482 | 0.189 -> 0.392 | 0.712 -> 1.715 | **+1.75** | 2.41 | **+0.66** |

(All columns `cpu_l12 -> replace_l12`, so the trees, the labels and the
phase-3 work are identical and only phase 1's implementation differs.)

Ratios sit between 1.4x and 2.4x, at every scale, in every column. Set
the penalty against what phase 1 saves — 7.14 s at 8 nodes falling to
2.41 s at 128 — and the whole shape of the top-line table follows: **the
saving shrinks as the CPU chain scales down, the penalty roughly doubles
into 128 nodes, and what is left cancels.** The measured whole-iteration
deltas track the `net` column (4.31, 2.42, 2.09, 1.38, -0.63 s); the
residual is tree build, which the flat-tree emit makes more expensive at
leaf 12 and free at leaf 128.

This is design/phase1-gpu.md section 17.3's unexplained phase-3
regression, and the sweep says three things about it that a single node
count could not:

1. **It is not the shadow.** Section 17.3 measured it while the device
   pass ran BESIDE the CPU chain. The CPU chain is gone here and it is
   still there, at the same ratio.
2. **It is not memory capacity.** Section 18.9 already showed
   `FOF_GPU_RELEASE=1` (freeing the ~600 MB of pinned staging after the
   scatter) moves phase 3 by 12 ms while costing ~100 ms per iteration in
   re-allocation. And it is not `hapiPollEvents`, eliminated by reading
   in section 18.4.
3. **It is communication.** Every regressed column — `upwardPass`,
   `loadCache`, the phase-3 walk, `uf2` — is message-bound, and the two
   phases that are not (`tip_encode`, `component_histogram`) move far
   less. The regression also grows with the node count, which is what a
   network effect does and what a per-process memory effect does not.

The working hypothesis is therefore an interaction between the HIP
context's device-memory registrations and libfabric's CXI memory
registration cache (`FI_MR_CACHE_MONITOR=userfaultfd`), on nodes running
8 processes that each pin hundreds of megabytes. **That is a hypothesis,
not a finding.** The test that would separate "having a GPU bound" from
"having used it" is a run that initializes the device and never launches
anything; the Projections traces below are the other half of the tool.

Note also that phase-3 cost is tree-dependent: `replace_l128`'s coarse
tree makes phase 1 cheaper and the phase-3 walk more expensive (0.497 vs
0.392 s at 128 nodes), so the l12/l128 choice is a phase-1/phase-3
trade, not a free win. At 64 nodes `replace_l12` is marginally the better
whole-iteration configuration (3.952 vs 4.008 s).

`loadCache` anti-scales in BOTH arms (0.022 -> 1.180 s on the CPU arm
alone), exactly as in every prior sweep. That is a pre-existing property
of the starter-pack broadcast, not something the device path introduced,
and by 128 nodes it is the single largest term in the iteration.

## 5. Projections traces (16 nodes)

Two traced arms, in the 16-node job only, matching the CPU sweep's
convention exactly: the binary is linked with `-tracemode projections` at
every node count (`make GPU=1 PROJECTIONS=1`) and every other arm runs
`+traceoff`, so the tracing-linked-but-disabled overhead is identical on
both sides of every comparison in this document.

| arm | traceroot | files | size |
|---|---|---|---|
| `traced_cpu_l12` | `.../scale_gpu_5264095/traced_cpu_l12` | 1794 | 2.7 GB |
| `traced_replace_l128` | `.../scale_gpu_5264095/traced_replace_l128` | 1794 | 2.5 GB |

1792 PE logs plus `.sts` and `.projrc` in each; gzip-verified. Under
`/lustre/orion/csc710/scratch/rrao/fof3_traces/scale_gpu_5264095/`, on
Lustre rather than the example directory — 1792 PEs of logs is not a
thing to point at a home filesystem.

**Both arms are traced, not just the device one.** The open question
these traces exist to answer is section 4's regression, and a single
timeline cannot show a difference: it needs the device-free timeline to
diff against. The two arms differ only in whether phase 1 ran on the GPU,
and they produce identical labels, so any divergence after phase 1 is the
effect and not the work.

`+logsize` is 20,000,000 rather than the CPU sweep's 100,000,000.
`LogPool` `reserve()`s the pool up front, and at 112 PEs per node 100M
entries reserves ~900 GB of address space per node against 512 GB of RAM
— survivable only because `reserve()` commits nothing until written.
Measured usage is ~1.5 MB compressed per PE. `logsize` changes buffering,
never trace CONTENT, so it costs nothing in comparability.

Timings from the traced arms are close to their untraced twins — traced
`cpu_l12` iteration 0 is 8.699 s against 8.562 s untraced, traced
`replace_l128` is 5.374 s against 5.663 s (i.e. the traced arm came out
FASTER, which is the size of the run-to-run noise) — but they should not
be quoted as the sweep's numbers.

## 6. Iteration 0, for comparison with the prior sweep

Appendix A reports iteration 0. These are the matching rows (seconds):

| Nodes | cpu_l12 iter 0 | replace_l128 iter 0 | cpu phase 1 | replace phase 1 |
|------:|------:|------:|------:|------:|
| 8   | 14.074 | 8.199 | 8.588 | 1.986 |
| 16  | 8.562  | 5.663 | 5.378 | 1.426 |
| 32  | 6.724  | 4.739 | 4.478 | 1.065 |
| 64  | 5.375  | 4.534 | 3.341 | 0.934 |
| 128 | 5.787  | 6.331 | 2.715 | 1.045 |

The device arm's iteration-0 phase 1 is 0.7-0.9 s above its steady state
at every node count, and that difference is one-time Kokkos/HIP
initialization plus the first pinned allocation. It does not scale down
with node count — at 128 nodes it is 0.785 s of a 1.045 s phase 1, i.e.
three quarters of the reported cost is startup — which is why the
steady-state tables above are the ones to read.

The `cpu_l12` rows here are within a few percent of Appendix A's
(14.074 vs 13.785 at 8 nodes, 8.562 vs 8.636 at 16, 6.724 vs 6.312 at
32, 5.375 vs 5.255 at 64, 5.787 vs 5.594 at 128), which is the check that
this sweep's baseline is the same baseline.

## 7. Raw command

```
env PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1 \
srun --mpi=cray_shasta --network=job_vni --unbuffered \
  --cpu-bind=none --distribution=block:block \
  --ntasks=$((8*NODES)) --gpus-per-node=8 \
  ./FoF3 -f <input> -d oct -u dist -c stats -l 128 -i 2 +ppn 14 \
  +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
  +lci_ndevices 7 +backend_poll_thread 2 +traceoff
```

`PARATREET_DEVICE_TREE=0` and no `FOF_GPU_*` for the CPU arm;
`FOF_GPU_VERIFY=1` for the verify arm. `LCI_ATTR_BACKEND=ofi`,
`FI_CXI_RX_MATCH_MODE=hybrid`, `FI_MR_CACHE_MONITOR=userfaultfd`,
`PMI_MAX_KVS_ENTRIES=4194304`. Built with
`GPU=1 PROJECTIONS=1` against the HIP-enabled charm
(`reconverse-linux-x86_64-amd`); see `fof/gpu/rebuild_deps.sh`.

## 8. What this sweep says to do next

1. **The phase-3/uf2/loadCache regression is now the whole story.** Phase
   1 is solved: 8-11x, scaling, exact. Everything the device path still
   costs the application lives after it, it is 1.4-2.4x on
   communication-bound phases, and it grows with node count until it
   cancels the win entirely at 128. The traces in section 5 are the tool.
2. **`loadCache` is the largest single term at 128 nodes in BOTH arms**
   (1.18 s CPU, 1.48 s device, against a 5.8 s iteration) and it
   anti-scales. That is independent of this work and older than it.
3. Phase 1's remaining cost is no longer the GPU pass (0.068 s at 128
   nodes) but the staging around it — pack + scatter is 0.067 s against
   it, and design/phase1-gpu.md section 12.1's "emit the device form at
   tree build" would remove most of the tree half.

---

# Appendix A. The 2026-08-10 CPU-only sweep (`uniform-annotation`, HEAD `16fd529`)

Kept verbatim as the prior record. The sweep above supersedes it for
current numbers; this one is the reference for what the CPU chain did
before the device path existed, and its `phase1_stages` table is the one
the new sweep's phaseA/phaseB discussion is continuous with.


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

## A.1 Correctness

**424,897,832 components, identical at every node count (8/16/32/64/128)**
— unchanged from every prior sweep, including the immediately prior
stealing-free/`-u serial` sweep.

## A.2 Top-line scaling

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

## A.3 phase1_stages breakdown (seconds) — mode-independent, comparable across sweeps

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

## A.4 phase1 detail and phase3 (uf2/walk) breakdown (seconds) — dist-mode path, not comparable to the prior serial-mode sweep

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

## A.5 Raw command (per node count N, procs/node = 8 fixed)

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
