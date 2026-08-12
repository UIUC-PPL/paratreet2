# Phase 1 on GPU: a Kokkos device implementation of the intra-process FoF

**STATUS: PLAN, no implementation. Written 2026-08-12 after a read of
fof/FoFPhase1.h (2719 lines, main), src/TreePiece.h, src/Node.h,
src/MultiData.h and the measurement ledgers
(design/fof3-2b-scaling.md, design/fof3-lambb500-scaling.md,
design/phase1-scaling.md). Supersedes nothing; it is the concrete
build-out of design/walk-unification.md's GPU dimension (stages 0 and 4
there) and of design/optimization-inventory.md's gap list, with one
structural change those documents did not commit to: on GPU the unit of
union-find becomes the PROCESS, not the PE, which deletes phaseB
instead of porting it.**

Target machine: Frontier, MI250X, 8 GCDs/node, already run as 8
processes/node x 14 PEs — i.e. the process/GCD mapping this plan needs
is the geometry already in production. Portability layer: **Kokkos**
(HIP + CUDA + a host backend from one source), which is installed on
this machine at `/ccs/home/rrao/kokkos` (4.7.00, `build-hip`, HIP +
Serial backends, `KOKKOS_ARCH_VEGA90A`, hipcc from ROCm 6.2.4,
rocThrust enabled).

---

## 1. What is being replaced, and what it costs today

Phase 1 today (FoFPhase1.h, driven by `startPhase1Chain`) is a
five-stage within-process chain:

| stage | what it does | 2B/16 nodes | 80M/1 node |
|---|---|---|---|
| phaseA | per-PE union-find, dual walks over that PE's TreePiece pairs | 2.68 s | 1.56 s |
| phaseB | cross-PE TreePiece pairs -> (tip, tip) edges through a process pool | 2.88 s | 0.15 s |
| merge | serial UF over the deposited edges -> tip map | 0.011 s | 0.003 s |
| relabel | rewrite group_number through the map | 0.31 s | 0.17 s |
| **phase 1 total (wall)** | | **5.36 s** | **1.76 s** |

(2B = cosmo25cmb 1.98B particles, 128 processes; 80M = lambb.00500.
Sources: design/fof3-2b-scaling.md, design/fof3-lambb500-scaling.md.)

Two facts from those ledgers set the target:

- **phaseA scales, phaseB does not.** phaseB is flat at 2.4-3.3 s across
  8->128 nodes at 2B and 0.12-0.21 s across 1->16 nodes at 80M. Past 8
  nodes it is the largest phase-1 term. Every attempt to move it
  (stealing, pool split tuning) has been measured and rolled back.
- **phaseA's cost is concentrated in SELF pairs** — ~60 of ~67 phase-1
  core-seconds (design/walk-unification.md finding 2).

phaseB exists for exactly one reason: phaseA's union-find is PER PE, so
pairs spanning two PEs of a process cannot union and must emit edges
instead. That is a consequence of the no-atomics/frozen-phase
discipline (design/phase1.md), which in turn exists because CPU threads
sharing a union-find need locks. **A device union-find does not have
that problem** — union-by-min-GLOBAL-ORDER is a total order, so a
lock-free CAS attach is cycle-free by construction
(design/optimization-inventory.md a.6). So the right GPU target is not
"phaseA on the device"; it is:

> One device union-find over ALL of the process's particles.
> phaseA, phaseB and merge collapse into a single device stage.

That deletes the one term in the profile that has resisted every
algorithmic fix so far, and it makes the GPU stage strictly simpler
than the CPU code it replaces (no pool, no LPT ordering, no claim
cursor, no SEEN set, no edge buffers, no tip map).

