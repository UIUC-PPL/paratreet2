# Walk unification: local traversals as framework drivers (+ the GPU dimension)

**STATUS: AGREED DIRECTION (Kale, 2026-08-05); GPU dimension analyzed
and merged in 2026-08-11 (agent audit against main 0fae562; this file
supersedes the short-lived separate walk-unification-gpu.md). No
implementation yet.** GPU context: a HIP/ROCm port is planned (Ritvik
driving; Frontier MI250X), phase 1 is the prime candidate, and the
planned division — paratreet2 owns traversals, applications own
visitors (open criteria + leaf calls) — makes the abstraction-boundary
question and the GPU-boundary question the same question. There is no
GPU code anywhere in the repo today.

## The principle (Kale)

paratreet2 exists to support tree walks. Local versus across-process is
a smaller variation than the commonality among walks. Therefore the
framework should offer ONE walk abstraction — the visitor contract:
certificates and actions in open()/leaf(), state carried by the visitor
through group-branch pointers, application fields through the Data
payload — crossed with two small axes:

1. WHERE the pair's data lives: chare-local (a tree against itself),
   PE-local (pairs among one PE's trees), process-local (pairs across
   the process's PEs), remote (the global tree through the cache). Only
   the remote scope engages fetch/park/install machinery at all.
2. HOW work is scheduled and terminated: chare-driven traversal closed
   by quiescence detection or by credit counting; pool-claimed unit
   scheduling closed by the process-local deposit chain.

## Evidence the abstraction already holds

- Mutation is not a barrier: gravity's leaf() mutates accelerations;
  the phase-3 FoFEdgeVisitor consults AND updates shared search state
  mid-walk inside the standard contract.
- Application node fields are not a barrier: everything FoF phase 1
  hangs on nodes went through the Data payload.
- Walk policies already live as visitor traits (SplitLargerOnly,
  TargetMustBeLeaf, SkipLocalSource, SkipMirroredPairs) with
  SFINAE-defaulted opt-in.
- The counter-example that proves the gap: FoF phase 1 re-implements
  descent mechanics (mindist pruning, split ordering, leaf-pair loops)
  in fof/FoFPhase1.h purely because local drivers do not exist in the
  framework. A second, smaller counter-example: the transposed walk's
  steal path uses a THIRD visitor entry shape (leafCollect with an
  injected std::function) — fold it into the unification rather than
  leave a fourth idiom.

## The two GPU headline findings

1. **The dispatch boundary is already kernel-shaped; the data is not.**
   Traversers are templated over the visitor with no virtual dispatch
   in the hot path; traits are static constexpr; optional capabilities
   are SFINAE. A HIP kernel templated over a visitor type inlines the
   open/leaf criteria exactly as the CPU walk does. What breaks device
   compilation is the data layer: SpatialNode is polymorphic (a host
   vtable in the very type every visitor signature takes);
   Node::getChild is pure virtual over atomic child pointers; the
   certificate/connectivity memos are hash maps KEYED BY HOST NODE
   POINTER; and the phase-3 visitor calls ckLocalBranch() + a mutex +
   a possible mid-leaf message send in its hot calls (phase 3 is NOT
   portable as written). Phase 1's callbacks contain no Charm calls at
   all — the port target is phase 1, as assumed.
2. **gridSelfUnion is the most GPU-shaped algorithm already in the
   tree, and it needs no tree.** Bin, sort, test-free same-cell and
   face-adjacent unions, a ~160-offset residual stencil with
   first-witness exit. Parked (default off) purely on a CPU THROUGHPUT
   objection — exactly the objection a GPU answers — with a standing
   byte-identical exactness oracle (80M grid-on/off; fof1 under
   -G 0.0001). phaseA SELF pairs — the grid's domain — carry ~60 of
   ~67 phase-1 core-seconds. HAND THIS TO RITVIK AS KERNEL ONE: no
   dependencies on anything below.

## Inventory: what moves, what stays

Moves into the framework (each with its closest existing analog):
- the local dual tree-pair walk (fof's walk() vs DualTraverser — same
  split rule minus closest-first ordering; fof's mindist/maxdist tests
  sit in the driver where phase 3 puts them in the visitor);
- the phaseA "pairs among the trees this PE owns" driver (NO framework
  analog — startDual routes even same-PE pairs through the cache);
- the process-local PAIR POOL: geometric enumeration (mindist-gated,
  split-leveled), LPT sort by a client-supplied cost key, atomic claim
  cursor. Unit = (node index, node index, key) — see the GPU stage
  order for why indices, not pointers;
- the deposit-chain termination harness (the only quiescence-free
  intra-process stage sequencer in the codebase);
- the box kernels mindist2/maxdist2 (duplicated today between fof and
  Traverser.h — the cheapest, least controversial lift);
- the freeze/materialize pass shapes (weakly generic).

Stays in applications, permanently: the union-find itself, the
fragment-certificate and connectivity-suppression semantics, tip
encoding, merge/relabel, the SEEN/edge machinery — as visitors and
payloads, exactly as SEEN works in phase 3 today.

## The visitor-contract gap

The framework has ONE non-descend outcome (open false -> node());
phase 1 needs TWO: PRUNE (pair fully resolved, no action) and ACCEPT
(positive certificate: whole-pair action, stop descending). Phase 3
only avoids this by emitting inside open(). Required change: **open()
returning {Descend, Accept, Prune}**, opt-in via a static constexpr
trait in the SplitLargerOnly idiom — existing visitors bit-for-bit
unchanged. Second gap: per-PAIR side context (fof's TreePieceRef
bases, mapping particle pointers to flat union-find indices) — extend
the maybeSetKeys write-members-before-each-call idiom. Third: a
PE-local driver is a plain function call, not an entry method — no pup
requirement on the visitor, raw pointers to per-PE state allowed.

## Staged order (serves toolkit AND GPU; supersedes the older 4-phase list)

- **Stage 0 — device data contract. Do now; unblocks Ritvik; no
  framework behavior change.** POD DNode {box, child_begin,
  child_mask, part_begin, n_particles, depth, Data} in flat arrays
  with DENSE INTEGER NODE INDICES, built beside addNodeToFlatSubtree
  (which already walks pre-order; today it emits vtable-carrying
  SpatialNodes with no child offsets). Device particle = the app's
  CachedParticle (FoF's is 20 B vs the 120 B full Particle: 6x
  bandwidth on a bandwidth-bound kernel). Compile-gate it the
  tests/treecache way (hipcc, no Charm headers; note Charm-freedom is
  necessary but not sufficient — Node.h passes that gate and is still
  device-hostile). Side benefit on CPU: dense node indices turn
  cert_rep/cert_tip into dense arrays instead of pointer-keyed maps —
  and make memos SHIPPABLE, which the hierarchical phaseB design
  (partition tasks stolen cross-process) also wants.
- **Stage 1 — lift pool + deposit chain**, with three amendments over
  the original phasing: (a) the registry is keyed by an OPAQUE CLIENT
  GROUP ID, not CkMyPe() — this makes balancing item 4b half 1
  (phaseA piece reassignment) pure policy on top, and should admit a
  HIERARCHY of group levels (Kale's hierarchical phaseB: partition
  groups inside the process — design note forthcoming); (b) PoolUnit
  carries node indices, not pointers — the pool then IS the kernel
  launch descriptor; (c) cost key and split-stop rule are client hooks
  (FoF supplies today's key verbatim for the exact gate). Prereqs:
  delete the FOF_POOL_SPLIT_SIZE remnants (agenda 4); land 4b half 2's
  density key on main FIRST (its gate passed) so the finished key
  crosses the interface once.
- **Stage 2 — contract change on CPU, driver unmoved.** Tri-state open
  + per-pair context, proven by repackaging fof's phase-1 callbacks as
  a visitor while fof's walk() is still the driver — isolates contract
  from driver for honest bisection.
- **Stage 3 — framework local drivers** (self-walk + PE-local pair
  walk, stack-based). fof's walk() deleted; FoFPhase1 shrinks to two
  visitors + union-find payload + label plumbing (~half of 2650
  lines). ANNOUNCE UP FRONT: the framework orders children
  closest-first and fof's walk does not, so descent order changes,
  witness order changes, and per-run counters (p1_conn_suppressed
  etc.) move — the gate is COMPONENT IDENTITY (counts + histograms),
  not counter identity.
- **Stage 4 — device pair-walk kernel, framework-owned**, consuming
  stage 1's pool and stage 0's tree, templated over the stage-2
  visitor concept.
- Later, on the same pool: cross-process offload/stealing (now part of
  the balancing program, item 4b — the hierarchical phaseB design).
- Second client (SPH-style local density pass) proves generality
  whenever convenient after stage 3.

## The GPU-facing contract for Ritvik (minimal device visitor)

    struct DeviceVisitor {                    // POD members only: no
      static constexpr bool CallSelfLeaf;     // proxies, host pointers,
      static constexpr bool TargetMustBeLeaf; // or std:: containers
      static constexpr bool SplitLargerOnly;
      __host__ __device__ Verdict open(const DNode& s, const DNode& t) const;
      __host__ __device__ void    node(const DNode& s, const DNode& t) const;
      __host__ __device__ void    leaf(const DNode& s, const DNode& t) const;
    };

Pool units (int, int, float). Explicitly out of scope in device code:
ckLocalBranch, mutexes, std::unordered_*, CkEnforce/CkPrintf/
CkWallTimer. FoF device pieces: the visitor (mindist prune + maxdist
certificate + connectivity against a dense node_rep array), the
union-find (union-by-min-GLOBAL-ORDER is a total order, so lock-free
CAS attach is cycle-free BY CONSTRUCTION — stronger than rank-based
GPU union-find), the grid solver. Freeze and relabel are dense and
data-parallel, and the phase-1 window touches only process-local data
(phasea-reassignment.md audit), so THE WHOLE PHASE CAN STAY
DEVICE-RESIDENT: upload positions+orders once after tree build,
download group_number once after relabel; only the small per-process
merge round-trips the host. Drop the SEEN set on device; dedup by
sort-unique — the merge is idempotent to duplicates.

## Practical HIP notes

- MI250X wavefront = 64; max_particles_per_leaf defaults to 12, so a
  leaf-pair cross product is <= 144 and usually far fewer — too small
  per-pair-per-lane. Batch leaf pairs per wavefront or raise leaf size
  for GPU runs (Configuration knob exists). The single most likely
  source of disappointing first numbers.
- Divergence: the open criterion is ~15 branch-free flops; the
  divergent parts today are memo hash lookups (dense arrays fix),
  variable child counts (child mask bounds), and the grid's findOcc
  binary search (replace with a dense cell index).
- Charm GPU integration: NONE on this stack — charm 8.0.0 HAPI is
  CUDA-only, reconverse has no GPU support at all ("device" there
  means LCI network device). Non-blocker: kernels launch from ordinary
  entry methods on HIP streams, and a stream-completion callback slots
  exactly where a_done.fetch_add sits in the deposit chain today.
- Process geometry: one process per GCD = 8/node, vs ppn 15 today.
  This moves BOTH of item 4b's measured baselines (within-process skew
  and pool sizes) — do not over-invest in 4b tuning before the GPU
  process geometry is settled, and expect to re-take those numbers.

## Sequencing against balancing item 4b

4b half 1 and stage 1 re-key the same two functions
(registerTreePiece's bucket, buildPoolSlice's iteration). They COMPOSE
if stage 1 introduces the opaque group id; they CONFLICT if lifted
with CkMyPe() hardcoded. Either order works; doing both SIMULTANEOUSLY
on those two functions is the thing to avoid — the failure mode is the
silent under-merge only fof1's phase-1-exact test catches. 4b half 2's
density key lands on main before stage 1 lifts the key. The
hierarchical phaseB design (partition-level groups, stealable
partition tasks) is a consumer of stage 1's group-id interface — its
requirements feed the interface before the lift.
