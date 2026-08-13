# Phase 1 on GPU: a Kokkos device port of the intra-process FoF

**STATUS: PLAN, no implementation. Written 2026-08-12; revised the same
day after review (Ritvik) on three points: one process per GPU is an
enforced invariant, the tree is KEPT (and made free), and the scope is
phase 1 only — cross-GPU tree walks for phase 3 are explicitly later
work. Built from a read of fof/FoFPhase1.h, src/TreePiece.h,
src/Node.h, the measurement ledgers (design/fof3-2b-scaling.md,
design/fof3-lambb500-scaling.md, design/phase1-scaling.md), and the
working Charm++/Kokkos/HIP precedent in ~/jacobi2D.**

This is the build-out of design/walk-unification.md's GPU dimension
(its stages 0 and 4) and of design/optimization-inventory.md's gap
list, with one structural change those documents did not commit to: on
the device the unit of union-find becomes the PROCESS, not the PE,
which DELETES phaseB rather than porting it.

---

## 0. Correction to design/walk-unification.md

That document states: "Charm GPU integration: NONE on this stack —
charm 8.0.0 HAPI is CUDA-only, reconverse has no GPU support at all."
**That is no longer true of this tree**, and the difference reshapes
the integration:

- `charm_reconverse/src/arch/cuda/hybridAPI/hapi_portable.h` maps the
  whole HAPI surface onto CUDA *or* HIP (`CMK_CUDA` / `CMK_HIP`).
- `reconverse-linux-x86_64-amd/` is a HIP-enabled build
  (`include/conv-mach-hip.sh`, `BUILD_HIP=1`, ROCm 6.2.4) and ships
  `lib/libhybridapi.a`.
- `hapiAddCallback(hapiStream_t, const CkCallback&, void*)` exists
  (hapi_impl.cpp:1886) — stream completion delivered as a Charm
  callback. So device completion is message-driven; nothing has to
  block a scheduler thread or poll.
- ~/jacobi2D is a working Charm++ + Kokkos + HIP program on Frontier
  over this runtime, including device-to-device communication
  (`+gpushm`, `+gpuipceventpool`, `+gpucommbuffer`).

Everything below assumes that stack. Note for the build matrix: the
top-level `charm_reconverse/include` symlink points at
`reconverse-linux-x86_64` (no HIP), so the GPU arm must be built with
`CHARM_HOME=$HOME/charm_reconverse/reconverse-linux-x86_64-amd`, and
the sibling stack (unionfind, prefixLib, htram) has to be rebuilt
against that same build.

---

## 1. What is being replaced, and what it costs today

| stage | what it does | 2B/16 nodes | 80M/1 node |
|---|---|---|---|
| phaseA | per-PE union-find, dual walks over that PE's TreePiece pairs | 2.68 s | 1.56 s |
| phaseB | cross-PE TreePiece pairs -> (tip, tip) edges through a process pool | 2.88 s | 0.15 s |
| merge | serial UF over deposited edges -> tip map | 0.011 s | 0.003 s |
| relabel | rewrite group_number through the map | 0.31 s | 0.17 s |
| **phase 1 total (wall)** | | **5.36 s** | **1.76 s** |

**phaseA scales, phaseB does not** — flat at 2.4-3.3 s across 8->128
nodes at 2B, and past 8 nodes the largest `phase1_stages` term. Every
attempt to move it (stealing, pool-split tuning) has been measured and
rolled back.

phaseB exists for exactly one reason: phaseA's union-find is PER PE, so
pairs spanning two PEs cannot union and must emit edges instead. That
is forced by the no-atomics/frozen-phase discipline
(design/phase1.md), which exists because CPU threads sharing a
union-find need locks. A device union-find does not have that problem:
union-by-min-GLOBAL-ORDER is a total order, so a lock-free CAS attach
is cycle-free by construction (design/optimization-inventory.md a.6).
So:

> **One device union-find over ALL of the process's particles.
> phaseA + phaseB + merge collapse into a single device stage.**

Anchor for the target: ArborX reports 37M particles fully clustered in
0.15 s on one A100 for the FoF case; a process holds 15.6M particles at
2B/16 nodes and 10M at 80M/1 node, and an MI250X GCD is roughly half an
A100 here. That puts a good device phase 1 at **0.1-0.2 s against
today's 5.4 s**. Honest framing of the payoff: at 2B/16 nodes the other
iteration terms (loadCache 5.9 s, walk 1.5 s, tree build 1.0 s) then
dominate — phase 1 stops being the problem rather than the whole run
getting 5x faster.

---

## 2. One process, one GPU (invariant, not a preference)

Enforced, not assumed:

- Frontier has 8 GCDs/node and the production run line is already 8
  processes/node x `+ppn 14`. So the required geometry is the geometry
  in use; nothing about the launch changes except the GPU binding.
- At startup the FoF nodegroup checks `hapiGetDeviceCount()` against
  the number of processes on the physical node
  (`CmiNumPesOnPhysicalNode` / `CmiMyNodeSize`, which HAPI itself uses
  for its device mapping, hapi_impl.cpp:571-577) and **aborts with a
  clear message** if the mapping is not 1:1. Two processes sharing a
  GCD is an unsupported configuration, not a slow one.
- The device id comes from `hapiGetDevice()` — HAPI has already done
  the PE->GPU mapping — and is handed to Kokkos, exactly as jacobi2D
  does it.

Consequence used throughout: the process's GPU is a single resource
with a single owner, so orchestration is serialized through one host
thread by design, not by limitation.

---

## 3. Keeping the tree, and making it free

Reviewer question: *"can we keep the tree without any cost?"* Yes — and
on the measurements below the tree should be kept because it is
**faster than the tree-free alternative in the regime that dominates**,
not merely because it is available. This reverses the first draft of
this plan.

### 3.1 Why the tree wins in the sparse bulk

With b = 0.2 x mean interparticle separation, a cube of side b holds
0.2^3 = **0.008 particles** at mean density. A tree-free cell list at
cell side ~b therefore has ~1 particle per occupied cell in the field,
so occupied cells ~ N and the base cost is 13 neighbor probes per
particle: ~200M hash probes at N=15.6M, tens of milliseconds and poor
locality.

The tree, walked LEAF-BATCHED, is cheaper: a 12-particle leaf spans
~11.5 b in the field, so one traversal amortizes over 12 particles, and
the descent is ~20-25 node tests of branch-free float arithmetic on
contiguous arrays. ~1.3-2.6M leaves x ~25 tests ~ 30-60M cheap
operations against 200M scattered probes. This is also what ArborX does
to reach its number (BVH traversal + DenseBox for the dense cells), and
it is why the CPU code gates `gridSelfUnion` on occupancy instead of
using it everywhere.

