# The paratreet2 distributed Friends-of-Friends algorithm: a complete description

**Internal technical report, 2026-08-01. Describes the algorithm as
implemented on main (commit b84ddaf), with code anchors and the measured
phase costs at the largest validated scale (1.98B particles, 16 Anvil
nodes, 128 processes x 15 PEs, reconverse runtime). Companion documents:
design/phase1-campaign-report.md (how the optimizations were found, with
failed paths), design/walk-uf2-overlap.md and design/phase1-scaling.md
(per-optimization measurement ledgers), design/sparse-uf2-encoding.md,
design/cached-particle-slimming.md (mechanism details).**

## 1. Problem statement

Friends-of-Friends (FoF) clustering: given N particles and a linking
length b, two particles are friends if their distance is at most b; the
FoF groups are the connected components of the friendship graph — the
transitive closure of the within-b relation. In cosmological practice b
is specified as a factor of the mean interparticle separation
(b = factor x (V/N)^(1/3); production factor 0.2), datasets are
Tipsy-format snapshots (int32 header: N < 2^31 by format), and the
output of record is the multiset of component sizes: the count, the
maximum, and a log2 size histogram, plus optionally a minimum-size
reporting filter (-m). Periodic boundary conditions are supported as
minimum-image with a cubic period L (-P; requires b < L/2).

The exact answer is required. Every optimization below is gated on
byte-identical component output against independent references
(Section 10).

## 2. Design overview: two levels plus a distributed closure

The algorithm exploits the machine hierarchy in three stages:

1. **Phase 1 — exact FoF restricted to each process.** Every process
   computes the connected components of the friendship graph induced on
   its own particles, with no communication beyond its own PEs. The
   result is a set of process-local components called FRAGMENTS, each
   named by a TIP.
2. **Phase 3 — the cross-process merge graph.** A pruned dual-tree walk
   discovers, for every pair of fragments on different processes that
   contain at least one cross-process friend pair, one merge EDGE. (The
   phase numbering is historical; there is no phase 2.)
3. **Distributed union-find (UF_2) — the closure.** A distributed
   union-find over tips, fed by the merge edges (streamed in during the
   walk), computes the global components; a final owner-writes relabel
   stamps every particle with its global component label.

The correctness cornerstone that makes stage 2 cheap is proved from
stage 1's exactness: after phase 1, if two particles within b of each
other carry DIFFERENT tips, they necessarily live on different
processes (had they shared a process, phase 1 — a complete FoF on that
process — would have unioned them). Therefore the walk needs no
ownership tests at all: "different tips and within b" is exactly the
merge-edge predicate (FoFPhase3.h header comment).

Everything else in this report is the machinery that makes the three
stages fast: certificates and suppression that prune the quadratic
pair work, a work-pool that levels phase 1's cross-PE stage, a
lock-free software cache that ships remote tree regions for the walk,
and a sparse encoding that lets UF_2 run without ever enumerating the
(hundreds of millions of) fragments.

## 3. Substrate: decomposition, trees, and the two chare arrays

paratreet2 is a particle-tree framework; FoF is an application on it
(examples/fof3 + src/FoFPhase1.h, FoFPhase3.h, FoFData.h — the
long-term plan moves these into their own module).

- **Input and decomposition.** Readers ingest the Tipsy file;
  decomposition computes splitters (oct decomposition is the FoF
  configuration) and assigns particles to SUBTREE chares (default
  8 x PEs chares, so ~N/(8 x PEs) particles per chare). Each TreePiece
  builds its local octree; TREE CANOPIES knit the TreePiece roots into a
  conceptual global tree. A second chare array, PARTITIONS, exists for
  traversal work; in the FoF configuration the two decompositions
  MATCH, and Partition leaves alias the TreePiece leaves by pointer
  identity (verifySharedLeaves enforces it; at 2B the framework
  reports zero particle copies from this duality). Design direction
  (design/single-distribution-mode.md): make Partitions optional.
- **Payload.** The per-node application data is FragData: a bounding
  box, particle count, and the FoF annotations min_frag/max_frag
  (Section 6). Particles carry `order` (global id, 64-bit) and
  `group_number` (the mutable label field that holds, successively:
  -1, a phase-1 tip, an encoded tip, and finally a global label).

## 4. Phase 1: exact FoF within each process