Upper bound on the win: ArborX reports 37M particles fully clustered in
0.15 s on one A100 for the FoF case (design/optimization-inventory.md,
gap 4). A process holds 15.6M particles at 2B/16 nodes and 10M at
80M/1 node; an MI250X GCD is roughly half an A100 on this kind of
kernel. That puts a well-implemented device phase 1 at **0.1-0.2 s
against today's 5.4 s** — call it 10-30x on the phase-1 core, and
honestly note that at 2B/16 nodes the remaining iteration terms
(loadCache 5.9 s, walk 1.5 s, tree build 1.0 s) then dominate: phase 1
stops being the problem rather than the run getting 5x faster.

---

## 2. Three findings that shape the design

### 2.1 The mean field is SPARSE; the tree is not doing anything silly

With b = 0.2 x mean interparticle separation d, a cube of side b holds
0.2^3 = **0.008 particles** at mean density. A grid over the whole
process at cell side b would have ~125 empty cells per particle, and
the mean number of particles within b of a given particle is
(4/3)pi b^3 rho = 0.034 x overdensity. This is why the CPU's
`gridSelfUnion` is GATED on occupancy (`-G 4`) and only fires in dense
chares: a uniform b/sqrt(6) grid with its ~160-offset stencil would
cost ~160 hash probes per particle across the sparse bulk, which is
strictly worse than a tree descent.

Consequence for the device: **cell side must be b (13 forward-half
offsets), not b/sqrt(6), for the base kernel**, with the finer grid
used only as a dense refinement (section 4.4). At c = b the stencil
volume is (3b)^3, so the expected pair tests per particle are
27 x 0.008 x overdensity = 0.216 x overdensity — 0.2 at mean density,
43 in a Delta=200 halo, and 2x10^5 in a Delta=10^6 cusp. The cusp
number is why the dense refinement is required work, not an
optimization: it is the device analog of the positive certificate and
the connectivity suppression that carry phaseA today.

### 2.2 No tree is needed for phase 1 — but the tree is free if we want it

Each TreePiece's particles are a key-sorted contiguous `std::vector`
and every node is a `(begin, n)` range into it
(TreePiece::recursiveBuild, src/TreePiece.h:461). Flattening the
existing forest into POD arrays is therefore mechanical
(walk-unification stage 0). But the cell-list formulation of section 4
needs no tree at all: it is O(N) with a sort, it handles both the
sparse bulk and dense cores, and it removes the two things that make
the CPU walk device-hostile (`Node` is polymorphic with atomic child
pointers; `cert_rep`/`cert_tip` are hash maps keyed by host node
pointer).

**Decision: v1 ships no device tree.** Keep the flat-tree work
(walk-unification stage 0) for the device phase-3 walk later, where a
tree is genuinely required because the source side is remote.

### 2.3 Downstream code binds to the REPRESENTATIVE structures, not to particles

Everything after phase 1 — `applyTipEncoding`, `relabelBody`,
`applyGlobalMap`, `applyUF2Labels`, `depositLabelCounts`,
`materializeLabels` — touches only `uf_parent`, `roots`, `rep_label`,
`root_counts` (design/relabel-representative.md). If the device stage
produces those four arrays with the same meaning, **every downstream
stage, phase 3, UF_2 and the histogram are unchanged**. That is the
property that keeps this change contained, and it is the interface
contract the device stage must meet.

One caveat found while reading for this plan, which the refactor in
section 5.3 makes load-bearing: `applyTipEncoding` writes `rep_label[r]`
for its own roots and then calls `materializeLabels()`, which READS
`rep_label` for roots that may belong to other PEs, with only a
trailing `contribute(cb)` — no barrier between the write phase and the
read phase. Today that is safe because `rep_label` is per-PE and each
PE's roots are its own. Once `rep_label` becomes process-wide (which
the device stage forces, because a component's min-order root can live
in any PE's range), **a deposit barrier must be inserted between the
encode and the materialize**. Same hazard in `applyGlobalMap` /
`applySliceOnPE` and in `relabelBody`. This is a correctness bug the
port would otherwise introduce silently, and it is precisely the class
`fof1` catches only sometimes.

---

## 3. Portability layer: Kokkos, and how it is wired to Charm

