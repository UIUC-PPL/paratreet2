# Per-TreePiece contiguous tree build (design note, 2026-08-14)

Kale's proposal; assessed and sized this morning. Status: DESIGN — 
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