Implemented by a per-PE group FoFPhase1 and a per-process nodegroup
FoFPhase1Node (src/FoFPhase1.h). TreePieces register their live tree
roots and particle blocks with their PE's branch
(TreePiece::registerFoF -> registerTreePiece). The whole phase then runs
as a WITHIN-PROCESS CHAIN: each stage is triggered by the last PE (or
last depositor) of the process finishing the previous stage, tracked
by atomic deposit counters on the nodegroup — one deposit per PE, no
global barriers between stages; a single final reduction returns
max-reduced stage walls to the driver.

### 4.1 phaseA: per-PE union-find

Each PE owns the particles of its registered TreePieces, indexed by a
flat offset space. It runs a sequential union-find (path-compressed,
frozen at the end of the stage) over all pairs of its TreePieces,
including self-pairs, where each pair is processed by a dual-tree walk
over the two TreePiece octrees. OrderING: all self-pairs first, then
cross-pairs (merge-early: local assembly populates the connectivity
memo below, so cross walks see maximal suppression).

The walk prunes with three mechanisms (design/phase1-scaling.md):

- **Negative certificate:** if mindist(box_a, box_b) > b, no pair in
  the product can be friends — prune. (PBC-aware: minimum-image
  distance when -P is set.)
- **Positive certificate:** if maxdist(box_a, box_b) <= b, EVERY pair
  is friends — union the two nodes wholesale without descending. This
  is memoized per node (certRep): a node's first positive certificate
  makes it a fragment via one O(n) star-union through a representative;
  every later certificate involving it is O(1).
- **Connectivity suppression** (the largest phaseA win at scale, ~4-7x
  at 80M): a monotone memo per node answering "is this node internally
  connected, and to which current component representative?"
  (connectedRep — leaves check directly with first-mismatch exit;
  internal nodes consult only their children's memos, so connectivity
  percolates lazily). A pair of nodes both connected and already in
  the same component cannot contribute — prune at any level, the
  phase-1 analog of phase 3's SEEN table. Positive memos only:
  negative memoization was measured counterproductive both ways.

**Per-chare grid option (-G t):** a chare whose root-box occupancy
n c^3 / V exceeds t (with cell size c = b / sqrt(6)) solves its SELF
pair by a cell grid in ~O(n) instead of the tree walk: particles in
the same cell are friends by construction (cell diagonal <= b), and
face-adjacent cells union for free; only a bounded residual stencil
needs distance tests. Cross-pairs always use the walk. Measured: -29%
phaseA max at 2B (skew compressor, not average-work reducer); the
win regime begins around ~15k particles per chare.

phaseA ends by FREEZING: full path compression, then every particle's
group_number is stamped with its fragment's TIP = the global `order`
of the component's minimum-order member. Tips are thus globally unique
without communication, and stable names (used by the verification
harness to compare labelings across configurations).

Instrumentation: per-PE phaseA wall, and a geometric predicted-work
proxy X = sum over the PE's chares of n^2/V, reduced with its Pearson
correlation against the measured walls (FOF3STAT density line). A
finding worth keeping: r ~ 0.9 at laptop scales but ~0.27 at 480 PEs —
the certificates and suppression absorb most of the naively predicted
work, so a placement scheme keyed on raw n^2/V would over-correct.

### 4.2 phaseB: cross-PE pairs through a process-wide pool

Pairs of TreePieces on DIFFERENT PEs of the same process are walked over
the frozen phaseA state; witnesses emit (tip, tip) EDGES (deduplicated
per PE by an exact two-uint64 pair key) rather than performing unions,
because phaseA state is frozen — this is the frozen-phase discipline
that keeps phaseA lock- and atomic-free.

Work division is a process-wide POOL on the nodegroup (shared memory,
not messaging): at the phaseA->phaseB transition the last depositor
builds a vector of pair units, geometry-gated (only pairs with box
mindist <= b enter, PBC-aware), with dense pairs split one level to
their child products when the boxes overlap (gap-gated depth-2), and
LPT-ordered (overlapping pairs by descending overlap volume first,
then by ascending gap). Every PE then claims units by a single atomic
fetch-add (chunk 1 — the pool is LPT-sorted, so chunked claims would
stack the costliest units on one PE). The per-unit maximum wall is
recorded as a divisibility diagnostic: maxpair ~= the phaseB wall
means an indivisible unit remains; maxpair << wall (the observed 2B
regime, 0.58 vs 2.8 s) means the residual is the CROSS-PROCESS work
volume — one process carries ~26x the average process's pair work,
leveled perfectly (within 1%) across its own PEs by the pool. Moving
work across process boundaries (larger ppn, or density-aware
placement) is the only lever left there.

