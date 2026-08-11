# Walk unification (agenda item 1) with the GPU dimension

Deep-dive analysis, 2026-08-11 (agent audit against main 0fae562; the
full evidence trail with file:line for every claim is in the session
record — the load-bearing references are reproduced here). Companion to
design/walk-unification.md (the original plan, still valid, amended
below) and design/phasea-reassignment.md (item 4b, which touches the
same registries). GPU target: Frontier MI250X, HIP/ROCm; Ritvik
driving; phase 1 is the prime candidate. There is no GPU code anywhere
in the repo today; this is greenfield.

## 1. The two headline findings

1. **The dispatch boundary is already kernel-shaped; the data is not.**
   Traversers are templated over the visitor with no virtual dispatch
   anywhere in the hot path, traits are static constexpr, optional
   capabilities are SFINAE — a HIP kernel templated over a visitor
   type inlines the open/leaf criteria exactly as the CPU walk does.
   But: SpatialNode is polymorphic (virtual dtor = host vtable in the
   very type every visitor signature takes); Node::getChild is pure
   virtual over atomic child pointers; the certificate/connectivity
   memos are hash maps KEYED BY HOST NODE POINTER; and the phase-3
   visitor calls ckLocalBranch() + a mutex + a possible mid-leaf
   message send inside its hot calls (phase 3 is NOT portable as
   written). Phase 1's callbacks, by contrast, contain no Charm calls
   at all — the port target is phase 1, as assumed.
2. **gridSelfUnion is the most GPU-shaped algorithm already in the
   tree, and it needs no tree.** Bin, sort, test-free same-cell and
   face-adjacent unions, a ~160-offset residual stencil with
   first-witness exit. It was parked (default off) purely on a CPU
   THROUGHPUT objection — the stencil costs more than the certificate
   walk on a CPU — which is exactly the objection a GPU answers. It
   has a standing exactness oracle (grid-on vs grid-off byte-identical
   at 80M; fof1 phase-1-exact under -G 0.0001). And phaseA SELF pairs
   — the grid's domain — carry ~60 of ~67 phase-1 core-seconds.
   HAND THIS TO RITVIK AS KERNEL ONE: no dependencies on anything.

## 2. Inventory (what is generic vs FoF)

Generic, belongs in paratreet2 eventually: the local dual tree-pair
walk (fof's walk() vs the framework's DualTraverser — same split rule,
minus closest-first ordering); the phaseA "pairs among the trees this
PE owns" driver (NO framework analog — startDual routes even same-PE
pairs through the cache machinery; this is the item-1 counter-example
confirmed); the phaseB pool + claim cursor (new machinery, no analog);
the deposit-chain termination (the only quiescence-free intra-process
stage sequencer in the codebase); the box kernels mindist2/maxdist2
(DUPLICATED today between fof and Traverser.h — cheapest lift); the
freeze/materialize pass shapes. Also fold in: the transposed walk's
steal path uses a THIRD visitor entry shape (leafCollect with an
injected std::function) — unify rather than leave a fourth idiom.

Permanently FoF-side: the union-find itself, the fragment-certificate
and connectivity semantics, tip encoding, merge/relabel, SEEN/edge
machinery.

## 3. The visitor-contract gap

The framework has one non-descend outcome (open false -> node());
phase 1 needs two: PRUNE (resolved, no action) and ACCEPT (positive
certificate, whole-pair action, stop). Phase 3 only avoids this by
emitting inside open(). The unified contract needs exactly one
change: **open() returning {Descend, Accept, Prune}**, opt-in via a
static constexpr trait in the SplitLargerOnly idiom — existing
visitors bit-for-bit unchanged. Second gap: per-PAIR side context
(fof's TreePieceRef bases, needed to map particle pointers to flat
union-find indices) — extend the maybeSetKeys write-members-per-pair
idiom. Third: a PE-local driver should be a plain function call, not
an entry method — removes the visitor pup requirement entirely.

## 4. Staged order (serves toolkit AND GPU)