The grid is still the right tool where the CPU already says it is: in
dense nodes, where its free unions collapse work the pair tests cannot.
So the device algorithm is the CPU algorithm — tree walk with
certificates, grid inside dense nodes — not a replacement for it.
That also makes the exactness story far stronger: the same
certificates, the same suppression semantics, the same tips, so stage-
by-stage A/B against the CPU arm is meaningful.

### 3.2 Why it costs nothing to have

Three facts make the device tree essentially free:

1. **The tree is built anyway.** upwardPass, the cache and phase 3 all
   need it; phase 1 is not what pays for it.
2. **Flattening happens at tree build, not in phase 1.** It is done as a
   separate breadth-first pass after `populateTree()`, which is what
   makes children contiguous; `recursiveBuild`'s depth-first order does
   not. **MEASURED CORRECTION (section 13): this is not free** — it cost
   +83 ms on a 561 ms tree build at 80M. It stays out of the phase-1
   wall, which is what matters for the comparison, but the "one store
   inside a walk that already happens" claim in the first draft of this
   plan was wrong, and emitting from inside `recursiveBuild` is the way
   to make it true.
3. **Node particle ranges are contiguous by construction.** A node's
   particles are `node_particles + start`, a slice of the TreePiece's
   key-sorted vector. So a `DNode` is just
   `{box, part_begin, n_below, child_begin, child_mask, depth}` — and,
   crucially, "union everything under this node" is a flat loop over a
   contiguous range, not a subtree walk (see the positive certificate
   in section 5).

```c++
struct DNode {              // 44 B; dense integer indices, no pointers
  float  lo[3], hi[3];      // FragData::box (Real is float, src/common.h:28)
  int    part_begin;        // into the process-flat particle array
  int    n_below;           // FragData::n_below (exists since 13b1f08)
  int    child_begin;       // dense index of first child, -1 if leaf
  unsigned char child_mask; // which of the 8 children exist
  unsigned char depth;
};
```

**Per-process forest -> one tree.** The device needs a single root, not
~100 TreePiece roots. Under oct decomposition the TreePiece keys ARE
oct-node keys, so the interior above them is reconstructible from the
keys alone: the process's home PE builds a small top tree (~100 leaves,
microseconds, host-side) whose leaves point at the flattened TreePiece
roots. No canopy communication, no new tree build.

**Sizing at N = 15.6M:** ~3M nodes x 44 B = ~130 MB uploaded once per
tree build, ~4 ms over Infinity Fabric. That upload is the only genuinely
new cost of keeping the tree. Dense node indices are also a CPU-side
win the walk-unification doc already noted: they turn `cert_rep` /
`cert_tip` from pointer-keyed hash maps into dense arrays.

**Leaf size.** `max_particles_per_leaf = 12` (examples/fof3/Main.C:35)
is a CPU tuning; 12 lanes of a 64-wide wavefront is thin. Mitigation in
the kernel (lanes cooperate over the candidate fan-out and over the
flattened leaf-pair cross product) rather than by retuning the build,
because the leaf size also moves phase 3. Keep it out of the phase-1
A/B; sweep it separately if the kernel proves leaf-bound.

---

## 4. What downstream code requires (the contract that keeps this contained)

Everything after phase 1 — `applyTipEncoding`, `relabelBody`,
`applyGlobalMap`, `applyUF2Labels`, `depositLabelCounts`,
`materializeLabels` — touches only `uf_parent`, `roots`, `rep_label`,
`root_counts` (design/relabel-representative.md). **If the device stage
produces those four with the same meaning, phase 3, UF_2, the histogram
and the tip encoding are unchanged.** That is the interface contract.

One hazard found while reading for this plan, which section 6.3 makes
load-bearing: `applyTipEncoding` writes `rep_label[r]` for its own
roots and then calls `materializeLabels()`, which reads `rep_label` for
roots that may belong to other PEs, with only a trailing
`contribute(cb)` — no barrier between the write phase and the read
phase. Safe today only because `rep_label` is per-PE. Once it becomes
process-wide (which a process-wide union-find forces, since a
component's min-order root can live in any PE's range), **a deposit
barrier is required between encode and materialize** — same in
`applyGlobalMap`/`applySliceOnPE` and `relabelBody`. Write the barrier
first; the failure mode is a silent wrong label on a fraction of
particles.

---

## 5. The device algorithm

Index space: **process-flat**, the concatenation of the process's
TreePiece particle blocks in registration order, `int32` indices (the
same assumption `uf_parent` already makes per PE).

### K0 — pack and upload (host-parallel, per PE)

Each PE packs its OWN TreePieces into its slice of a pinned SoA buffer
(`float3 pos`, `int64 order`), replacing today's `phaseABody`. This
matters: `Particle` is ~112 B, so a single-threaded gather of 15.6M
particles reads 1.75 GB (~0.1 s); split across the process's 14 PEs it
is ~10-30 ms. Device payload is 20 B/particle (the same 20 B
`FoFCachedParticle` already ships on the wire) = 312 MB, ~8 ms. Each PE
issues its own async H2D copy on its own HAPI stream
(`ExecSpace(hapiGetStream())`, the jacobi2D pattern); the owner joins
on an event.

### K1 — union-find primitives

```c++
parent[i] = i;                       // root = min global order, by construction
KOKKOS_INLINE_FUNCTION int find(int i) {          // path splitting, lock-free
  while (parent[i] != i) { int g = parent[parent[i]]; parent[i] = g; i = g; }
  return i;
}
KOKKOS_INLINE_FUNCTION void unite(int a, int b) {
  for (;;) {
    a = find(a); b = find(b);  if (a == b) return;
    if (order[a] > order[b]) { int t = a; a = b; b = t; }  // attach larger under smaller
    if (Kokkos::atomic_compare_exchange(&parent[b], b, a) == b) return;
  }
}
```

Union-by-min-global-order is a total order, so the parent graph is
acyclic by construction and the CAS loop cannot livelock into a cycle —
stronger than the rank/size schemes usual in GPU union-find, and the
CPU `unite` (FoFPhase1.h:2362) already relies on the same property.
**The result is order-independent**: union-find is a semilattice, so
the device's nondeterministic execution order cannot change the final
partition, and the tip (root order = min order in the component) is
deterministic. That is what makes the exactness gates in section 7
meaningful despite the atomics.

### K2 — leaf-batched traversal (the base kernel)

One team per leaf L, an explicit stack of node indices (depth <= 32,
team scratch), descending the process forest:

- **Negative certificate:** `mindist2(box_L, box_M) > b2` -> prune.
  Identical predicate to the CPU walk.
- **Positive certificate:** `maxdist2(box_L, box_M) <= b2` -> every
  cross pair is a friend. Union `rep(L)` with `certRep(M)` and STOP
  descending. `certRep(M)` is the device form of the CPU memo
  (FoFPhase1.h:2186): a dense `int node_rep[n_nodes]` array; the first
  certificate on M unions M's CONTIGUOUS particle range
  `[part_begin, part_begin + n_below)` through one representative and
  publishes it with a CAS; every later certificate is O(1). Contiguity
  is what makes this a flat loop instead of a subtree walk. Keep the
  CPU's cheap gate (`(measure_a+measure_b)^2 <= 12 b^2`) before the
  full `maxdist2`.