### 4.3 Merge and relabel

The last phaseB depositor runs, inline, a per-process union-find over
the deposited edge buffers (unioning tips), producing a tip -> tip
merge map; each PE then rewrites its particles' group_number through
the map (identity if absent). The result: every particle carries the
tip of its PROCESS-level fragment. Stage walls at 2B: reset 0.04,
register 0.03, phaseA 2.7-2.9 (max PE), phaseB 2.7-2.8 (max PE, the
cross-process floor), merge 0.01, relabel 0.1-0.2 s.

## 5. Tip encoding (sparse-uf2)

Before anything is shipped off-process, every tip is rewritten to the
owner-decodable value `(owning_process << 43) | tip` (43 index bits |
20 process bits, static-asserted to fit 63 bits; FoFPhase1.h). Two
properties carry the rest of the pipeline:

- **O(1) location decoding, no directory.** UF_2 routes every message
  by the pure function vid -> (chare, local id) = (vid >> 43,
  vid & mask), registered with the library (no lookup structure, no
  enumeration pass).
- **Enumeration-free.** The local id is the tip itself (a particle
  order), NOT a dense index — so no per-fragment counting, numbering,
  or vertex-array construction ever happens. This removed a measured
  ~2.2 s of per-process serial enumeration (countFragments /
  computeTipEncoding, now off the critical path; the fragments
  histogram survives only as the optional -g diagnostic).

The rewrite is per-particle, in place, BEFORE upwardPass/loadCache —
so every copy the walk later reads, including cache-shipped remote
copies, already carries the encoded, owner-decodable value (an
ordering hazard class this codebase has hit twice; the visitor
carries a CkEnforce tripwire).

## 6. Annotation and the software cache

**upwardPass** recomputes each TreePiece's per-node FragData bottom-up:
min_frag/max_frag = the min/max (encoded) tip over the node's
particles. A node with min_frag == max_frag is UNIFORM: all its
particles belong to one fragment — the property phase 3's certificates
and suppression key on. Canopies propagate the annotations up the
shared levels.

**loadCache** then ships a starter pack (the top shared levels) to
every process's CacheManager — a per-process nodegroup implementing a
lock-free software cache of the global tree: atomic child-pointer
exchange for concurrent installs, a per-node atomic `requested`
bitmask for request deduplication (one fetch per node per process, no
matter how many PEs miss on it), and pooled node storage. Mutation
happens only under phase separation (quiescence between build,
annotation, and traversal phases) — there are deliberately no data-path
locks (design/cache-concurrency.md). On a miss during the walk, the
requesting PE parks its traversal continuation with the per-PE
Resumer; when the fetched subtree (nodes to a configurable SHARE DEPTH
plus leaf particle payloads) is installed, the displaced placeholder's
waiter bitmask is handed to the installed node and exactly the waiting
PEs are notified (the resumption fanout — a measured 15x message
reduction over the previous broadcast; the waiter-bitmask handoff in
swapIn closes the park-vs-install race, with late parkers covered by a
substituted-placeholder re-check).

Two footprint controls matter at 2B:

- **Slim cached particles** (design/cached-particle-slimming.md): an
  application may declare `Data::CachedParticle`; FoF's carries only
  position + group_number (24 B vs ~112 B). Cache-shipped copies are
  stored (and wired) in this type end-to-end. At 2B: 216.5M cached
  copies = 5.2 GB machine-wide instead of ~24 GB. Non-opting
  applications are bit-identical (SFINAE detection; the alias trick in
  SpatialNode keeps every existing visitor compiling unchanged).
- **Share depth** (default 3, runtime -D): depth+1 halves the request
  count but ships ~8x more internal structure per reply. Measured
  sign flip: -12% walk at 80M, 2x SLOWER at 2B (+34% installed nodes
  for the same particle payload) — density decides, default stays 3.

## 7. Phase 3: the cross-process merge-edge walk

Driver: runFoFPhase3Dist (FoFPhase3.h), called from the app's
traversalFn on the threaded driver. Sequence: verifySharedLeaves ->
resetPhase3 -> create the UF_2 library and register the locator
(BEFORE the walk) -> arm edge streaming -> launch the walk -> CkWaitQD
-> statistics -> inject the edge remainder -> quiesce -> components.

