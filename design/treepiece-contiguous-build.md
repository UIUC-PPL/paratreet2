# Per-TreePiece contiguous tree build (design note, 2026-08-14)

Kale's proposal; assessed and sized this morning. Status: DEFERRED (see section 34 item 2: the S3 motivation is undercut — DENSITY cut donor per-grant cost 5x with no end-to-end effect; surviving case = walk locality + zero-copy on-ramp, pending a locality probe). Original: DESIGN — 
implementation to start after Frontier's s3Shipment parallel rebuild
lands (stable A/B baseline).

## Motivation, with numbers

1. Donor-side flatten is the dominant remaining S3 donor cost:
   ~30 ms/grant post-POD-wire (62% of collection), running ON the
   straggler — proc 55 ships ~80 grants ~ 2.4 s of critical-path
   flatten, the same order as the whole residual gap to the 0.25 s
   floor (sections 31-33).
2. THE PREORDER-RANGE PROPERTY: with pieces stored contiguous in
   preorder, ANY subtree is a contiguous address range. flatten's
   pointer-chasing recursion becomes a linear pass; ultimately a
   memcpy-plus-fixup ("the buffer is the message" on-ramp). Walks
   (phaseA/B, ~90% of runtime) may also gain locality — speculative,
   measured by the A/B either way.
3. MICROBENCH (laptop M-series, 113 MB working set, faithful node
   shape: 216 B, vptr, 8 atomic children; design/flatten-bench.cpp, clang++ -O2):
     interleaved pointer-chase   47.2 ns/node   (today)
     contiguous, same code       13.0 ns/node   3.6x
     contiguous range sweep       6.7 ns/node   7.0x
   Production sanity: Frontier flatten ~105 ns/node (30 ms / ~285k
   elements) — same regime, so expect flatten 30 -> ~4-9 ms/grant.
   Contiguity ALONE gives 3.6x with zero flatten-code change.

## Verified assumption (load-bearing)

Pool units reference LOCALLY BUILT piece subtrees only:
buildPoolSlice pairs roots from pe_treepieces (this process's
registered pieces); cached/remote nodes are walk-read but never
flatten sources (corroborated: flatten reads particles(), which slim
cached leaves do not populate, and 1.15M forced-shipped units were
exact). So contiguity needs to cover ONLY the local piece build.

## Design

- Per-piece arena, CAPACITY-BOUNDED up front from n_particles and
  leaf size (nodes <= ceil(2n/leaf)+depth-slack; measure the real
  bound), bump-allocated, built single-threaded depth-first per piece
  (pieces are the parallel grain) => preorder for free.
- NEVER compact or move after build: registries, parked waiters, and
  PoolUnit hold raw Node*. Accept the slack (transient phase-1
  memory; windowed flush bought headroom).
- SCOPE: local piece builds only. The CacheManager's async remote
  arrivals stay in the existing per-lane FullNodePool. Two allocation
  paths, one flag: PARATREET_PIECE_ARENA=1 (default off until the 2B
  A/B).
- Flatten unchanged in step 1 (pointer-chase over contiguous memory,
  3.6x). Step 2 (optional): range-sweep flatten using the preorder
  range + positional parent/slot derivation (7x); only if step 1's
  measured win says the residual matters.
- Destructor path: arena-aware piece teardown (destroy in place, free
  arena) mirroring the S3 receive-side destroyStealArena pattern.

## Risks

- Node* escape before build completes: none expected (build is
  per-piece serial, pointers escape after), verify during
  implementation.
- Capacity bound wrong => overflow: assert + fallback to pool for the
  overflow tail (correct, loses contiguity for that piece, counted).
- Memory slack: bounded by (capacity - used) x sizeof(FullNode);
  report as a FOF3STAT line in the A/B.

## Gates and A/B

