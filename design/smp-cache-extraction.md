# Extracting the SMP cache as a standalone library (the local-only cut)

**STATUS: DESIGN DIRECTION (Kale, 2026-07-28). Decision: Kale, with
discussion with Ritvik, and Thomas Quinn (UW) for the ChaNGa side.
(Joseph Hutter, the original ParaTreeT author, remains attached to the
work but is in medical school.) The interface shape in
Sec. 2 is Kale's; it supersedes an earlier framework-owner-interface
sketch because it removes messaging from the library entirely.**

## 1. Goal and the insight that scopes it

The lock-free SMP cache is paratreet2's (and old ParaTreeT's) most
broadly valuable piece: atomic child-pointer-exchange insertion,
per-node request deduplication, lock-free reads with phase-separated
mutation, pooled node storage. Today it is compiled into the paratreet
module and entangled with the module's other actors — Subtree answers
its requests, TreeCanopy restores boundary data, Resumer owns waiting
lists. No other application (ChaNGa in particular) can use it without
adopting the framework.

The scoping insight: **extract only the LOCAL piece.** How nodes are
REQUESTED — messages, entry methods, who owns what — remains entirely
outside. The library is a passive, process-shared, concurrent tree
cache: callers ask it for children and either get them or get a miss
(with the slot where fetched data must later be installed); the
enclosing framework does whatever communication it likes, and when a
response arrives it hands the cache a partial subtree to install
atomically. With that cut, the core has NO Charm dependency — it is a
C++ data structure over atomics — and any runtime or application (a
Charm framework, ChaNGa, an MPI+threads code) can drive it with its
own transport.

## 2. The interface (Kale's formulation)

Given a cache handle shared by the threads/PEs of one process:

- **`lookupChildren(Node* n) -> {children} | Miss{slot, first}`**
  For a node assumed present in the cache, return its children, or a
  miss carrying (a) the SLOT — the stable address/placeholder where
  the fetched subtree must later be installed (the referent the
  caller can also park continuations on), and (b) `first` — whether
  this caller is the first to miss this slot (the per-node atomic
  request bitmask lives INSIDE the cache; `first` is what tells the
  framework "issue the request" vs "someone already did — just wait").
  (A partial subtree = what a node request returns today: the node,
  its descendants to the configured share depth, and leaf particle
  payloads. Publication is the existing atomic child-pointer
  exchange; concurrent lookups see the placeholder or the complete
  subtree, never a partial state.)
- **`park(slot, opaque) -> PARKED | ALREADY_INSTALLED`**
  (Resolution of the continuation question, Kale 2026-07-28.) The
  per-slot waiting list lives INSIDE the library, on the placeholder —
  not because clients couldn't keep a slot->waiters map of their own,
  but because the park-vs-install race can only be closed against
  install's atomic publication, i.e., inside. `park` appends an OPAQUE
  reference (a small value the library never interprets) to the
  placeholder's lock-free list; ALREADY_INSTALLED means the data won
  the race and the caller proceeds synchronously — that return code is
  the lost-wakeup fix, provided once here instead of re-solved by
  every client.
- **`install(slot, PartialSubtree) -> {Node*, parked_opaques[]}`**
  Install RETURNS the parked continuations rather than invoking a
  callback: the cache stays passive (no execution context, no
  reentrancy, no runtime assumptions — ChaNGa and a Charm framework
  will want different resumption machinery). The guarantee: every
  parked opaque is handed back EXACTLY ONCE, by its own park
  (ALREADY_INSTALLED) or by install's returned list. Push-style
  callbacks are a five-line client-side layer over this pull
  primitive. Under this design, paratreet2's Resumer shrinks to pure
  policy — its waiting[key] map dissolves into the placeholders, and
  it keeps only "given these opaques, schedule these resumptions."
- **Transform hooks**: caller-supplied functions converting the
  incoming wire format into the cache's node/payload format (the seam
  that absorbs format differences — including ChaNGa's node layout —
  and where paratreet2's CachedParticle conversion already lives).
- **Auxiliaries**: root/topology installation (starter packs), bulk
  teardown between phases, statistics (the tally behind
  cached_particle_MB), and the phase-separation contract for mutation
  (upwardPass-style refresh), documented as caller obligations exactly
  as cache-concurrency.md states them today.

What stays OUTSIDE, by construction: request transport and routing,
retry/completion, the waiting-list/resumption machinery (Resumer),
canopy/boundary protocols, quiescence. The framework keeps all of it;
ChaNGa would keep its own equivalents.

## 3. Mapping from the current code