### 3.1 Kokkos over raw HIP

For: one source for MI250X and NVIDIA; `Kokkos::sort` /
`sort_by_key` already dispatching to rocThrust (enabled in the local
build) or CUB, so no per-vendor sort dependency; `TeamPolicy` with
`AUTO` team size absorbs wavefront-64 vs warp-32; device
`UnorderedMap`, atomics and `ScatterView` are portable; and — the
argument that decides it for this codebase — **the same kernel source
compiles for the Serial host backend**, giving an A/B that separates
"the algorithm changed" from "the device changed". Given how this
project gates changes (byte-identical component output, oracle arms
kept permanently: `-u serial`, `-w transposed`, `FOF_GRID_ROOT_ONLY`),
a host-executable arm of the device kernels is worth more here than it
would be in most codes.

Against: another dependency and a build seam. Not decisive — the
alternative (a hand-rolled HIP/CUDA macro layer) still needs
rocPRIM/CUB for the sort, which reintroduces the same seam with none
of the host-arm benefit.

**Decision: Kokkos 4.7, HIP + Serial backends. No Kokkos::OpenMP** —
Charm already owns the host threads and an OpenMP backend inside a
14-PE process would oversubscribe.

### 3.2 The Charm/Kokkos boundary

Hard constraints on this stack (verified): charm 8.0.0's HAPI is
CUDA-only and reconverse has no GPU support at all ("device" there is
an LCI network device). So there is no runtime-integrated stream
support; kernels launch from ordinary entry methods. Two consequences:

- **Single Kokkos owner per process.** `Kokkos::initialize` is not
  thread-safe and Kokkos does not promise that concurrent `View`
  allocation from 14 Charm PE threads is safe. The process's home PE
  (`CkNodeFirst(CkMyNode())`, already the PE that hosts the UF_2
  element and runs `mergeBody`) is the only thread that calls into
  Kokkos. With one GCD per process this costs nothing: the GPU is the
  parallel resource, and serializing dispatch through one host thread
  is the correct shape.
- **Completion without blocking the scheduler.** v1 may simply
  `Kokkos::fence()` on the home PE — the deposit chain has that PE
  doing nothing else, and network progress is on the comm thread
  (`+backend_poll_thread 2` is already in the production run line).
  v2 replaces it with an event poll from `CcdCallFnAfter`
  (`CcdCallOnConditionKeep`/`CcdCallFnAfter` do exist in reconverse:
  reconverse/src/conv-conds.cpp:247,280), which slots exactly where
  `a_done.fetch_add` sits in the chain today and re-enables overlap.

Device selection: `Kokkos::InitializationSettings().set_device_id(local
rank)` from `SLURM_LOCALID` (or `CkMyNode() % procs_per_node`), and
pass explicit settings rather than `argv` so Kokkos never parses
Charm's command line.

### 3.3 Compilation

The device translation unit includes **no Charm and no paratreet
headers** — same discipline as tests/treecache. It is compiled by
`hipcc` (or `nvcc_wrapper`) with the Kokkos flags and exposes a plain
C++ POD interface; `charmc` links the resulting object. Concretely:

```
fof/gpu/FoFDevice.h    // POD interface, includes <cstdint> and nothing else
fof/gpu/FoFDevice.cpp  // Kokkos; built by hipcc
fof/gpu/Makefile       // KOKKOS_PATH, arch flags, -ffp-contract=off
```

`-DFOF_GPU` gates the host side; without it the binary is byte-identical
to today's. A runtime flag (`fof3 --gpu`, default off until the gates in
section 6 pass) selects the path, so both arms live in one binary and
can be interleaved in one job — the measurement discipline this project
already uses.

**Floating point:** `Real` is `float` by default (src/common.h:28;
`USE_DOUBLE_FP` is not set by any Makefile here), so positions are
float3 and the device kernel is a float kernel — good for bandwidth and
for NVIDIA consumer parts. But `-ffp-contract` differs between hipcc
(fast by default) and the host build, and a fused vs unfused
`dx*dx+dy*dy+dz*dz` can flip a pair sitting exactly on b. Pin
`-ffp-contract=off` on BOTH sides for the A/B arms. Expect the
component-identity gate to catch it if this is missed; that is what it
is for.

