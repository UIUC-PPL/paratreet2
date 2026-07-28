# Making Phase 1 Disappear: an Optimization Campaign on the paratreet2 Parallel Cluster Finder

### PPL internal technical report — July 18–27, 2026
### (the expanded, narrative companion to `opt-report-2026-07.md`)

## 0. Why this report exists

The FoF cluster finder on paratreet2 has a two-level design whose whole
point is the *distributed* phase: a tree walk over a distributed forest
that discovers connections between processes, and a distributed
union-find over the resulting merge graph. That is where the research
questions live — asynchronous control, interleaving traversal with
union-find, latency tolerance at scale. Phase 1, the within-process
part, was supposed to be the cheap preamble.

Measurements said otherwise. When the first full-scale runs came back
(80 million particles, the LAMBS cosmology snapshot, up to 16 processes
on Anvil), phase 1 and its associated bookkeeping consumed roughly 86%
of the algorithm's time, and the fraction was *growing* with process
count. Before the distributed phase could be studied on its merits, the
local phase had to be gotten out of the way. This report tells that
story — including the attempts that failed, because several of the
failures produced the understanding the successes were built on.

The punchline first. At the fixed benchmark configuration (80M
particles, 8 processes x 15 worker PEs = 120 cores):

| stage | July 23 | July 27 | factor |
|---|---|---|---|
| phase 1 (local FoF) | 7.82 s | ~1.0 s | ~8x |
| tip encoding | 2.09 s | 0.036 s | 58x |
| upward pass | 2.15 s | 0.108 s | 20x |
| phase-3 walk | 2.80 s | 0.72 s | 3.9x |
| distributed union-find | 0.17 s | 0.043 s | 4x |
| **algorithmic total** | **~15 s** | **~3.2 s** | **~4.7x** |

Every optimization below preserved a byte-identical output: 23,707,197
components with an identical size histogram, re-verified after every
change. That invariant — not any single speedup — is what made the
campaign's pace possible: each step could be validated in minutes.

## 1. The algorithm, and the vocabulary used throughout

Friends-of-friends clustering links any two particles closer than a
linking length b; clusters are the connected components of that
relation. The parallel algorithm computes this in two levels.

**Phase 1** runs entirely inside each process, in stages: *phaseA* — a
per-PE union-find over the PE's local trees, computing components among
each PE's own particles; *phaseB* — detection of links that cross PEs
of the same process, over frozen phaseA results, emitting merge edges;
*merge* — a small per-process serial union-find over those edges; and
*relabel*. The result: every particle is labeled with the complete FoF
component it belongs to *as restricted to its process*. We call such a
process-local component a **fragment**, and we name it by its
**process-tip**: the global order (input index) of its minimum-order
particle. Two properties make process-tips powerful: they are globally
unique without any communication (a global order is unique by
definition), and every particle of a fragment lives in exactly one
process.

**Phase 3** discovers the merge graph *between* processes. A key
theorem keeps it simple: after phase 1, if two particles within b of
each other carry different tips, they necessarily belong to different
processes — so cross-tip proximity is exactly the definition of a merge
edge, no ownership tests needed. A tree walk finds such pairs, a
distributed union-find (UF_2) over the touched process-tips computes
the global components, and a relabel finishes. In between, an upward
pass refreshes per-tree-node annotations (min/max fragment ids) that
the walk's certificates consult.

Everything below is an attack on some term of this pipeline.

## 2. The walk: certificates, suppression, and a retired design (3a/3b)

The phase-3 walk's efficiency rests on three prunes. A *negative
certificate*: if the minimum distance between two nodes' bounding boxes
exceeds b, no pair inside can link — prune. A *positive certificate*:
if both nodes are uniform (each entirely one fragment) and their
*maximum* distance is within b, every pair links — emit one edge, don't
descend. And *SEEN suppression*: a process-level table of fragment
pairs for which an edge already exists; the first witness wins and all
later encounters of that pair are skipped. At 80M these prune in
staggering ratios — roughly 400 million negative prunes and 100
thousand suppressions funnel to 22,326 actual edges.