- **Suppression:** if `node_rep[L]` and `node_rep[M]` are both
  published and `find` to the same root, the pair can contribute
  nothing -> prune at any level. Monotone positive memo, exactly the
  CPU's `connectedRep`; races are benign (worst case, repeated work).
- **Each pair once:** descend only into candidate leaves with
  `leaf_id > my_id`, plus the self-leaf triangle — the structural
  i<j rule (optimization-inventory gap 1, whose CPU fix measured ~30%
  at 80M/480 PEs).
- **Leaf-leaf:** lanes cooperate over the flattened cross product
  (12x12 = 144 -> ~3 wavefront iterations), `unite` on each hit, with
  the single-witness early exit when both leaves are already known
  connected.

Pass ordering: run an intra-TreePiece pass before the cross pass, the
device analog of the CPU's self-pairs-first "merge-early" ordering, so
the cross pass sees maximal suppression. Correctness does not depend on
it (semilattice); throughput does.

### K3 — dense-node grid fast path

The CPU's per-level `-G` gate (walk's self branch, FoFPhase1.h:1949)
transliterated: a node whose occupancy `n_below * c^3 / vol >= thresh`
(c = b/sqrt(6)) is solved by the cell grid instead of descending —
same-cell pairs are friends (diagonal b/sqrt(2)), FACE-ADJACENT cell
pairs are friends (max separation exactly b), and only the ~160-offset
residual stencil needs distance tests, with first-witness exit. Because
a node's particles are contiguous, the grid runs over a range exactly
as `gridSelfUnionRange` does (FoFPhase1.h:2028). On device: bin, sort
by cell key (rocThrust/CUB via `Kokkos::sort_by_key`, `libkokkos
algorithms.a` is in the local install), segmented scan for occupied
cell runs, dense cell index instead of the CPU's `findOcc` binary
search (the divergence the CPU comment already flags).

This is the piece that keeps dense cusps from going quadratic: pair
tests per particle scale as 0.216 x overdensity, i.e. 0.2 in the field,
43 at Delta=200, ~2x10^5 at Delta=10^6. It is required work, not an
optimization. It also arrives with a byte-identical CPU oracle already
in the tree (`-G 0.0001` under fof1; 80M grid on/off).

Threshold: start from the CPU's `-G 4` semantics and sweep. Do NOT
inherit the CPU tuning — on device the free unions cost nothing and the
extra probes do, so the trade moves.

### K4 — freeze, tips, counts

`find` every index (full compression), write
`label[i] = order[root[i]]` (the tip = global order of the min-order
member, the same globally unique stable name the CPU produces), and
`atomic_fetch_add` into `root_counts[root]`; stream-compact the
`roots` list. Three data-parallel passes.

Deliberately NOT ported: the fused `min_frag`/`max_frag` annotation
(`freezeAndAnnotate`). It exists only to serve phaseB's certificates,
and phaseB is gone; `TreePiece::upwardPass` already recomputes the real
annotations after relabel, which is what phase 3 reads.

### K5 — download and scatter (host-parallel, per PE)

Download `label` and `parent` into pinned buffers; each PE scatters its
own slice into its own `Particle::group_number` and fills its
`uf_parent` view and its `roots` list (roots falling in its own index
range) — the same owner-writes shape as `materializeLabels()` today.
Completion of the D2H copy is signalled with `hapiAddCallback`, so the
deposit chain stays message-driven.

**Memory at N = 15.6M:** pos 187 MB + order 125 MB + parent 62 MB +
labels 125 MB + tree 130 MB + node memos 12 MB + grid scratch (dense
nodes only) — well under 1 GB against 64 GB of HBM. Memory is not a
constraint; this would hold a 10x larger process.

---

## 6. Host-side changes

### 6.1 Kokkos ownership and initialization (jacobi2D pattern, one fix)

jacobi2D initializes Kokkos in a `KokkosGroup` **group** constructor:

```c++
int device; hapiCheck(hapiGetDevice(&device));
Kokkos::InitializationSettings args; args.set_device_id(device);
Kokkos::initialize(args);
hapiCreateStreams();
```

A Charm *group* has one branch per PE, so at `+ppn 14` that calls
`Kokkos::initialize` fourteen times in one process. **Use a nodegroup**
(one branch per process) — which is also where phase 1's process-scoped
state already lives (`FoFPhase1Node`) and which is the natural owner of
the device instance under the one-process-one-GPU invariant. Pass
`InitializationSettings` explicitly so Kokkos never parses Charm's
argv. `Kokkos::finalize()` from the same nodegroup at exit. (Worth
fixing in jacobi2D too if it is ever run at ppn > 1.)

Per-PE Kokkos use is limited to staging (View allocation + async copies
on that PE's own HAPI stream) — a pattern jacobi2D already exercises
from many chares in one process. All compute kernels are launched by
the process's home PE (`CkNodeFirst(CkMyNode())`, already the PE that
hosts the UF_2 element and runs `mergeBody`).

### 6.2 New files

```
fof/gpu/FoFDevice.h     // POD interface: no Kokkos, no Charm in the header
fof/gpu/FoFDevice.cpp   // Kokkos kernels; compiled by hipcc
fof/gpu/Makefile        // KOKKOS_DIR, arch flags, -ffp-contract=off
src/DeviceTree.h        // POD DNode + the recursiveBuild emit hook
```

The interface is POD so `FoFPhase1.h` (heavy Charm templates) is never
seen by hipcc:

```c++
struct FoFDeviceConfig { float b2; float dense_gate; };
class FoFDevice {                          // one per process
  static bool available();
  void   init(int device_id);
  float* stagePositions(size_t n);         // pinned; PEs fill their slices
  long*  stageOrders(size_t n);
  void   uploadTree(const DNode*, int n_nodes, const int* tp_roots, int n_tp);
  void   run(const FoFDeviceConfig&, void* completion_cb);  // K1-K4
  const long* labels() const; const int* parents() const;
  void   stats(FoFDeviceStats*) const;     // per-kernel walls, occupancy tail
};
```

Build (paratreet2 is Makefile-based; jacobi2D's CMake is the reference
for the flags): compile `FoFDevice.cpp` with `hipcc` +
`-I$(KOKKOS_DIR)/include --offload-arch=gfx90a`, link with charmc plus
`-L$(KOKKOS_DIR)/lib64 -lkokkoscore -lkokkoscontainers
-lkokkosalgorithms`, against
`CHARM_HOME=.../reconverse-linux-x86_64-amd`. `-DFOF_GPU` gates the
host side; without it the binary is byte-identical to today's. A
runtime flag (`fof3 --gpu`, default off until section 7 passes) selects
the arm, so both live in one binary and can be interleaved in one job —
the measurement discipline this project already uses.

