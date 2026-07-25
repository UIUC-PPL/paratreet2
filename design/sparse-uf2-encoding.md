# Enumeration-free tip encoding + lazy UF_2 vertex storage

**STATUS: DESIGN FOR DISCUSSION (with Ritvik — UnionFindLib API is his).
No code change yet.** Recorded 2026-07-24, from the Projections analysis
of the 80M P=8 trace (design/phase1-scaling.md has the numbers).

## The costs this removes

At 80M, 23.7M fragments exist and ~66k are ever touched by a merge edge
(edges_sent at P=16: 33k, two endpoints each). Everything below does work
proportional to 23.7M and appears in the trace as serial or per-process
sections:

1. countFragments (the blue-violet declining wedge, ~1.5 s at P=8): each
   PE counts its particles per tip into a private map, then the 15 per-PE
   maps merge SERIALLY under the process-wide FoFPhase1Node lock (~10M
   locked inserts per process).
2. computeTipEncoding (the ~6.7% plateau, ~0.7 s at P=8 — one busy PE per
   process): serial enumeration of ~3M fragments per process to assign
   dense indices and build the UF_2 vertex array.
3. UF_2 vertex array memory: all 23.7M vertices pre-allocated (~1-1.5 GB
   across processes) though ~66k participate.
4. boss_count_prefix_done / find_components / start_component_labeling:
   serial O(V_local) scans over ~1.5-3M vertices per process on one PE
   (the Projections hotspot examined 2026-07-24 and then deferred — it
   becomes free here, V drops to ~edge-touched counts).

## The key facts

- A tip is already globally unique: it is the min-order particle's global
  order (fits in 40 bits at any realistic N).
- Every particle of a fragment lives in ONE process (process-level tips),
  and each PE knows its own process id.
- Therefore the owner-decodable encoding is a pure per-particle
  operation: group_number = (my_process << 40) | group_number. No map, no
  merge, no enumeration. applyTipEncoding becomes a tight loop; the walk
  and getLocationFromID are unchanged (owner = vid >> 40 as today).
- The ONLY consumer of dense local ids is UnionFindLib's vertex ARRAY
  addressing (initialize_vertices + arrIdx in the boss-find cascade).
  The walk never looks tips up; particles carry them inline.

## Proposed shape: one id contract, two storage modes

Universal contract: UF_2 vertex ids are owner-decodable 64-bit values
((owner << kBits) | local_id); getLocationFromID decodes owner. Storage
is a per-client choice at init:

- DENSE mode (existing behavior, unchanged): local_id is an index into a
  pre-sized vertex array. Right for clients whose vertices are mostly
  touched — explicit-graph clients and old particle-level FoF. A client
  with contiguously assigned global vertex ids converts by arithmetic
  alone: local_id = global_index - my_first_index, owner from the
  partition boundaries (Kale's observation, 2026-07-24) — and its dense
  local ids keep serving as array indices, so it loses nothing.
- LAZY mode (new): vertices live in a hash map keyed by local_id, created
  on first touch (union_request / find_boss arrival). Right for
  fragment-level FoF: ~66k entries instead of 23.7M. Internally, arrIdx
  in the cascade messages becomes the local_id (already 64-bit-safe in
  the message structs); boss counting and component labeling iterate the
  map (O(touched)) instead of the array (O(V)).

Semantics note for LAZY mode: totalNumBosses counts only touched
vertices. fof3's authoritative component count already comes from the
label histogram; untouched fragments are their own components and are
countable by subtraction. The "Number of components found" print from
the library changes meaning for lazy-mode clients; document it.

## FoF-side changes (paratreet2, gated on the lazy mode existing)

- applyTipEncoding: the map-free OR-rewrite above.
- countFragments + computeTipEncoding: removed from the critical path.
  The FOF3STAT fragments: histogram line is the only surviving consumer —
  compute it off the timed path (or stats mode only), and distribute the
  merge if it ever matters (same treatment as the component histogram).
- Vertex SIZES: uf2 vertices currently carry fragment sizes from the
  counts. fof3's size outputs come from the label histogram, not uf2;
  lazy vertices can default size=1 or carry sizes only if a client
  supplies them. Confirm no other consumer with Ritvik.

## Variant recorded for later: process-local tip values (not now)

The tip value need not be global (Kale, 2026-07-25). The encoded id only
requires PROCESS-LOCAL distinctness (the prefix separates processes), so
the process-wide flat index of the min particle would serve in place of
its global order. Effects: the local field needs only
log2(particles-per-process) bits (~24-26 at any realistic scale) instead
of log2(N) — the id becomes scale-invariant in N and the 40-vs-44-bit
question disappears; smaller ids also shave hashing cost (mask the unused
bits) and communication (do not send them) — ultra-optimizations, noted
as negligible today. The price: tips and untouched-fragment labels become
decomposition-dependent (global orders are input-stable across runs and
process counts; component STRUCTURE stays deterministic either way, and
the step-4 label-agnostic checks compare structure, not labels).
Decision: keep global orders now (zero-change option); this variant is
the escape hatch if the bit budget or id-handling costs ever matter.

## Sizing rule (2026-07-25): kUF2IdxBits >= log2(N) under this design
(the local field holds a raw particle order). Default for the branch:
44/20 — 17.6e12 particles, 1,048,576 processes (HACC is at a trillion;
process counts will not approach a million). Single constexpr; add a
static assertion when the branch starts.

## Fallback if the lib change is declined

Sharded parallel encoding (paratreet2-only): hash-shard the tip space
across the process's PEs; each PE routes its counts to shard owners;
shards merge and enumerate concurrently; dense ids = shard prefix + local
offset. Removes the lock serialization and the one-PE plateau but keeps
dense ids, the 23.7M-vertex array, and the O(V) uf2 scans.

## Validation plan (when built)

Laptop matrix + 1M b0.2/b0.8 + LAMBS 1M grid-verified, 8M/16M stats
determinism vs current main; 32-proc reconverse run; then an Anvil sweep —
expected effect at P=8: the wedge and plateau vanish (~2.2 s), uf2 phase
shrinks (O(touched) scans), memory_MB drops by the vertex-array size.