An early design (3b, "parking") proposed going further: when several
concurrent searches chase the same fragment pair, park the redundant
ones. Two Anvil measurement campaigns killed it, instructively.
Per-process redundancy *falls* with process count (~P^-0.89) — the
laptop extrapolation that motivated the design was wrong-way. A
single-process control showed the residual redundant work is
*unsavable*: it is the verification of pairs that produce no edge,
which no memoization of successes can suppress. And concentration
histograms showed the redundancy that does grow with P is spread
maximally thin — nothing for a parking mechanism to grab. Ceiling: ~1%
of walk core-seconds. Retired.

## 3. The dual-tree walk: 20x from fixing the walk's shape

The original walk was source-driven: each partition swept its flat
list of target leaves against the global tree. Hidden in it was a
quadratic-flavored term — every opened source node re-scanned the
entire local target-leaf vector — that made low-process-count runs
grotesquely slow and scaling superlinear.

The replacement is a symmetric dual-tree co-descent: both sides refine
together, so pruning tests compare size-matched boxes. Version 1
underperformed (slower than the original at 1M!) and taught the real
lesson: the naive 8x8 joint expansion is only ~5% selective at its
leaf frontier. Version 2 added an alternating split-the-larger-side
policy and closest-child-first ordering — witnesses are found early, so
SEEN suppression covers the rest of the expansion. Result at 80M: walk
speedups of 20x / 15x / 7.6x / 2.9x / 1.6x at P = 1..16, identical
outputs, and redundancy concentration collapsed as a side effect
(hottest pair: 3,230 descents down to 32) — burying 3b a second time.
The dual walk is the default; the transposed walk is kept forever as an
independent A/B oracle (`-w transposed`).

## 4. PhaseA: certificates, connectivity suppression, and two failed memos

With the walk fixed, phaseA became the wall: 25.7 s at P=1, scaling at
only 4.4x on 16x nodes, slowest PE 3.4x above the average. The cause is
density skew — clustered data concentrates pair work onto the PEs
holding halo cores.

Two mechanisms attacked the *work* itself, both intra-process analogues
of the walk's machinery. Positive certificates, hierarchical: when a
node pair's maximum distance fits within b, the whole cross product
links at once, and the node is memoized as a fragment representative —
in effect, fragments form hierarchically before they are named. And
connectivity suppression: a monotone per-node "connected, with
representative" memo (a find on the representative stays valid across
merges — path compression at node granularity). Any node pair whose
sides are internally connected and already share a component is pruned
wherever it is encountered.

Two variants of *negative* memoization were also built and measured
worse — exact-epoch invalidation added 140% map churn at subcritical
density; epoch backoff blocked fresh suppressions. Positives-only is
the shipped design: failed checks are cheap by construction, so
remembering failures buys less than it costs.

At 80M the package delivered 4.3–7.5x on phaseA at every P (7.34 to
0.98 s at P=8) and — the subtler effect — compressed the skew from 3.4x
to 1.6x: the hot PE's excess had largely been *redundant re-proving of
connectivity*, so suppression attacked imbalance from the work side.
A proposed alternative, density-weighted placement, was measured
against this and deferred: perfect placement would have recovered
~0.45 s of a then-6.4 s pipeline.

## 5. Sparse UF_2: the bookkeeping that touched 23.7 million fragments so ~7,000 could matter

A Projections trace then showed the local phase dominated not by
linking but by *bookkeeping*: counting particles per process-tip (per-PE
maps merged serially under a process lock, ~1.5 s), enumerating ~3M
fragments per process to assign dense union-find vertex indices
(~0.7 s, one PE per process), a pre-allocated vertex array (~1–1.5 GB),
and O(V) scans inside the union-find library. All of it proportional to
23.7 million fragments — of which only about 7,000 are ever touched by
a cross-process edge.