| current (CacheManager and friends) | becomes |
|---|---|
| `Node::requested` atomic fetch_or + placeholder types | inside: the miss/`first` logic |
| `makeCachedNode`, `insertNode`, `swapIn`, atomic `exchangeChild` | inside: `install` |
| node pools (`FullNodePool`), cached-particle storage (CachedParticle) | inside |
| `handleRemoteNode`'s "request or wait" decision | caller, driven by `Miss.first` |
| `serviceRequest` / `addCache` entries, `requestNodes` routing | caller (framework transport) |
| `Resumer` waiting[key] map | inside: per-placeholder parked lists |
| `Resumer` resumption scheduling (`process()`) | caller: consumes install's returned opaques |
| `recvStarterPack` / `restoreData` | thin caller wrappers over `install` |
| `resetCachedParticles`, `destroy` teardown | auxiliaries |
| cacheStats tally | auxiliary statistics |

The concurrency invariants transfer verbatim (cache-concurrency.md):
lock-free hot path, maps_lock only for registry inserts, mutation only
under phase separation.

## 4. Charm-independence and its one caveat

The core needs: atomics, per-thread rank for pool selection (a caller-
supplied thread id — today `CkMyRank()`), and nothing else from the
runtime. Ship it as a header library with a thin optional Charm shim
(the current CacheManager chare becomes that shim: transport +
Resumer + the library). The caveat: the SLOT lifetime contract — a
miss's slot must remain valid until install — is today guaranteed by
the placeholder-node discipline; the library must own placeholder
creation to keep that guarantee self-contained.

## 5. ChaNGa adoption path

VERIFIED against N-BodyShop/changa (2026-07-28): ChaNGa's
CacheInterface.h builds on charm's CkCache (`CkCacheEntryType`,
`CkCacheFillMsg`), and its tree nodes are GenericTreeNode. Two
consequences. First, CkCache is the ancestor of this design without
the lock-free SMP interior. Second — encouraging for adoption —
ChaNGa's `CkCacheEntryType` already carries a per-entry-type
`unpack(CkCacheFillMsg*, ...)` converter: structurally the SAME seam
as this library's transform hooks, so a binding slots where `unpack`
sits today instead of asking ChaNGa to learn a new idiom. Adoption = ChaNGa keeps its request/reply
protocol and treats this library as its process-level store: its
GenericTreeNode maps through the transform hooks (either into
Node<Data> with a ChaNGa-shaped Data, or — if impedance is too high —
the library additionally templates over the node type; decide during
extraction, not before). The realistic sequence: extract with
paratreet2 as the first client (behavior byte-identical, validated by
the existing suites), then prototype a ChaNGa binding as a separate
project with the ChaNGa maintainers.

## 6. Implementation plan (2026-08-04, branch smp-cache; started on
## Kale's go after the single-distribution merge)

Each phase ends with the full regression (fof3 matrix + 1M baselines +
gravity suite + annotate/searchAlgos, classic AND reconverse) and the
outputs byte-identical / within recorded bands — no phase merges
without it.

- **Phase 1 — carve the passive core in place.** New `src/TreeCache.h`:
  a plain class owning what is today CacheManager's passive state —
  node pools, root, registries (local_tps/leaf_lookup + the narrow
  maps_lock), placeholder creation, atomic install (today's
  makeCachedNode/insertNode/swapIn/exchangeChild), the requested
  bitmask logic behind Miss.first, teardown, and the cacheStats tally.
  CacheManager keeps every entry method and all messaging, delegating
  to the core. Pure code motion plus an interface; behavior identical.
- **Phase 2 — waiting lists move inside.** park/install per the
  resolution in section 2: per-slot parked lists on the placeholders,
  install returns the opaques, ALREADY_INSTALLED closes the
  park-vs-install race. Resumer::waiting dissolves; Resumer keeps only
  scheduling policy (which goDown to send, subtree vs partition
  routing). handleRemoteNode rewrites onto lookupChildren/park. The
  fanout-fix semantics (notify exactly the requesting lanes) become
  install's returned-opaque list.
- **Phase 3 — Charm-independence audit.** No Charm types or calls
  inside TreeCache.h: assert hook instead of CkAbort, caller-supplied
  lane id instead of CkMyRank, transform hook instead of the MultiData
  knowledge. Gate: a standalone C++ unit test (plain pthreads driving
  concurrent lookup/park/install against a scripted fill order) that
  compiles with no Charm headers on the include path.
- **Phase 4 — documentation.** Library header carries the concurrency
  contract (from cache-concurrency.md); mapping table below updated to
  point at the new seams; ChaNGa binding remains a separate future
  project with the maintainers.

## 7. Relation to the toolkit-boundary program

This is the complementary half of the standing structural item: FoF
moves OUT of the core (fof/ module), and the cache moves BELOW it
(standalone library). End state: a small paratreet2 core (trees,
decomposition, traversal drivers) over a reusable cache library, with
applications as modules — the disaggregation of old ParaTreeT
completed rather than merely restarted.