**Floating point:** `Real` is `float` by default (src/common.h:28;
no Makefile here sets `USE_DOUBLE_FP`), so this is a float kernel —
good for bandwidth and for NVIDIA parts. But hipcc defaults to
`-ffp-contract=fast`, and a fused vs unfused `dx*dx+dy*dy+dz*dz` can
flip a pair sitting exactly on b. Pin `-ffp-contract=off` on BOTH
sides for the A/B arms.

### 6.3 Modified

- `startPhase1Chain` gains a device branch: pack -> `deviceStage` ->
  scatter -> (relabel is identity: no tip map exists) -> the same final
  reduction with the same `FoFPhase1Stages` shape, so every existing
  scaling table stays comparable (device kernel walls reported in the
  phaseA slot, 0 in phaseB).
- `FoFPhase1Node` owns the `FoFDevice` and the now process-wide
  `rep_label` / `root_counts`; each PE keeps a `roots` list holding
  only roots in its own index range, so every root still has exactly
  one writer and all `for (int r : roots)` loops stay per-PE.
- **Deposit barriers** between the write and materialize phases of
  `applyTipEncoding`, `applyGlobalMap`/`applySliceOnPE` and
  `relabelBody` (section 4). Non-negotiable.
- `TreePiece::recursiveBuild` / `buildTree`: emit the flat `DNode`
  array beside `flat_subtree` (section 3.2).

### 6.4 Unused on the device path (kept, as the permanent CPU oracle)

`phaseBBody`, the pool (`poolPushInto`, `buildPoolSlice`,
`phaseb_pool`, the claim cursor), `mergeBody`, `tip_map`, the SEEN
sets, `edge_buf`, `leafLeafEmit`, `emitSubtreeTips`. Same policy as
`-w transposed` and `-u serial`: the CPU arm stays and is the oracle.

---

## 7. Correctness gates

Reusing the harness that exists (fof-algorithm-report.md §10):

1. **fof1 vs serial O(n^2)** on 100/1k/10k, GPU and CPU arms. Under a
   device path this is the whole algorithm, so it is a much stronger
   gate than it is for the CPU code (where phase 3 can mask a phase-1
   under-merge).
2. **Multi-process runs mandatory** (2 and 4 processes) — the standing
   rule, and doubly relevant since the device stage changes what
   "process-local" means.
3. **80M lambb.00500: 23,707,197 components** at 1/2/4/8/16 nodes, with
   `FOF_COUNT_VERIFY=1` silent on every run.
4. **2B: 424,897,832 components, max 185,317,566** at 8-128 nodes.
5. **Tips, not just counts.** Tips are stable names (min-order member),
   so compare the GPU arm's labeling directly against the CPU arm's,
   not only the histogram.
6. **Kokkos Serial arm**: the same kernels compiled for the host
   backend, at 1k/10k. Serial-vs-HIP divergence = a race or an
   FP-contraction difference; Serial-vs-CPU-walk divergence = an
   algorithm error. Telling those apart is the main practical reason to
   take the Kokkos dependency rather than hand-rolled HIP.