---

## 4. The device algorithm

Index space: **process-flat**, the concatenation of the process's
TreePiece particle blocks in registration order. `int32` indices (a
process never holds 2^31 particles — the same assumption `uf_parent`
already makes per PE).

Device state (per process, N ~ 10-16M):

| array | type | bytes at N=15.6M |
|---|---|---|
| `pos` | float3 | 187 MB |
| `order` | int64 | 125 MB |
| `parent` | int32 | 62 MB |
| `cell_key` / `cell_id` | int64 / int32 | 187 MB (scratch) |
| `root_counts`, `label` | int32/int64 | 190 MB |

~750 MB against 64 GB of HBM per GCD. Memory is a non-issue; this could
hold a 10x larger process.

### K0 — pack and upload (host-parallel)

Every PE packs its OWN TreePieces into its slice of a pinned SoA
staging buffer (positions + orders), in parallel, replacing today's
`phaseABody`. This matters: the AoS `Particle` is ~112 B, so a
single-threaded gather of 15.6M particles reads 1.75 GB (~0.1 s
serial); split across the process's 14 PEs it is ~10-30 ms. Upload is
312 MB, ~8 ms over Infinity Fabric.

### K1 — cell binning (c = b)

`cell_key = morton_or_linear(floor((p - origin)/b))` over the
process's bounding box (at 2B a process box is ~1235 b per side, so
3 x 21 bits is ample). `sort_by_key` on (cell_key, index) via
rocThrust/CUB, then a segmented scan to build the compressed occupied
cell list `(key, begin, end)`. ~15M keys sorts in a few ms.
Neighbor-cell lookup is a binary search over the sorted unique keys
(as the CPU `findOcc` does) or a `Kokkos::UnorderedMap` — measure both;
the UnorderedMap removes the log factor and the divergence the CPU
comment already flags.

### K2 — union-find on device

```
parent[i] init i;  // representative = min global order, by construction
KOKKOS_INLINE_FUNCTION int find(i):    // path-splitting, no locks
  while (parent[i] != i) { int g = parent[parent[i]];
                           parent[i] = g; i = g; }
KOKKOS_INLINE_FUNCTION void unite(a, b):
  for (;;) { a = find(a); b = find(b); if (a == b) return;
             // attach the LARGER global order under the SMALLER
             if (order[a] > order[b]) swap(a, b);
             if (atomic_compare_exchange(&parent[b], b, a) == b) return; }
```

Correctness: union-by-min-global-order is a total order, so the
parent graph is acyclic by construction and the CAS loop cannot
livelock into a cycle — this is stronger than the rank/size schemes
usual in GPU union-find, and the codebase already relies on the same
property on the CPU (`unite`, FoFPhase1.h:2362). The result is
**order-independent**: union-find is a semilattice, so the device's
nondeterministic execution order cannot change the final partition, and
the tip (root order = min order in the component) is deterministic.
This is the reason the exactness gates in section 6 are meaningful
despite the atomics.

### K3 — cell-pair traversal (the base kernel)

One team (wavefront) per occupied cell; each team walks the 13
forward-half neighbor offsets plus its own cell:

- probe the neighbor cell; skip if absent;
- **early-out:** if every particle of A and of B already shares one
  root, the pair can contribute nothing — skip. (O(n_a + n_b) finds
  against O(n_a x n_b) tests; this is the device form of the CPU's
  connectivity suppression, and in a merged dense core it is what keeps
  repeat visits O(1).)
- otherwise test the cross product with `Kokkos::TeamThreadRange` x
  `ThreadVectorRange`, `unite` on each hit.

Same-cell pairs are the i<j triangle, handled by the same team.
Coalescing: after K1 the particle arrays are permuted into cell order,
so a cell's particles are contiguous.

