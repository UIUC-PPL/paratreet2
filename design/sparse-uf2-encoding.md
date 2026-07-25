# Enumeration-free tip encoding + lazy UF_2 vertex storage

**STATUS: IMPLEMENTED ON BRANCHES (2026-07-25) — paratreet2
`sparse-uf2` + unionfind `lazy-vertices`. NOT merged: Ritvik's review of
the UnionFindLib API addition is still wanted (the library is his to
steward), and main is frozen while he tests the 64-bit-audit/slimming
commits.** Design recorded 2026-07-24, from the Projections analysis of
the 80M P=8 trace (design/phase1-scaling.md has the numbers).

Implementation notes (where they differ from or pin down the text
below): the split is 43/20, not 44/20 — one bit is reserved so encoded
tips never set the sign bit of the (long) group_number field, keeping
the -1 sentinel and the negative final labels -(component + 2) disjoint
from encoded tips (static_assert in FoFPhase1.h). The library gains
LAZY mode behind `unionFindInitOnePerNodeLazy(ready, node_map)` +
`registerMakeVertexID` (the inverse the library uses to reconstruct a
vertex's full id on first touch); dense mode is byte-for-byte unchanged.
Label readback is two-step (collectUF2Labels copies the touched-vertex
labels out of the library's hash storage on each process's home PE;
applyUF2Labels rewrites touched tips to negative labels,
identity-if-absent for untouched fragments). fof3 gains `-g` to opt the
fragments histogram (the only surviving countFragments consumer) back
in. Validated 2026-07-25 (laptop): 12-run small matrix 72/390/3549; 1M
b0.2 333,889 / b0.8 41,315 grid-verified; LAMBS 1M 379,884; 8M and 16M
stats histograms bit-identical dist-vs-serial (2,657,656 / 5,317,213);
100k at 32 processes full-check PASSED. The library's "Number of
components found" print now reports touched-only counts in lazy mode,
as anticipated below.

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

## Anvil validation run (instructions for Ritvik, 2026-07-25)

One run suffices: 80M (lambb.00500), b_factor 0.2, 8 processes x 15
PEs/proc (120 cores) — the same configuration as the 2026-07-24 sweep's
P=8 point and the Projections trace, so every number has a direct
baseline. Repeat the identical run once only if a number looks
surprising.

Branches (BOTH required, and unionfind needs a CLEAN rebuild — the
vertex-storage header layout changed, stale .o files will link garbage):

    cd unionfind && git fetch && git checkout lazy-vertices
    make clean && make            # PROFILE= as usual; AGGREGATION stays on
    cd ../paratreet2 && git fetch && git checkout sparse-uf2
    cd src && make clean && make
    cd ../examples/fof3 && make clean && make

Run: same command as the last sweep (no -w flag needed, dual is the
default; do NOT pass -g on the timed run).

Output changes that are EXPECTED, not bugs:
- The library line "Number of components found: N" now prints the
  touched-vertex count (tens of thousands), not ~24M. Lazy mode counts
  only fragments an edge reached; the authoritative count is the
  FOF3STAT components line, unchanged.
- No "FOF3STAT fragments:" line unless -g is passed (countFragments is
  off the timed path now). If you want the fragments histogram, do one
  extra run with -g.
- The "FOF3STAT time_s: phase1 ..." line has a new fragcount field
  (0.000 without -g).
- phase1_stages values are now process-local walls (stages of different
  processes overlap; their sum can exceed the phase1 total) — the four
  global barriers between phaseA/phaseB/merge/relabel are gone.
- -u serial (if you A/B) now needs an explicit -w transposed.

Readout against your 2026-07-24 P=8 numbers:
1. CORRECTNESS: FOF3STAT components line must match bit-for-bit.
2. tip_encode: was ~1.5s countFragments wedge + ~0.7s computeTipEncoding
   plateau at P=8; should drop to ~tens of ms.
3. upwardPass: laptop showed a large indirect drop (0.805 -> ~0.06s at
   8M, suspected heap-churn removal) — confirm or refute at 80M.
4. uf2: the O(V) per-process scans are gone; expect a visible drop at
   80M (they were the Projections hotspot).
5. memory_MB: should drop by roughly the vertex-array + counting-map
   size (laptop: -215 MB/proc at 8M).
6. balance phaseB_s min/avg/max: the lower-PE triangular rule is
   replaced by a fair subtree-pair hash; skew (was ~11x inside a
   process) should compress.

Rollback if anything fails: current main (= audit + slimming) is the
first fallback; the branch commits are separable (sparse UF_2 66d258d,
phaseB fairness 62fd898, barrier chain 041c67f) for bisection.

## Anvil result (2026-07-25, Ritvik): CONFIRMED

80M lambb.00500, b 0.2, 8 procs x 15 PEs (120 cores), branch build
(sparse-uf2 + lazy-vertices, includes fairness + chain commits).
Baselines: 07-23 phase-division table (step3.md 6h) and the 07-24
suppression sweep, both P=8.

| field | baseline P=8 | this run | change |
|---|---|---|---|
| tip_encode | 2.087 | 0.036 | 58x — wedge+plateau gone |
| upwardPass | 2.151 | 0.108 | 20x — the laptop's indirect drop holds at scale |
| uf2 | 0.170 (htram-off) | 0.043 | O(touched) scans; htram-on here |
| phase1 | 7.818 pre-suppr; phaseA 0.98 post-suppr | 1.087 (phaseA 0.911) | consistent + barrier latency gone |
| phaseB wall | 0.34-0.42 | 0.197 | fair division ~2x |
| walk (dual) | ~0.96 (2.795 transposed / 2.9x) | 0.718 | ~25%, plausibly slimming |
| relabel | 0.045 | 0.024 | |

Algorithmic total at P=8 is now ~3.2 s (phase1 1.09 + canopy/loadCache
1.20 + walk 0.72 + tips/upward/uf2/relabel ~0.2) vs ~8 s on the 07-24
state and ~15 s on 07-23. The design's predicted ~2.2 s recovery
undercounted: the indirect upwardPass effect roughly doubles it.

Correctness: components 23,707,197 max_size 1,519,203 (full histogram in
the log). REMAINING GATE: diff this components line bit-for-bit against
any P=8 line from the 07-24 sweep logs — the count matches the known
~23.7M total but the histogram comparison is the real check.
"Number of components found: 7029" is the expected touched-only print.

New observations from the run:
- FOF3STAT memory_MB prints 0.0 on Anvil/reconverse — CmiMemoryUsage
  returns 0 there. The vertex-array memory win cannot be verified from
  this line; the cache line works (pool_MB 8338 over 8 procs). Gap
  noted for the charm-convergence project.
- 15x "Allocating out of pool" from the CacheManager pools (~1 GB/proc
  pools exhausted). Harmless (falls back to heap) but pool sizing is
  now on the radar.
- component_histogram (harness stats, off the algorithm) grew to 5.2 s
  (23.7M label-count pairs concat-gathered to PE 0) — exclude from any
  timing readout, distribute if it ever matters.
- phaseB residual: max/avg 0.197/0.014 over 120 PEs — the fair division
  halved the wall but one density-hot PE remains; not worth further
  work at 0.2 s.
- End-to-end, input+decomposition (9.8 s: Tipsy read 3.1 + flush 5.9)
  now dominates the iteration. Within the algorithm the frontier is
  canopy loading (1.2 s), phaseA (0.91 s), walk (0.72 s) — the walk is
  where the asynchronous-control research questions (interleaving
  traversal with union-find) will surface at larger scale.