The fix has two halves, and the first is an observation rather than
code: a process-tip is *already* globally unique, and every particle of
a fragment lives in one process — so the owner-decodable vertex id the
distributed union-find needs is a pure per-particle bit operation,
(process << 43) | tip. No counting, no lock, no enumeration. The second
half is a library addition: lazy vertex storage, where union-find
vertices live in a hash map and are created on first touch, with the
library reconstructing a vertex's full id through a registered inverse.
Dense array storage is untouched for existing clients (verified by
running the library's explicit-graph example identically under both
library versions).

The measured effect exceeded its own prediction. Tip encoding: 2.087 to
0.036 s (58x). And the upward pass — whose code was untouched — fell
from 2.151 to 0.108 s, because its old cost was collateral damage: heap
churn from the ~24M map allocations the encoding step no longer
performs. The union-find phase itself dropped 4x, and per-process
memory fell by the vertex array.

## 6. Phase 1's concurrency structure: fairness, chains, and pools

Three successive changes turned phase 1 from a barrier-stepped global
sequence into an overlapped, self-scheduled pipeline. Each was driven
by a specific measured imbalance, and the sequence itself is a small
case study in how load-balancing mechanisms should be chosen: measure,
identify the *kind* of imbalance, and apply the narrowest mechanism
that addresses it.

**Fair division (static).** PhaseB's original rule — the lower PE of
each pair walks all their shared subtree pairs — gave PE i of an N-PE
process N-1-i partners: triangular load, ~11x skew. Replaced by a
symmetric hash of the two subtree root keys choosing the walker per
subtree pair. (An amusing vetting detail: the plausible
"even/odd of the smaller PE" rule is badly unbalanced — worked out on
paper, it assigns 3,0,2,1 pairs at N=4.)

**Within-process chaining (barriers removed).** The driver used to
interpose a *global* reduction between phaseA, phaseB, merge, and
relabel. But after registration, every dependency in that pipeline is
process-local. The barriers were replaced by per-process atomic deposit
counters: each PE deposits stage completion; the last depositor
triggers the next stage with plain entry sends; the per-process serial
merge runs inline on the last phaseB depositor (exclusive access holds
by construction); one global reduction remains at the very end. No PE
ever blocks — it is a completion count with message-driven
continuation, not a barrier. In the 480-PE timeline view the effect is
directly visible: early processes' phaseB spikes ride on top of late
processes' phaseA tails.

**The phaseB pool (dynamic).** The same timeline showed phaseB
stragglers trailing against idle sibling PEs. PhaseB has a property
that makes dynamic scheduling trivially safe: a subtree pair reads only
frozen data and *emits edges into the executing PE's own buffer* — the
later merge is idempotent to duplicates — so any PE of the process may
execute any pair. The static assignment was replaced by a process-wide
pool: the last phaseA finisher enumerates the pairs; PEs claim units
through an atomic index until it drains.

The pool's evolution then followed the diagnostics. The first Anvil
A/B cut the phaseB wall 41% but did not flatten it — the balance line
showed one PE still at ~0.148 s, *reproducible to the millisecond*,
which is the signature of one indivisible unit, not statistical
imbalance. So the pool build gained geometry: pairs enter only if their
boxes are within b of each other (the walk's own test, run once at
build), every surviving pair splits one level into its child cross
product, and — after a measured regression showed unconditional
grandchild splitting inflating per-unit fixed costs — the second split
level is gated to *overlapping* pairs only, the only geometry that can
hide a giant. A per-PE "longest single unit" diagnostic was added to
the output so the divisibility question answers itself in every future
log. Finally, LPT ordering: units carry a geometric cost key
(overlapping pairs first, by descending overlap volume), the pool is
sorted, and claims drop to single units so the sorted head spreads
across PEs.