- **Stage 0 — device data contract. Do now; unblocks Ritvik; no
  framework behavior change.** POD DNode {box, child_begin,
  child_mask, part_begin, n_particles, depth, Data} in flat arrays
  with DENSE INTEGER NODE INDICES, built beside addNodeToFlatSubtree
  (which already walks pre-order; today it emits vtable-carrying
  SpatialNodes with no child offsets). Device particle = the app's
  CachedParticle (FoF's 20 B vs 120 B full Particle: 6x bandwidth on a
  bandwidth-bound kernel). Compile-gate it the tests/treecache way
  (hipcc, no Charm headers; note Charm-freedom is necessary but not
  sufficient — Node.h passes that gate and is still device-hostile).
  Side benefit on CPU: dense node indices let cert_rep/cert_tip become
  dense arrays instead of pointer-keyed hash maps.
- **Stage 1 — lift pool + deposit chain** (walk-unification.md step 1)
  with three amendments: (a) registry keyed by an OPAQUE CLIENT GROUP
  ID, not CkMyPe() — this is what makes 4b half 1 pure policy on top;
  (b) PoolUnit carries node indices, not pointers — the pool then IS
  the kernel launch descriptor; (c) cost key and split-stop rule are
  client hooks (FoF supplies today's key verbatim for the exact gate).
  Prerequisites: delete the FOF_POOL_SPLIT_SIZE remnants (agenda 4);
  land 4b half 2's density key on main FIRST (its gate passed) so the
  finished key crosses the interface once.
- **Stage 2 — contract change on CPU, driver unmoved.** Tri-state
  open + per-pair context, proven by repackaging fof's phase-1
  callbacks as a visitor while fof's walk() is still the driver —
  isolates contract from driver for honest bisection.
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

## 5. GPU-facing contract for Ritvik (minimal device visitor)

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
CAS attach is cycle-free BY CONSTRUCTION — stronger than the usual
rank-based GPU union-find), the grid solver. Freeze and relabel are
dense and data-parallel, and the phase-1 window touches only
process-local data (phasea-reassignment.md audit), so THE WHOLE PHASE
CAN STAY DEVICE-RESIDENT: upload positions+orders once after tree
build, download group_number once after relabel; only the small
per-process merge round-trips the host. Drop the SEEN set on device;
dedup by sort-unique — the merge is idempotent to duplicates.

## 6. Practical HIP notes

- MI250X wavefront = 64; max_particles_per_leaf defaults to 12, so a
  leaf-pair cross product is <= 144 and usually far fewer — too small
  per-pair-per-lane. Batch leaf pairs per wavefront or raise leaf size
  for GPU runs (Configuration knob exists). The single most likely
  source of disappointing first numbers.
- Divergence: the open criterion is ~15 branch-free flops; the
  divergent parts today are memo hash lookups (dense arrays fix),
  variable child counts (child mask bounds), and the grid's findOcc
  binary search (replace with dense cell index).
- Charm GPU integration: NONE on this stack — charm 8.0.0 HAPI is
  CUDA-only, reconverse has no GPU support at all ("device" there
  means LCI network device). Non-blocker: kernels launch from ordinary
  entry methods on HIP streams, and a stream-completion callback slots
  exactly where a_done.fetch_add sits in the deposit chain today.
- Process geometry: one process per GCD = 8/node, vs ppn 15 today.
  This moves BOTH of item 4b's measured baselines (within-process skew
  and pool sizes) — do not over-invest in 4b tuning before the GPU
  process geometry is settled, and expect to re-take those numbers.

## 7. Sequencing against item 4b

4b half 1 and stage 1 re-key the same two functions
(registerTreePiece's bucket, buildPoolSlice's iteration). They COMPOSE
if stage 1 introduces the opaque group id; they CONFLICT if lifted
with CkMyPe() hardcoded. Either order works; doing both
SIMULTANEOUSLY on those two functions is the thing to avoid — the
failure mode is the silent under-merge only fof1's phase-1-exact test
catches. 4b half 2's density key lands on main before stage 1 lifts
the key.