7. **Per-kernel A/B arms**, so a regression bisects: `FOF_GPU_NO_CERT`
   (drop the positive certificate), `FOF_GPU_NO_SUPPRESS`,
   `FOF_GPU_GRID_ONLY` (tree-free cell list, the arm that tests
   section 3.1's claim directly), `FOF_GPU_DENSE_GATE=<t>`.

---

## 8. Staged plan

**Stage 0 — integration spike. COMPLETE 2026-08-12, all gates green;
section 11 has the measurements and the traps it cost.** Both halves
done: the standalone/Charm integration gates, and the K0/K5 staging
round trip inside FoF3 against real TreePiece particle blocks with the
CPU chain still computing the answer.

**Stage 1 — flat device tree, built at tree-build time.** `DNode` emit
in `recursiveBuild`, the per-process top tree from TreePiece keys, and
upload. **Gate:** CPU results unchanged (the emit is additive);
tree-build delta and upload time measured. Side benefit to bank
immediately: dense node indices let `cert_rep`/`cert_tip` become dense
arrays on the CPU too.

**Stage 2 — device union-find + leaf-batched traversal (K1, K2).**
Replaces phaseA + phaseB + merge on the device path. **Gate:** all of
section 7. **Measure:** against CPU phaseA+phaseB at 80M/1-16 nodes and
2B/8-128 nodes; the per-kernel `stats()` walls say whether the residue
is sparse-bulk traversal or dense-cusp pair work.

**Stage 3 — dense-node grid fast path (K3)** and the dense-gate sweep.
**Measure:** the 2B occupancy tail specifically — the regime where
`-G 4` bought -29% on the CPU.

**Stage 4 — the idle-CPU question.** During the device stage the
process's 14 PEs are idle. Options in increasing ambition: (a) accept
it (phase 1 becomes ~2% of the iteration — defensible); (b) shrink
`ppn` for GPU runs, though tree build and the phase-3 walk still want
the cores; (c) real overlap — a CPU share of the particles feeding
`unite` between kernels, or CPU work pulled forward from other phases.
Decide on stage 2/3 measurements, not now.

**Explicitly later, not in this plan:** phase 3 on the device and
cross-GPU tree walks. The device-to-device path is proven on this stack
(jacobi2D, `+gpushm`/`+gpuipceventpool`), and the flat tree from stage
1 is exactly what a device phase-3 walk would consume — but phase 3's
hot path calls `ckLocalBranch()`, takes a mutex and can send a message
mid-leaf, so it is a separate design. Nothing in this plan forecloses
it; the device tree and the device particle arrays can simply stay
resident.

---

## 9. Risks

| risk | mitigation | residual |
|---|---|---|
| Kokkos init once per process | nodegroup, not group (section 6.1) | none, once written this way |
| >1 process per GCD | startup abort (section 2) | none; unsupported by design |
| FP contraction (hipcc fast vs host) flipping pairs at exactly b | `-ffp-contract=off` both sides; gate 5 catches it | low, but it WILL be the first mystery diff if missed |
| dense-cusp quadratic blowup before stage 3 | stage 3 is planned work, not optional; it transliterates already-exact CPU code | medium until stage 3 lands |
| process-wide `rep_label` read-after-write | deposit barriers (section 4), written first | this is a bug being introduced — treat it as such |
| 12-particle leaves under-filling a 64-wide wavefront | lane cooperation over fan-out and leaf-pair cross product; leaf size swept separately | medium; the most likely source of a disappointing first number |
| staging cost (~30 ms pack + ~8 ms upload) | per-PE parallel pack; persistent pinned buffers across iterations | low; measured in stage 0 before anything depends on it |
| build matrix (HIP charm build + sibling rebuild) | pinned in stage 0 | low, but it is a full rebuild of unionfind/prefix/htram/paratreet/fof |

## 10. Open questions for review

1. **PBC.** The device kernel plans plain Euclidean distance plus an
   assertion that the process's box is under L/2 per axis — the same
   condition `gridSelfUnionRange` already checks before it will run
   (FoFPhase1.h:2042). Confirm that is acceptable for production PBC
   runs, or plan the cell-index wrap.
2. **Where the ArborX comparison lands.** Their 37M/0.15 s on an A100
   is per-rank shared-memory scope = our within-process phase 1
   exactly. After stage 3 there is a directly comparable number for the
   first time — and, unlike them, we still have the distributed
   closure. Worth publishing the pair.
3. **Does the CPU arm keep phaseB at all long-term**, or does the
   process-wide union-find idea come back to the CPU as a locked/
   sharded variant? The device result will say whether phaseB's floor
   was ever anything but an artifact of the no-atomics discipline.

---

## 11. Stage 0: measured (2026-08-12)

Both gates pass on Frontier. Code: `fof/gpu/` (device library +
standalone gate) and `tests/kokkos-spike/` (Charm integration gate);
run with `fof/gpu/run_stage0.sbatch`. Job 5254721.

### 11.1 What ran

- **Standalone gate** (`fof/gpu/fof-device-test`, no Charm, no HAPI —
  the tests/treecache discipline): 4M particles, exact label round trip
  and exact bounding box against a host recomputation.
- **Integration gate** (`tests/kokkos-spike`): 8 processes x 14 PEs on
  one node, 4M particles per process, every PE filling its own slice of
  one pinned buffer, atomic deposit counter, launch on the home PE,
  completion by `hapiAddCallback`, per-PE verification, reduction.
  Every particle round-tripped on every process.

HAPI's own line confirms the invariant of section 2 without any work on
our side: `HAPI> Config: 1 device(s) per process, 14 PE(s) per device,
8 device(s) per host`, with the eight processes binding devices 0-7.

### 11.2 Numbers

| | 4M particles/process |
|---|---|
| upload (80 MB: float3 + int64) | 3.76 ms (~21 GB/s) |
| identity kernel + bbox + checksum | 1.43 ms |
| download (32 MB) | 1.28 ms |
| async round trip, enqueue -> Charm callback, 8 processes at once | 8.3-11.9 ms |

Extrapolating the staging to a 2B-scale process (15.6M particles):
~15 ms upload, ~5 ms download. That is consistent with section 5's
budget and small against the 5.4 s it is meant to replace. The gap
between 6.5 ms of measured device work and the 8-12 ms async wall is
callback latency plus eight processes sharing node bandwidth; worth
re-measuring once real work sits between the copies.

**Do not quote these as performance numbers yet**: the HIP charm build
(`reconverse-linux-x86_64-amd`) is `CMAKE_BUILD_TYPE=Debug`. It must be
rebuilt Release before any stage-2 timing claim. The device library
itself is `-O3` and unaffected.

### 11.3 The seven traps (all now encoded in the Makefiles)

1. **A `Kokkos::HIP` member cannot exist before `Kokkos::initialize`.**
   Holding an `ExecSpace` in the pimpl aborts at chare construction with
   `Kokkos::HIP::HIP instance constructor : ERROR device not
   initialized`, because the nodegroup constructs the Device long before
   `init()`. Store the stream handle and wrap it on demand — wrapping is
   a handle copy, not an acquisition.
2. **A nodegroup entry method does NOT run on the process's home PE.**
   Measured: the `start` broadcast landed on PEs 9, 20, 30, 46, 61, 72,
   88, 105 — none of them `CkNodeFirst` values. The single-owner design
   of section 6.1 therefore has to be *made* true: the nodegroup entry
   immediately hops to `CkNodeFirst(CkMyNode())` through a PE-addressed
   group entry, and every Kokkos/HAPI call happens there. (The
   completion callback likewise lands on an arbitrary PE, which is
   harmless — the HAPI event was recorded on, and polled by, the home
   PE.)
3. **Two ROCm installs.** charm's HIP support was configured against
   `/opt/rocm-default` (-> 6.4.2) and charmc hardcodes
   `-L/opt/rocm-default/lib` on every link; Kokkos was built with hipcc
   from 6.2.4, which is also what the run environment loads. Naming
   6.2.4's lib dir in the link makes ld resolve 6.4.2's libamdhip64
   against 6.2.4's libhsa-runtime64 and fail on
   `hsa_amd_enable_logging@ROCR_1`. Worked around with
   `-Wl,--allow-shlib-undefined` (nothing we call is involved) so the
   runtime resolves one consistent 6.2.4 stack through LD_LIBRARY_PATH.
   **Proper fix, recommended before stage 2: rebuild the HIP charm with
   `ROCM_PATH=/opt/rocm-6.2.4`.**
4. **`-D__HIP_PLATFORM_AMD__` is required** on any charmc-compiled TU
   that includes `hapi.h`. hipcc defines it implicitly, g++ does not, and
   without it `hip_runtime_api.h` declares no `hipStream_t` — so every
   `hapi*` prototype naming one silently fails to declare, and the error
   surfaces as "hapiAddCallback was not declared in this scope".
   `hapi_portable.h` does not include the vendor header itself, so the
   TU must include `<hip/hip_runtime_api.h>` (host-only; not
   `hip_runtime.h`) before `hapi.h`.
5. **hipcc compiles archives as source.** It puts `-x hip` in front of
   every positional argument, so `hipcc ... libfoo.a` fails with "source
   file is not valid UTF-8". Archives must arrive through `-L`/`-l`.
6. **charmc's link line names liblci.so but not liblct.so**, so ld
   cannot resolve the transitive DT_NEEDED ("undefined reference to
   `LCT_*`"). Needs `-Wl,-rpath-link` into `charm_reconverse/lci/lib64`;
   adding `-Wl,-rpath` for it and for the charm build's own `lib` also
   makes the binary runnable without LD_LIBRARY_PATH setup
   (`libreconverse.so` is not on any default path either).
7. **charmc defaults to `-std=gnu++11`** here; Kokkos needs C++17 in any
   TU that sees it, and the firewall header is cleaner at 17. Pass
   `-c++-option -std=gnu++17`.

None of these are deep, but together they are most of what "does the
integration work" meant, and all of them would have been discovered
inside the much larger stage-2 change instead.

### 11.4 What the firewall bought

`tests/kokkos-spike/spike.C` is compiled by charmc and includes **no
Kokkos header**; `fof/gpu/FoFDevice.cpp` is compiled by hipcc and
includes **no Charm header**. They meet at a POD interface. This is a
deliberate departure from the jacobi2D precedent, which compiles the
Charm translation unit itself with hipcc — workable for a single-file
program, but paratreet2's template stack should not go through hipcc,
and with the firewall it does not have to. The standalone gate is the
payoff: when something breaks, it says whether the problem is Kokkos or
the integration before you start looking.

---

## 12. Stage 0 second half: staging inside FoF3 (2026-08-12)

Job 5254944, one Frontier node. `FOF_GPU_STAGE0=1` runs the staging
chain between registration and the phase-1 chain, where the registered
particle blocks are live and nothing has written `group_number` yet. It
is purely additive — the CPU chain still computes the answer — so the
gate is that BOTH the round trip is exact and the components are
unchanged.

| arm | components | round-trip failures |
|---|---|---|
| 100k, 2 processes, CPU-only | 33,933 (`TEST PASSED`, full O(n^2)) | — |
| 100k, 2 processes, staging | 33,933 (`TEST PASSED`) | 0 |
| 80M lambb.00500, 8 processes, CPU-only | 23,707,197 | — |
| 80M lambb.00500, 8 processes, staging | 23,707,197 | 0 |

23,707,197 is the standing gate value for this dataset
(design/fof3-lambb500-scaling.md), so the staging arm reproduces it
exactly. "Round-trip failures 0" is the stronger of the two checks: the
device pass is the identity, so every particle's returned value must be
that particle's own `order` — which verifies the per-PE base/offset
arithmetic through the process-flat index space end to end, not merely
that a transfer happened.

### 12.1 What staging costs (10.0-10.1M particles per process)

| step | max over PEs/processes |
|---|---|
| K0 pack (AoS `Particle` -> 20 B SoA, per PE, parallel) | 88 ms |
| device round trip (enqueue -> Charm callback) | 25 ms |
| K5 read-back pass (per PE, parallel) | 45 ms |
| whole stage wall | 0.80 s |

Two things to read off this:

- **The pack is node-bandwidth-bound, not thread-bound.** Each PE gathers
  ~715k particles out of 112-byte structs — 80 MB read for 14 MB written
  — and 14 PEs x 8 processes is ~9 GB of strided reads per node in 88 ms,
  i.e. ~100 GB/s. Splitting it further will not help; the fix, if it ever
  matters, is to stop gathering (write the device form at tree build,
  where the sort already touches every particle).
- **The 0.80 s wall is mostly first-touch allocation**, not the 158 ms of
  work above it: `resize()` does ~200 MB of `hipHostMalloc` per process
  inside the timed region, and pinned allocation runs around 1 GB/s. In
  stage 2 the buffers are allocated once per tree build and reused across
  the phase, so this cost does not recur per phase-1 call. Do not read
  0.80 s as the staging overhead; 158 ms is, and that is ~9% of the
  1.71 s of phaseA+phaseB it is meant to replace at this scale.

### 12.2 Charm rebuilt (production + tracing + local reconverse)

    ./build charm++ reconverse-linux-x86_64 amd \
      --with-production --enable-tracing \
      --with-fetch-reconverse-dir=$HOME/charm_reconverse/reconverse \
      --force -j16

with `rocm/6.2.4` loaded (script: `fof/gpu/rebuild_charm_hip.sh`).
Result: `CMAKE_BUILD_TYPE=Release`, `TRACING=1`, `BUILD_HIP=1`,
`FETCHCONTENT_SOURCE_DIR_RECONVERSE` = the local checkout. In buildcmake
`--with-production` only sets the build type and `--enable-tracing` only
sets `-DTRACING`, so unlike the old `buildold` they compose — production
does not silently disable tracing.

**This retires trap 3.** `conv-mach-hip.sh` prefers `$ROCM_PATH` and only
falls back to `/opt/rocm-default` (6.4.2) when it is unset; with the
module loaded, charmc now emits 6.2.4 paths throughout and matches the
Kokkos build. `-Wl,--allow-shlib-undefined` has been removed from
tests/kokkos-spike and is no longer needed anywhere.

**Side effect worth knowing:** the top-level `charm_reconverse/include`
and `lib` symlinks now point at `reconverse-linux-x86_64-amd`, so a plain
`CHARM_HOME=$HOME/charm_reconverse` resolves to the HIP Release build.
That is convenient (one charm for both arms, and the production
`LD_LIBRARY_PATH=.../charm_reconverse/lib` keeps working) but it means
CPU-only builds now also link a HIP-enabled runtime.

Dependency rebuild (`fof/gpu/rebuild_deps.sh`, order per the top-level
README): prefixLib -> htram -> unionfind -> fof/gpu -> src -> fof ->
examples/fof3, with `AGGREGATION=`/`PROFILE=` per
design/frontier-labeling-ab.md.

### 12.3 Three more traps, from the rebuild and the FoF3 wiring

8. **A HIP-enabled charm makes EVERY client need
   `-D__HIP_PLATFORM_AMD__`.** `charm++.h` -> `ckrdmadevice.h` ->
   `conv-rdmadevice.h` -> `hapi_portable.h` pulls in
   `<hip/hip_runtime.h>`, which hard-`#error`s without a platform macro.
   This broke prefixLib — plain Charm code with no GPU content — the
   moment charm started pointing at 6.2.4's headers. It is a property of
   the charm build, not of the GPU arm, so `src/Makefile.common` detects
   it (`$(wildcard $(CHARM_HOME)/include/hapi.h)`) rather than tying it
   to `GPU=1`, and `rebuild_deps.sh` passes it to the siblings through
   their charmc override. Arguably charm should define it itself.
9. **A rebuilt archive does not relink the application.** `examples/fof3`
   named `-lfofdevice` in its link line but not `libfofdevice.a` among
   the FoF3 prerequisites, so after fixing the device library `make`
   reported everything up to date and ran a stale binary — costing one
   job and producing a confusing abort. Same class as the stale-`.a`
   trap design/frontier-labeling-ab.md records for libunionFind.a. Fixed
   by making the archive a prerequisite (`FOF_GPU_LIB`).
10. **The mapping invariant is "no two processes share a GPU", which is
    `<=`, not `==`.** The first check required
    `processes == visible devices` and so aborted a 2-process debug run
    on an 8-GCD node, where HAPI had in fact given the two processes
    devices 0 and 4 and nothing was shared. Small-scale debugging runs
    are exactly where this check gets exercised most, so getting the
    comparison right matters more than it looks.

### 12.4 Where the code is

- `fof/gpu/{FoFDevice.h,FoFDevice.cpp,standalone.cpp,Makefile}` — the
  device library and its Charm-free gate.
- `fof/gpu/{rebuild_charm_hip.sh,rebuild_deps.sh}` — the two rebuilds.
- `fof/gpu/{run_stage0.sbatch,run_stage0_fof3.sbatch}` — the gates.
- `tests/kokkos-spike/` — the Charm/HAPI/Kokkos integration gate.
- `fof/FoFPhase1.h` — the `#ifdef FOF_GPU` staging chain (deviceStage0 ->
  deviceInitOnHome -> devicePack -> deviceLaunchOnHome ->
  deviceCompleteOnHome -> deviceVerify) plus CPU-only stubs, and the
  `FOF_GPU_STAGE0` hook in `runFoFPhase1`.
- `src/Makefile.common` — the `GPU=1` arm; off by default, and with it
  unset the compile and link lines are unchanged apart from the
  auto-detected HIP platform macro.

Next: stage 1 (flat `DNode` emitted from `TreePiece::recursiveBuild`,
uploaded once per tree build), which is additive on the CPU side and
gated on "CPU results unchanged".

---

## 13. Stage 1: the flat device tree (2026-08-12)

Jobs 5255153 and 5255194, one Frontier node, 8 processes. Code:
`src/DeviceTree.h` (the `DNode` type, `flattenTree`, `verifyFlatTree`),
`TreePiece::buildTree` (the emit), `fof/FoFPhase1.h`
(`devicePlanTree`/`deviceFinishTree` + the per-PE copy), and the
`resizeTree`/`hostNodes`/`uploadTree` half of `fof/gpu/FoFDevice`.
Gate script: `fof/gpu/run_stage1.sbatch`.

### 13.1 Gates

| arm | components |
|---|---|
| 100k, tree off | 33,933 (`TEST PASSED`, full O(n^2)) |
| 100k, tree + host verify | 33,933 (`TEST PASSED`) |
| 100k, tree + verify + upload | 33,933 (`TEST PASSED`) |
| 80M, tree off | 23,707,197 |
| 80M, tree on | 23,707,197 |
| 80M, tree + host verify | 23,707,197 |
| 80M, tree + upload | 23,707,197 |

The emit is additive and nothing on the CPU path reads it, so identical
components across all seven arms is the "CPU results unchanged" gate.
Three independent structural checks back it:

- **Host, at build**: `verifyFlatTree` walks the flat array and the live
  tree together and compares boxes, child counts, leaf `part_begin` and
  the root's particle total (`PARATREET_DEVICE_TREE_VERIFY=1`, ~22 ms at
  80M — cheap enough to leave on in regressions).
- **Device, after upload**: a `parallel_reduce` over every node checking
  that it holds particles, that its children live inside the array and
  *after* it, that child `n_below` sums to the parent's, and that the box
  is non-inverted. 0 bad nodes.
- **Device, coverage**: particles reachable from the piece roots, summed
  on the device, must equal the staged particle count. 10,023,529 /
  10,023,529 exactly — this is what catches a bad index rebase, which is
  otherwise invisible until a traversal reads the wrong particles.

### 13.2 Cost, and a correction to section 3.2

At 80M, per process: 10.0M particles, **3,216,269 nodes over 539
TreePieces, 122.7 MB**. (Section 5 estimated ~3M nodes / ~130 MB at
15.6M particles; the node count per particle is higher than assumed.)

| step | cost |
|---|---|
| tree build, flat tree OFF | 561.5 ms |
| tree build, flat tree ON | 644.7 ms (**+83 ms, +15%**) |
| tree build, ON + host verify | 666.2 ms (+22 ms more) |
| concat + rebase, max PE (parallel) | 23.9 ms |
| device copy of 122.7 MB | 8.1 ms |
| device structural check | 4.0 ms |

**The flat tree is not free.** The first draft of this plan claimed the
emit "rides an existing walk" and so costs nothing; it measured +83 ms on
a 561 ms tree build. The reason is structural: children must be
CONTIGUOUS for the device traversal to address them by an add, and
`recursiveBuild`'s depth-first order does not produce that, so the emit
is a separate breadth-first pass that re-walks the live tree —
26 ns/node, which is what pointer-chasing heap-scattered nodes through a
virtual `getChild` costs. Making it genuinely free means emitting from
inside `recursiveBuild` with reserve-then-recurse (each node reserving a
contiguous block for its children before descending). Worth doing, but
it touches core tree build and belongs in its own change with its own
gate, not smuggled into stage 2.

What survives of the original claim is the part that matters for the
comparison: this cost is at TREE BUILD, not in the phase-1 wall, and it
is paid once per build no matter how many phase-1 calls follow.

### 13.3 The serial concatenation, found and fixed

First measurement had concat + upload at **248 ms, of which 236 ms was
the host-side concatenation** — one PE memcpy'ing 123 MB and rebasing
3.2M `child_begin` links while thirteen other PEs idled. Fixed by
splitting it the same way the particle pack is split: the home PE
computes only the LAYOUT (per-piece node offsets, piece roots, piece
particle bases) into pinned buffers, and each PE then copies and rebases
its OWN pieces in the same round as its particle pack. **236 ms -> 23.9
ms.** The layout plan needs no extra bookkeeping because
`FoFPhase1Node::pe_treepieces` holds the pieces in the same registration
order each PE's own `treepieces` vector does.

Worth stating as a pattern, since stage 2 will face it repeatedly: any
per-process device preparation step that touches O(particles) or
O(nodes) data must be structured as per-PE slices with a home-PE
planner. One PE doing it is 14x too slow and the code looks perfectly
reasonable until measured.

### 13.4 Design notes worth keeping

- **`part_begin` stays PIECE-local** in the flat tree and is paired with a
  per-piece particle base in a separate array. The alternative — baking
  the process-flat offset into the nodes — would tie the tree to one
  particular particle layout and force a rewrite of all 3.2M nodes
  whenever the staging order changed. As it is, the tree is a pure
  function of the TreePiece and can be built once and reused.
- **Empty nodes are dropped entirely.** Every `DNode` has `n_below > 0`,
  so device traversal never tests for them. The pruning test must be
  `n_particles == 0`, never `<= 0`: internal nodes carry -1 by design
  (Node.h), and `<= 0` would drop every internal node and silently
  produce a depth-1 tree. This is the same rule `firstFlat` learned the
  hard way in FoFPhase1.h.
- **The `PerTreePieceAble` extension is additive**: a second
  `operator()` overload carrying `(const DNode*, int)` whose default
  implementation forwards to the existing 3-argument form, so every
  existing visitor compiles and behaves identically.

### 13.5 Two more traps

11. **The application Makefile did not list `FoFPhase1.h`.**
    `examples/fof3/Makefile` tracked `FoFData.h` and `FoFPhase3.h` as
    header dependencies of `Main.o`, but `Main.h` includes `FoFPhase1.h`
    too — so phase-1 changes did not rebuild `Main.o`, and the link kept
    a stale instantiation of the templated chare. Symptom: the new tree
    upload stage ran and printed nothing at all. Cost a job. Same family
    as trap 9; both are "the build system does not know what the code
    depends on", which in a header-only templated codebase is the
    default failure mode rather than an unusual one.
12. **Default member initializers make a struct a non-aggregate under
    C++11**, which is what charmc compiles at by default here. Adding
    `= nullptr` to `TreePieceRef`'s new fields broke the existing
    `TreePieceRef{root, parts, n}` brace initialization. Field
    assignment instead.

Next: stage 2 — the device union-find and the leaf-batched traversal
(K1/K2), which is where the flat tree stops being staged data and starts
being read by a kernel.

---

## 14. Stage 2: the device union-find and traversal (2026-08-12)

Jobs 5256843 / 5256925 / 5256946 / 5256985, one Frontier node, 8
processes x 14 PEs, lambb.00500 (80M). Code: the `runPhase1` half of
`fof/gpu/FoFDevice.cpp` (union-find, top tree, traversal, freeze) and
the launch/verify wiring in `fof/FoFPhase1.h`. Gate script:
`fof/gpu/run_stage2.sbatch`.

### 14.1 The gate: exact per-particle agreement

The device pass runs AFTER the CPU chain, deliberately. By then every
particle's `group_number` holds its PROCESS-level tip — the global order
of the minimum-order member of its process-level component — which is
exactly what a process-wide device union-find computes. Both use
union-by-minimum-global-order, a total order, so both answers are
order-independent and must agree **particle by particle**, not merely in
component count.

| arm | result |
|---|---|
| 1k, 2 processes | 0 mismatches; 390 components (`TEST PASSED`, full O(n^2)) |
| 100k, 2 processes | 0 mismatches; 33,933 components (`TEST PASSED`, full O(n^2)) |
| 80M, 8 processes | 0 mismatches; 23,707,197 components |

This is a much stronger gate than the component histogram: a labeling
that got the partition right by luck but named a component differently
would fail it, and so would any single particle placed in the wrong
component anywhere on any process.

### 14.2 Result

Per process (10.0M particles), against the same run's CPU numbers:

| | time |
|---|---|
| CPU phaseA + phaseB + merge (max PE) | **1.207 s** |
| device walk (max process) | **516 ms** |
| device pass total: prepare 28 + walk 516 + freeze 0.3 + download 3.2 | 548 ms |
| + staging: particle pack 94, tree pack 27, tree upload 10, scatter 36 | ~731 ms |

**2.3x on the kernel against 14 CPU cores, ~1.65x end to end.** Real,
but well short of the 10-30x section 1 projected from ArborX's
37M/0.15 s on an A100 (which scales to roughly 80 ms for 10M on half an
A100 — we are ~6x off that). Section 14.4 is the diagnosis.

Note the baseline moved: the ledger's 1-node row
(design/fof3-lambb500-scaling.md) reads phaseA 1.563 s, but this build
measures 1.127 s for the same configuration. Comparisons here use the
number from the same job, not the ledger.

### 14.3 Two optimizations, one of which was wrong

**Wrong: the certificate thundering herd.** The first version's memo was
a plain check-then-set, so thousands of leaf threads could reach the same
hot node simultaneously, all read "unpublished", and all run the same
O(n_below) star-union. That is genuinely unbounded redundant work and it
looked like the obvious culprit. Fixing it — claim with a CAS (-1 -> -2
-> rep), losers unite with the node's first particle instead of waiting,
which is correct because the winner is committed to collapsing that node
and union-find is order-independent — **made no difference at all**
(walk went 531-766 ms to 578-848 ms, i.e. slightly worse, within the
noise of the extra atomic). The fix is kept because it is correct and
bounds a real worst case, but it was not the bottleneck. Reordering the
leaf list from atomic-compaction order to node order (a `parallel_scan`,
for locality and determinism) likewise did nothing measurable.

**Right: put the wavefront on the work.** The leaf-size sweep settled it,
and it runs the opposite way to intuition:

| max_particles_per_leaf | leaves/process | device walk (max) |
|---|---|---|
| 12 (default) | 2.7M | 850 ms |
| 32 | 1.2M | 2306 ms |
| 64 | 0.6M | 4075 ms |

Bigger leaves mean 4.5x fewer traversals and were still 4.8x SLOWER.
That can only happen if the per-thread leaf-pair product — leaf_size^2,
run serially by one lane — dominates, which also rules out traversal
node loads as the primary cost. So the kernel became **one team
(wavefront) per leaf instead of one thread**: every lane runs the same
traversal on a shared stack in team scratch (so node loads are one
broadcast per team rather than 64 scattered gathers), and the leaf-pair
cross product and the star-union are split across the 64 lanes over
consecutive particles (so they coalesce). Lane 0 owns push/pop and
broadcasts the popped entry, which is what keeps the shared stack
consistent. **850 ms -> 516 ms on the worst process, 1.6x**, with the
labels still bit-exact.

### 14.4 Where the remaining time goes

Certificate count tracks walk time across processes almost linearly:

| process | certificates | walk |
|---|---|---|
| 0 | 31.5M | 320 ms |
| 6 | 61.5M | 516 ms |

1.95x the certificates, 1.61x the time. The positive certificate is the
dominant cost, and it is concentrated in the dense processes — which is
precisely the regime stage 3's cell grid exists for (section 5, K3). The
CPU answers this with the same two mechanisms we have not ported yet:
connectivity suppression (a pair both-connected and already in the same
component prunes at ANY level — measured 4-7x on phaseA at 80M) and the
`-G` cell grid inside dense nodes. Expect stage 3 to move this number
much more than further kernel tuning would.

Second-order items visible in the same numbers: `prepare` doubled from
12 ms to 26 ms when the leaf compaction moved from an atomic counter to
a `parallel_scan` (kept anyway — determinism is worth 14 ms when chasing
a mismatch), and staging is now 166 ms, comparable to a third of the
walk, so the "write the device form at tree build" idea in section 12.1
stops being premature if the walk keeps dropping.

### 14.5 Design notes

- **The traversal starts at a top tree**, not at 539 piece roots: without
  it every leaf would test every piece (539 x 2.7M box tests). It is a
  median-split BVH over the piece root boxes, built on the host in
  microseconds, and traversal entries are tagged (`>= 0` a node index,
  `< 0` a top-tree index) so one stack serves both levels.
- **`runPhase1` is synchronous**, with a fence between kernels so the
  per-kernel walls are real. The async form (enqueue + `hapiAddCallback`,
  no fence, no blocked scheduler) is proven in stage 0 and is a two-line
  change; measurement came first.
- **PBC is not implemented on the device.** The device distance test is
  plain Euclidean. Runs with `-P` must use the CPU arm until the cell
  wrap is written (section 10, open question 1).

### 14.6 One more trap

13. **A kernel entry point that silently reads stale device memory.**
    `runPhase1` did not upload positions and orders — that copy lived in
    `enqueueRoundTrip`, the stage-0 method it replaced — so every label
    came back 0. It failed loudly only because the gate compares every
    particle; a component-count check would have reported a plausible
    wrong number. Each device entry point now owns the uploads it reads,
    rather than relying on a sibling method having run first.

Next: stage 3 (the dense-node cell grid), which section 14.4 identifies
as the largest remaining lever, and then the section-4 contract work
(process-wide `rep_label` + deposit barriers) that lets the device
labeling actually replace the CPU chain rather than run beside it.