This kernel alone is an exact FoF for the process. Stages 1 of the plan
stops here.

### K4 — dense refinement (required for the cusps, section 2.1)

For cells whose occupancy exceeds a threshold, refine to the CPU's
`b/sqrt(6)` sub-grid and inherit its two test-free guarantees: same
sub-cell pairs are friends (diagonal b/sqrt(2)) and FACE-ADJACENT
sub-cell pairs are friends (max separation exactly b). Both become
`unite` with no distance test; only the residual stencil
(`gridOffsets()`, ~160 forward offsets, reachability rule
sum(((|d|-1)+)^2) <= 6) needs tests, with first-witness exit. This is
`gridSelfUnionRange` (FoFPhase1.h:2028) transliterated — an algorithm
already carrying a byte-identical exactness oracle on the CPU
(`-G 0.0001` under fof1, 80M grid on/off), which is a large de-risking
asset. Cross-pairs between two dense cells use the same sub-cell
stencil.

Threshold: start at the CPU's `-G 4` semantics (expected particles per
b/sqrt(6) cell) and sweep. Note the CPU measured -29% phaseA at 2B from
the root-gated version; on device the trade is different (the free
unions cost nothing, the extra probes do), so re-measure rather than
inherit the tuning.

### K5 — freeze, tips, counts

`find` every index (full compression), write
`label[i] = order[root[i]]` (the tip = global order of the min-order
member — the same globally unique, stable name the CPU produces), and
`atomic_fetch_add` into `root_counts[root]`. A stream compaction
produces the compact `roots` list. All three are single data-parallel
passes.

Deliberately NOT ported: the fused annotation pass
(`freezeAndAnnotate` writing `min_frag`/`max_frag`). That annotation
exists only to serve phaseB's certificates, and phaseB is gone;
`TreePiece::upwardPass` already recomputes the real annotations after
relabel, which is what phase 3 reads. Dropping it removes a device
tree-walk requirement from the critical path.

### K6 — download and scatter (host-parallel)

Download `label` (125 MB) and `parent` (62 MB, as the per-particle root
index) into pinned host buffers, then every PE scatters its own slice
into its own `Particle::group_number` and fills its `uf_parent` /
`roots` view — the same owner-writes shape as `materializeLabels()`
today, and parallel across the process's PEs.

---

## 5. Host-side changes, concretely

### 5.1 New

- `fof/gpu/FoFDevice.{h,cpp}` — the Charm-free device library above.
  Interface, roughly:
  ```c++
  struct FoFDeviceConfig { float b2; float period[3]; float dense_gate; };
  class FoFDevice {                       // one instance per process
    static bool available();
    void  init(int device_id);
    float* stagePositions(size_t n);      // pinned, PEs fill their slices
    long*  stageOrders(size_t n);
    void   run(const FoFDeviceConfig&);   // K1-K5
    const long* labels() const;           // pinned, PEs read their slices
    const int*  roots()  const;
    void   stats(FoFDeviceStats*) const;  // per-kernel walls, occupancy tail
  };
  ```
- `FoFPhase1::packDeviceSlice()` (per-PE entry) and
  `FoFPhase1::scatterDeviceLabels()` (per-PE entry) — the K0/K6 host
  halves, deposited through the existing counter pattern.
- `FoFPhase1Node::deviceStage()` — runs on the home PE, calls `run()`,
  then triggers `scatterDeviceLabels` on the process's PEs, exactly
  where `buildPoolSlice`'s last depositor triggers `phaseBChained`
  today.

### 5.2 Modified

- `startPhase1Chain` gains a branch: device path =
  pack -> deviceStage -> scatter -> (relabel becomes a no-op identity,
  since there is no tip map) -> the same final reduction with the same
  `FoFPhase1Stages` shape (report device kernel walls in the phaseA
  slot and 0 in phaseB, so every existing scaling table stays
  comparable).