**The walk itself** is a symmetric DUAL-TREE traversal
(TreePiece::startDual -> DualTraverser): every TreePiece walks its live
local tree (the target side, whose internal nodes upwardPass annotated
in place) against the global tree through the cache (the source side).
The historical alternative — the TRANSPOSED walk (every Partition's
flat leaf list against the global tree) — is kept permanently as the
A/B oracle (-w transposed); dual won 1.6-20x at 80M because the
transposed recursion sweeps its whole flat leaf vector per opened node.

FoFEdgeVisitor implements the pruning state machine
(design/step3.md):

- **Case 1 (negative):** source/target boxes farther than b — prune.
- **Case 2 (positive certificate):** both nodes UNIFORM (min_frag ==
  max_frag on each side), different fragments, and maxdist <= b —
  every particle pair is a friend pair, so emit the (g, f) edge
  WITHOUT descending.
- **Case 3 (SEEN suppression):** the process-level SEEN table on
  FoFPhase1Node already contains (g, f) — some walk on this process
  already witnessed this fragment pair; descending again can add
  nothing — prune at any level. First witness wins under the table's
  mutex; leaf-level witnesses insert-and-emit exactly once.
- **Same-fragment prune:** both sides uniform over the SAME tip — an
  intra-fragment pair cannot produce a merge edge.

Non-uniform pairs descend under an 8-way ALTERNATING SPLIT (only the
shallower/larger-box side splits per step, opt-in trait
SplitLargerOnly), children explored closest-box-first — so witnesses
land early and SEEN covers the sibling expansions (2.3x fewer leaf
tests than the joint 64-way product at 1M). Emitted edges are
(encoded tip, encoded tip) pairs, deduplicated per PE by the exact
128-bit pair key.

**Edge streaming (-E, default 4096):** each PE submits its edge buffer
to the process's UF_2 element whenever it reaches the batch size;
union-find is a semilattice, so unions are order-independent and can
race with the walk. The streamed cascades are plain entry sends and
ride the WALK's quiescence detection. Measured neutral at current
edge volumes (970k edges total at 2B) — kept for the bounded edge
buffer and because it removes injection from the post-walk critical
path.

Completion is RTS quiescence detection over the whole
walk+fetch+resume+union traffic. At 2B, ppn 15: walk 3.1-3.4 s.

## 8. Distributed union-find (UF_2)

The UnionFindLib (sibling repo, UIUC-PPL/unionfind; two student
generations, primarily Raghavendra; Ritvik Rao current steward) runs
ONE library chare per process, placed by a pre-created UFNodeMap
(created early because array-construction races its map broadcast on
runtimes without group-dependency buffering — a bug class found here
and fixed with message-level group dependencies). Configuration used:
LAZY vertex mode (sparse-uf2): a vertex record is created in a hash
map the first time a message touches its id — only ~2 x edges
vertices ever exist (~1.5M at 2B), against 425M fragments that are
never touched.

Protocol sketch: union_request(v1, v2) routes by the registered
locator; if both vertices are local to the receiving chare it performs
a sequential local union with path compression; otherwise an
asynchronous find_boss chain walks parent pointers (messages between
chares), and an anchoring step links the two bosses with
priority-ordered tie-breaking; path-compression messages shorten
chains opportunistically. All routing decisions go through
getLocationFromID; every locality test compares chare indices — a
property verified end-to-end (it is what would make k-chares-per-
process a ~30-line change; examined and parked, design/uf2-k-chares.md,
because measured lib-chare compute is only ~0.1 s/PE at 2B).

After quiescence, find_components counts BOSSES (component roots)
per chare, computes a global prefix over the chares (prefixLib
hypercube), numbers components serially within each chare's range,
and set_component broadcasts stamp every touched vertex's
componentNumber. The library's measured compute at 2B is tiny
(4.1 s CPU machine-wide); its wall (0.8-1.4 s) is the serial latency
structure of these rounds — quiescence settles plus sparse
cross-process message chains — a runtime-layer issue (the reconverse
sparse-message latency finding, charm-notes/reconverse-qd-latency.md),
not an algorithmic one.

## 9. Final labeling and component accounting