Final interleaved measurement at 4 nodes / 480 PEs: phaseB wall 0.251
to 0.066 s (-74%), reproducible to the millisecond, with the per-PE
average *unchanged* — the synchronization and granularity overheads
priced out at zero. What remains is one 63 ms depth-2 unit sitting just
above a ~46–50 ms *cross-process* floor (the heaviest process carries
26x the phaseB work of the lightest — no process-local mechanism can
touch that). Both are understood and recorded: a third split level is
the knob for the former; moving work between processes is the (bigger)
mechanism for the latter. At current stakes, both are deferred.

## 7. The per-chare grid: a negative result that paid its way

Mid-campaign, a promising idea from first principles: inside a dense
chare, replace tree-walk linking with a cell grid. With cell side
b/sqrt(6), two guarantees hold with *no distance tests*: all same-cell
pairs link (the cell diagonal is b/sqrt(2)), and all pairs in
face-adjacent cells link (maximum separation exactly b). So a dense
chare collapses to: union each cell into a clique through its first
particle, union face-adjacent occupied cells, and test distances only
across a small residual stencil. Gated per chare on occupancy (expected
particles per cell), computed from the chare root's density.

It is exactly correct (validated by forcing it onto every chare across
the whole test suite, including periodic boundaries), and at 120 PEs on
80M it won ~10% of the slowest-PE phaseA time. But the win decomposed
tellingly: the *average* moved 1.9% while only the max moved — the grid
is a skew compressor, not a work reducer — and multi-node runs showed
the advantage fading with PE count and inverting by 480 PEs. The
mechanism: chares shrink as PEs grow (particles per chare ~ N / (8 x
PEs)), the grid accelerates only *intra*-chare linking, and
surface-to-volume erodes exactly that fraction. Default off; the
controlling ratio is documented so future datasets can predict whether
`-G` is worth an A/B (larger datasets restore the ratio — worth one
retry per major dataset-size step).

The grid also settled a methodological dispute worth recording. An
early claim — "the 1M subsample dilutes halo density ~80x in b-units" —
was challenged and found *wrong*: uniform subsampling with b scaled to
the mean separation is scale-free (rho b^3 is invariant), so mean
density in b-units cannot distinguish 1M from 80M. What differs is the
*tail*: finer sampling resolves density cusps that coarse sampling
blurs, and the 80M snapshot is also a later, more clustered output. The
grid's tail-only win at 80M against laptop parity is precisely the
signature that confirmed the tail explanation.

## 8. Getting the harness out of the way (and out of the traces)

Two costs that were never part of the algorithm had grown large enough
to pollute measurements. The component-size histogram was computed by
concat-reducing every PE's (label, count) pairs to PE 0: ~400 MB of
payload, *re-copied at every level of the reduction spanning tree*
(visible in traces as an all-PE block of black runtime overhead),
followed by a 24-million-entry serial merge — 5.2 s at P=8.

The replacement exploits a property the sparse-UF_2 encoding provides
for free: the *sign* of a particle's final label classifies its
component. Positive labels are untouched fragments — process-local by
construction — so each process computes their exact sizes internally
(via an intra-process reduce-scatter keyed by label hash, not a locked
merge) and contributes a fixed 64-bin histogram to a standard sum
reduction. Only the ~7,000 edge-touched components need cross-process
summing — about 112 KB. Result: 5.2 s to 0.23–0.28 s at scale, and the
black blob left the traces. Two general lessons went into the shared
practitioner notes: pre-merge large reductions at the process level,
and for large concatenation payloads skip the spanning tree entirely —
trees amortize per-message cost (~2 us) and *multiply* per-byte cost
(0.1–0.5 ns/B).

Separately, remote-particle slimming: cache fetches used to ship the
full ~112-byte Particle when the walk reads only position and label —
an opt-in per-app declaration now ships 20 bytes (5.6x on the wire).
The cached *storage* still reconstructs full particles; parameterizing
the cached type is the known next step, now motivated by memory limits
at the ~2B dataset.