- `FoFPhase1Node` gains the device instance and the process-wide
  `rep_label` / `root_counts` (section 5.3).

### 5.3 The one real refactor: process-wide representatives

A component's root is its min-order particle and can live in any PE's
range, so `rep_label`/`root_counts` must move from the per-PE
`FoFPhase1` to the process-wide `FoFPhase1Node`; each PE keeps a
`roots` list holding only the roots that fall in ITS index range, so
every root is still owned by exactly one writer and all the
`for (int r : roots)` loops stay per-PE and parallel.

**This introduces the read-after-write hazard flagged in section 2.3.**
`applyTipEncoding`, `applyGlobalMap`/`applySliceOnPE` and
`relabelBody` each write their own roots and then call
`materializeLabels()`, which reads roots owned by other PEs. Each of
those needs a deposit barrier between its write phase and its
materialize phase. Cheap (the existing counter pattern) and
non-negotiable; write it into the code as a comment naming the hazard,
because the failure mode is a silent wrong label on a fraction of
particles that only a full component-identity check catches.

### 5.4 Deleted from the device path (kept for the CPU arm)

`phaseBBody`, the pool (`poolPushInto`, `buildPoolSlice`,
`phaseb_pool`, the claim cursor), `mergeBody`, `tip_map`, the SEEN
sets, `edge_buf`, `cert_rep`/`cert_tip`, `connectedRep`, `certRep`,
`leafLeafEmit`, `emitSubtreeTips`. Nothing else in the codebase reads
them. The CPU path keeps all of it and stays the permanent oracle —
same policy as `-w transposed` and `-u serial`.

---

## 6. Correctness gates (nothing merges without these)

Reusing the harness that already exists (fof-algorithm-report.md §10):

1. **fof1 vs serial O(n^2)**, the only true phase-1 test, on
   100/1k/10k — GPU arm and CPU arm, single process. Under a device
   path this test is now the whole algorithm, so it is a much stronger
   gate than it is for the CPU code (where phase 3 could mask a phase-1
   under-merge).
2. **Multi-process runs are mandatory** (2 and 4 processes) — the
   standing rule here, and doubly relevant because the device stage
   changes what "process-local" means.
3. **80M lambb.00500: 23,707,197 components**, exact, at 1/2/4/8/16
   nodes, plus `FOF_COUNT_VERIFY=1` silent on every run.
4. **2B: 424,897,832 components, max 185,317,566**, at 8-128 nodes.
5. **Tips, not just counts.** Tips are stable names (min-order member),
   so the existing cross-configuration labeling comparison applies
   unchanged — compare the GPU arm's labeling against the CPU arm's
   directly, not only the histogram.
6. **Kokkos Serial arm**: the same device kernels compiled for the host
   backend, run at 1k/10k. A divergence between the Serial arm and the
   HIP arm is a race or an FP-contraction difference; a divergence
   between the Serial arm and the CPU walk is an algorithm error. Being
   able to tell those two apart is the main practical reason to take
   the Kokkos dependency.

---

## 7. Staged plan

Each stage is separately measurable and separately revertible.

**Stage 0 — integration spike (days, no algorithm).** Build a Charm
nodegroup that initializes Kokkos on the home PE, runs a trivial
`parallel_reduce`, and prints the GCD it landed on, under the
production run line (8 procs/node, `+ppn 14`, srun GPU binding). Then
add K0/K6 only: pack -> upload -> download -> scatter, with the CPU
still computing the answer. **Gate:** byte-identical output; staging
cost measured in isolation. This retires nearly all the integration
risk (charmc + hipcc + Kokkos + reconverse + srun binding) before any
algorithm work.

**Stage 1 — device union-find + cell list (K1-K5), c = b.** Replaces
phaseA+phaseB+merge on the device path. **Gate:** all of section 6.
**Measure:** against CPU phaseA+phaseB at 80M/1-16 nodes and 2B/8-128
nodes. Expect the sparse-bulk win immediately and a possible dense-cusp
tail; the per-kernel walls from `stats()` tell you which.