**Label collection:** each process's home PE copies the touched-vertex
labels (local id -> componentNumber) out of the library's hash into
the nodegroup. **applyUF2Labels** then rewrites every particle,
owner-writes, with the sign convention that keeps the label namespace
collision-free WITHOUT any global numbering of untouched fragments:

- tip PRESENT in the touched map -> group_number = -(componentNumber+2)
  (negative: a multi-process component's serial number);
- tip ABSENT -> the fragment was never referenced by any merge edge;
  it IS its own global component and keeps its (globally unique)
  encoded tip as its label.

**Component histogram** (the output of record) exploits the same sign
split in a distributed reduce-scatter (FOF3STAT components line,
0.6-0.9 s for 425M components at 2B): positive labels (process-local
fragments) are counted into local 64-bin log2 histograms and
sum-reduced; negative labels (touched components, only ~1M) are
concat-gathered by label hash, merged, and folded in. -m applies the
reporting filter (never a relabeling).

## 10. Verification harness and references

Every change gates through byte-identical component output:

- **fof1**: phase 1 alone against a serial O(n^2) FoF —
  the ONLY true phase-1 test (fof3's full check would mask phase-1
  under-merges because phase 3 repairs them). Single-process by design.
- **Grid-hash reference**: an O(n) exact FoF via cell hashing, used to
  FULL-verify references up to 16M laptop / 80M on Anvil (the 80M
  line 23,707,197 and the 2M line 666,737 are full-verified; the 2B
  line 424,897,832 max 185,317,566 is the cross-config determinism
  reference — identical across process counts, ppn, grid, depth,
  streaming, and traced/untraced runs).
- **-u serial**: the v1 gather-to-one UF_2 kept as the distributed
  path's oracle; **-w transposed**: the original walk kept as the dual
  walk's oracle. LAMBS real-data subsamples cross-check against the
  original ParaTreeT FoF implementation (0.03% agreement, explained by
  its default min-size pruning).
- Multi-PROCESS configurations are mandatory in every regression (a
  single process shares live objects and hides remote-copy bugs —
  learned the hard way, twice).

## 11. Measured phase costs at 2B (the reference profile)

cosmo25cmb.768g2_dm.001024 (1.98B particles), 16 Anvil nodes,
128 processes x 15 PEs, reconverse, untraced, current main:

| phase | seconds |
|---|---|
| decomposition (read + splitters + assignment) | ~8-9 |
| tree build | ~1.5 |
| phase 1 total | 4.6-5.0 |
| — phaseA (max PE) | 2.7-2.9 (2.0 with -G 4) |
| — phaseB (max PE; cross-process floor) | 2.7-2.8 |
| tip encode | 0.2 |
| upwardPass + canopy | 0.7-0.9 |
| loadCache | 0.05 |
| dual walk (incl. fetches, resumptions, streamed unions) | 3.1-3.4 |
| uf2 completion + find_components | 0.8-1.4 |
| relabel | 0.2-0.3 |
| component histogram | 0.6-0.9 |

Process RSS 10.6-12.6 GB (~92 GB/node of 257); cache: 86.5M installed
nodes, 216.5M slim cached particle copies (5.2 GB machine-wide).
End-to-end wall ~28-33 s including I/O and startup. The three known
levers on this profile, in current priority order: the runtime's
sparse-message latency (uf2 wall and every quiescence settle),
cross-process phaseB work placement (the 2.8 s floor), and phaseA
density skew (-G 4's -29% on the max, default decision pending).

## 12. Invariants the implementation depends on

1. **Frozen-phase discipline**: phaseA state is immutable during
   phaseB; phase-1 tips are immutable during phase 3. Cross-phase
   information flows only through stamped particle fields and
   annotations refreshed at phase boundaries.
2. **Phase separation over locks**: cached-tree mutation happens only
   in quiescence-separated windows; the cache hot path is lock-free.
3. **Every waiter is discoverable at install time**: a PE sets its
   requested bit BEFORE parking; install reads the mask AFTER
   publication; late parkers detect the substitution themselves.
4. **Tips are stable, globally unique names** (min-order member),
   which is what makes the harness's cross-configuration equality
   checks meaningful.
5. **Union streaming is safe because union-find is a semilattice**:
   unions commute and idempote; only find_components requires "all
   unions delivered", which quiescence provides.
6. **The merge-edge predicate needs no ownership test** (Section 2) —
   this is what keeps the walk's inner loop free of any process-id
   logic.