- Laptop both runtimes: full suite incl. loopback (wire unchanged —
  loopback here guards the BUILD refactor's tree correctness).
- 2B A/B, one allocation, flag off/on x2: phaseA wall (locality
  effect either way), s3_time flatten ms/grant (the sized claim),
  phaseB_s max, RSS (slack).

## UN-DEFERRED, and re-motivated (Kale, 2026-08-14)

Kale restated this as the third standing option: give the LOCALLY OWNED
TreePiece tree a contiguous allocation in PREORDER, built before phase 1
(explicitly NOT the CacheManager's remote-node storage, which keeps its
per-lane pool and its async arrivals).

The deferral in section 34 was correct on its own terms and wrong as a
verdict on the idea. What relay3 killed was the S3-FLATTEN motivation:
per-grant donor cost does not bind, so speeding the flatten buys nothing.
Three motivations survive, and two of them got STRONGER since:

1. **WALK LOCALITY — now the dominant remaining cost.** Section 34 places
   the residual straggler in phaseBChained on the hot process, i.e. its
   own local walk, untouched by every S3 change. The walk descends
   pointer-linked nodes allocated from a per-LANE pool, so sibling and
   parent-child nodes of one piece are interleaved with other pieces'
   nodes across the heap. Preorder-contiguous storage makes a descent a
   forward scan and makes any subtree a contiguous range.
   Microbench (design/flatten-bench.cpp, faithful 216 B node with vptr
   and 8 atomic children, 113 MB working set): the same pointer-chasing
   code runs 47.2 ns/node interleaved vs 13.0 ns/node contiguous — 3.6x
   from the ALLOCATION CHANGE ALONE, no code change to the traversal. A
   pure range sweep reaches 6.7 ns/node (7x) if the access is later
   rewritten to exploit contiguity.
2. **IT IS THE ENABLER FOR IDEA 1 (SIMD).** The distance-test loops are
   gather-bound because positions live 12 bytes at a time inside ~100 B
   AoS Particles. A contiguous per-piece build is the natural place to
   also emit a packed SoA position array (x[], y[], z[], 12 B/particle),
   after which the leaf-leaf loops are contiguous float loops the
   compiler can vectorise. Neither idea reaches its ceiling without this.
3. **The buffer-is-the-message endgame** (unchanged): once a subtree is a
   contiguous range of POD-ish nodes, shipping one approaches a memcpy.
   Lowest priority of the three now, but free once the layout exists.

### Scope, unchanged from the original design

Local piece builds only; CacheManager untouched. Capacity-bounded arena
per piece sized from n_particles, single-threaded depth-first build
(pieces are already the parallel grain, so preorder comes for free), and
NEVER compact or move afterwards — registries, parked waiters and
PoolUnit all hold raw Node*. Overflow falls back to the lane pool for
that piece's tail (correct, loses contiguity, counted). Flag
PARATREET_PIECE_ARENA, default off, 2B A/B reading phaseA wall, phaseB
wall and phaseB_s max — with the honest possibility that the walk is
latency-bound in a way contiguity does not fix, which the A/B settles
either way.

### Interaction with the other two ideas

All three are complementary and touch different code:
- contiguous+preorder layout: the tree BUILD (src/)
- SIMD: the distance-test LOOPS (fof/), enabled by the layout's packed
  positions
- targeted shedding: WHICH PROCESS owns a piece (fof/ + the pre-build
  migration window)
Sequence them so attribution stays clean: measure the compiler flags
first (running now, zero code), then the layout (it enables SIMD), then
SIMD, with shedding on its own track since it is orthogonal to all of it.

## MEASURED PREMISE CHECK (2026-08-14): the layout is ALREADY preorder —
## contiguous only in 27 KB runs, and the chunk size was never set

Before building a per-piece arena, I checked what the current allocator
actually produces. Two facts change the plan:

1. **`buildTree` is one entry method with no yields.** `recursiveBuild`
   is a plain recursion over `cm_local->makeNode`, so from the
   scheduler's view a piece's whole tree is built atomically: nothing
   from another piece, and nothing from the CacheManager, interleaves.
   `FullNodePool::alloc` bump-allocates. Therefore a piece's nodes are
   ALREADY laid out in creation order, which is depth-first PREORDER.
   The interleaved-allocation scenario my microbench modelled (47.2 vs
   13.0 ns/node) does NOT occur, so that 3.6x overstates the available
   gain — an honest correction to this note's earlier motivation.
2. **But contiguity stops at the chunk boundary, and the chunk is 128
   nodes.** Each pool chunk is its own `new char[sizeof(FullNode) *
   pool_elem_size]`, and `config.pool_elem_size` is never assigned by any
   app and is not a registered config field, so
   `std::max(config.pool_elem_size, 128)` has ALWAYS resolved to the 128
   floor — 27 KB per chunk (confirmed at runtime). A 29k-particle piece
   at 12 particles/leaf is ~5k nodes, i.e. ~40 separately-malloc'd
   blocks with arbitrary addresses between them.

So the real state is "preorder, contiguous in 27 KB runs, ~40 heap jumps
per piece" — much better than assumed, but not contiguous.

### Consequence: the first experiment is a knob, not an arena

`PARATREET_POOL_ELEM_SIZE` (added same day) sets the chunk size. Sized to
hold a whole piece it yields fully contiguous preorder local trees with
NO structural change — no arena, no capacity bound, no destructor path,
no fallback. Exactly parallel to the `-march` experiment: run the cheap
version first, and let it decide whether the expensive one is worth
building.
- If a large chunk moves phaseA/phaseB: contiguity matters, and the
  per-piece arena (which additionally guarantees exact bounds and enables
  the packed-position array) becomes worth its complexity.
- If it does not: the walk is latency-bound in a way layout cannot fix,
  and option 3 closes cheaply — with the packed-SoA-positions idea for
  SIMD still standing on its own, since that attacks the gather, not the
  node layout.
Cost of the knob: memory. A chunk is allocated whole, so oversized chunks
waste up to one chunk per lane per piece-tail; report RSS in the A/B.

## CLOSED — measured null (Frontier relay4, 2026-08-15)

`PARATREET_POOL_ELEM_SIZE` default/4096/16384/65536 at 2B/16 nodes:
phase1 spans 3.245-3.376 s while the two IDENTICAL default arms differ
by 1.8%, and the drift control is the largest phaseB outlier of the
five. No ordering by chunk size in phaseA, phaseB, Iter0 or RSS. Pool
bytes rise +0.4/+1.6/+6.1%, so the knob demonstrably took effect — it
simply bought nothing.

Full contiguity within a piece therefore produces no measurable gain,
and the per-piece ARENA cannot do better: it differs from the knob only
in removing the same chunk boundaries more precisely. THIS IDEA IS
CLOSED on measurement.

Worth keeping from it: the reason it was cheap to close. The knob was
~15 lines and one job; the arena would have been a capacity-bounded
allocator, a destructor path, an overflow fallback and a week of
gating, to reach the same answer. Also the premise check that preceded
it (buildTree never yields, so pieces were ALREADY preorder) is what
made the microbench's 3.6x recognisably an overstatement before anyone
built on it.

NOTE for the SIMD idea, which cited this note as its enabler: the
packed-SoA-position array does NOT depend on this. It attacks the
gather in the distance-test loops, not the node layout, and can be built
independently if part 1 is ever revived — though relay4's measurement
(phaseB unmoved by vectorisation) argues against that too.