**Stage 2 — dense refinement (K4) + cell-pair early-out.** Sweep the
occupancy gate. **Measure:** the 2B occupancy tail specifically — the
regime where `-G 4` bought -29% on the CPU.

**Stage 3 — overlap and the idle-CPU question.** During the device
stage the process's 14 PEs are idle. Options in increasing order of
ambition: (a) accept it (phase 1 becomes ~2% of the iteration, so this
is defensible); (b) shrink `ppn` for GPU runs and give the cores back
to other processes — but tree build and the phase-3 walk still want
them; (c) genuine overlap, splitting the process's particles into a
device share and a CPU share with the CPU side emitting edges into the
device union-find between kernels. Decide on stage 1/2 measurements,
not now.

**Stage 4 — only if stage 2 leaves a dense-cusp floor:** the device
tree walk (walk-unification stages 0+4), i.e. flat POD nodes + a
stackless traversal + the positive certificate. Deliberately last: it
is the largest piece of work in the entire program and the cell-list
formulation may well make it unnecessary for phase 1. The flat-tree
work is not wasted either way — the device phase-3 walk needs it.

---

## 8. Risks, and what each one costs

| risk | mitigation | residual |
|---|---|---|
| Kokkos + Charm SMP thread safety | single Kokkos owner PE per process | none; costs nothing at 1 GCD/process |
| GPU binding with 8 procs/node under srun | explicit `set_device_id(SLURM_LOCALID)`; verified in stage 0 | low |
| `Kokkos::initialize` parsing Charm's argv | pass `InitializationSettings` explicitly | none |
| FP contraction (hipcc fast vs host) flipping pairs at exactly b | `-ffp-contract=off` both sides; gate 5 catches it | low, but it WILL be the first mystery diff if missed |
| dense-cusp quadratic blowup at c = b | stage 2 (K4), which is a transliteration of already-exact CPU code | medium; the reason stage 2 is planned, not optional |
| process-wide `rep_label` read-after-write | deposit barriers, section 5.3 | this is a real bug being introduced; write the barrier first, not after the first wrong answer |
| `Kokkos::fence()` blocking the home PE's scheduler | acceptable in v1 (comm thread does progress); Ccd event poll in v2 | low |
| pinned-buffer staging cost (~30 ms) | parallel per-PE pack; persistent buffer across iterations | low; measured in stage 0 before anything depends on it |

Out of scope, explicitly: phase 3 (the cross-process walk consults
`ckLocalBranch()`, a mutex and a possible mid-leaf send in its hot
path — not portable as written), the software cache, UF_2, and the
component histogram. All of them keep working unchanged because the
device stage honors the contract in section 2.3.

## 9. Open questions for the review

1. **Process geometry.** This plan assumes 1 process per GCD, which is
   the geometry already in production on Frontier. If a future
   configuration puts 2 processes on a GCD, the single-owner rule still
   holds but the two processes contend; worth stating as a supported
   or unsupported configuration up front.
2. **PBC.** The device kernel plans plain Euclidean distance plus an
   assertion that the process's box is smaller than L/2 per axis —
   which is the same condition `gridSelfUnionRange` already checks
   before it will run (FoFPhase1.h:2042). Confirm that is acceptable
   for production PBC runs, or plan the cell-index wrap.
3. **Leaf size.** `max_particles_per_leaf = 12` is a CPU tuning
   (examples/fof3/Main.C:35). The device path does not use leaves at
   all, but phase 3 does; if the tree build is retuned for the GPU
   runs, phase-3 numbers move with it. Keep the knob out of the phase-1
   A/B.
4. **Where the ArborX comparison lands.** Their 37M/0.15 s on an A100
   is per-rank shared-memory scope = our within-process phase 1
   exactly, so after stage 2 we have a directly comparable number for
   the first time — and, unlike them, we still have the distributed
   closure. Worth publishing the pair.