## 9. Infrastructure findings along the way

Not speedups, but load-bearing:

- **The array-map creation race.** A "Local branch of array map is
  NULL!" crash at 32 processes traced to a genuine Charm++ trap: array
  creation with bindTo plus a *freshly created* map group declares no
  dependency on the map, so the array construction message can beat the
  map's installation on a remote PE. Fixed with user group dependencies
  on the array creation messages (the message waits; no thread waits).
  Reproduced and validated locally; documentation PR filed upstream.
- **htram aggregation for UF_2 is a net loss at current scale.** The
  final interleaved measurement: aggregation ON costs 1–2.4 s of the
  union-find phase at 480 PEs while carrying 22k edges — tram group
  creation and per-destination buffers scale with machine size (they
  appeared in traces as a per-PE block of constructor-time overhead).
  Default flipped to off in both repos; the flag remains for the
  multi-billion-particle regime it was always aimed at.
- **The 64-bit audit** ahead of the ~2B dataset: global counts and
  orders widened past the 2.147e9 int cliff; exact wide dedup keys.
  Tipsy's header remains int32 *by format* — beyond 2^31 particles the
  file format, not the code, is the wall.
- **Test-harness lessons.** The strict phase-1 test is only valid
  single-process (it compares process-tips against a global reference) —
  and, more subtly, the end-to-end check *cannot* catch phase-1
  under-merging, because phase 3's edge predicate silently repairs it.
  The narrow test now guards itself. And on SMP builds, `+pN` without
  `++ppn` launches N one-PE *processes* — a configuration trap that
  cost one debugging session and is now documented everywhere.

## 10. Methodology, distilled

The campaign's speed came less from any single idea than from
discipline that made each idea cheap to try:

1. **A byte-identical output gate.** Every optimization must reproduce
   the exact components line. This converts "is it correct?" from a
   judgment into a grep.
2. **Keep the oracle.** The transposed walk, the `-u serial` union-find,
   the `-G 0` walk, dense-mode vertex storage: every replaced mechanism
   survives behind a flag as an independent cross-check.
3. **Interleave A/B runs in one allocation.** A cross-allocation
   comparison of a ~0.2 s effect is noise: one early pool run "beat"
   the baseline on a stage its commit could not touch, exposing the
   allocation itself as the variable.
4. **Instrument the decision.** The per-PE balance lines, the stage
   timers, and the max-single-unit diagnostic were each added *so that
   the next measurement would answer the next question by itself*.
5. **Read the raw traces when the GUI runs out.** The black-region
   diagnosis (unattributed per-PE gaps, all starting the instant PE 0
   constructed the tram groups) came from a 40-line script over the
   per-PE logs, not from the visualization.
6. **Record failures with their evidence.** 3b parking, negative
   memos, dual-walk v1, unconditional grandchild splitting, the b-unit
   dilution claim: each retired path is written down with the numbers
   that killed it, so it stays dead — or comes honestly back if the
   regime changes.

## 11. Where this leaves the project

At 120 cores the algorithm now spends its time in exactly two places:
phaseA's balanced work floor (which strong-scales ~1/P on its own) and
the phase-3 walk — more than half of which is the remote-fetch/resume
tail, latency-bound and imbalanced (51x spread in leaf visits across
PEs at 480 PEs). End-to-end, input and decomposition (~10 s) now dwarf
the algorithm entirely.

Which is to say: phase 1 has been made to disappear, and the original
research target is finally the actual frontier. The walk's resumption
phase — asynchronous control, streaming edges into the distributed
union-find during traversal instead of walk-quiesce-drain, and
partition-level load shaping — is where both the remaining time and the
open questions live. The ~2B dataset will press there first, with a
memory dimension (cached-particle typing, response-shipping depth) that
already has a worked design waiting.
