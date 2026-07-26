# Optimization of the paratreet2 parallel FoF cluster finder: an account
### Internal technical report — 2026-07-18 through 2026-07-26

All performance claims below are anchored to the same benchmark unless
noted: the 80M-particle LAMBS snapshot (lambb.00500), linking factor
b = 0.2 x mean interparticle separation, on Anvil at 8 processes x 15
worker PEs (120 cores), reconverse runtime. Laptop measurements
(Apple Silicon, 8 cores) are labeled as such. The correctness
invariant held through every change: 23,707,197 components with a
byte-identical size histogram, verified after each optimization wave.

## 1. The algorithm and its vocabulary

Two-level friends-of-friends. **Phase 1** computes, entirely within
each process, the complete FoF restricted to that process's particles:
per-PE union-find over frozen local trees (phaseA), cross-PE detection
within the process (phaseB), a per-process merge, and a relabel. Each
resulting process-local component is a **fragment**, identified by its
**process-tip**: the global order of its minimum-order particle —
globally unique, computed with no communication. **Phase 3** discovers
merge edges between process-tips of different processes by a tree walk
over the distributed forest (any two particles within b holding
different tips after phase 1 are necessarily on different processes),
runs a distributed union-find (UF_2) over the touched process-tips,
and relabels. Between the phases, an upward pass refreshes per-node
annotations (fragment-id intervals min_frag/max_frag) that the walk's
certificates consult.

At the start of this period (2026-07-23 measurement, the earliest
complete 80M timing), the algorithmic cost at P=8 was ~15 s:

| stage | s at P=8 (07-23) |
|---|---|
| phase 1 | 7.82 |
| tip encode | 2.09 |
| upward pass | 2.15 |
| phase-3 walk | 2.80 |
| UF_2 | 0.17 |
| relabel | 0.05 |

By 2026-07-26 the same computation, same configuration, same output:

| stage | s at P=8 (07-26) | change |
|---|---|---|
| phase 1 | 1.09 | 7.2x |
| tip encode | 0.036 | 58x |
| upward pass | 0.108 | 20x |
| phase-3 walk | 0.718 | 3.9x |
| UF_2 | 0.043 | 4x |
| relabel | 0.024 | 2x |

Algorithmic total ~15 s -> ~3.2 s (~4.7x) at fixed core count, with
the harness's own statistics cost cut separately (Sec. 8). The
sections below give each contribution, including the attempts that
failed — several of the failures bought the understanding that the
successes were built on.

## 2. Walk certificates and suppression (the 3a package)

The phase-3 walk prunes on three certificates: negative (box gap
mindist > b: no pair can link), positive (both boxes uniform — one
fragment each — and maxdist <= b: every pair links, emit one edge
without descending), and SEEN suppression (a process-level table of
(g, f) process-tip pairs for which an edge already exists; first
witness wins). At 80M these fire at overwhelming ratios: 397.6M
negative prunes and 105k suppressions funnel down to 22,326 emitted
edges. The positive certificate proved structurally subsumed by
suppression (it fired once in the entire 80M run) but is kept: it is
O(1) after memoization and its intra-process cousin (Sec. 5) earns
its keep.

## 3. Failed path: 3b parking (redundancy elimination)

Design 3b proposed parking redundant concurrent searches over the
same fragment pair (searcher identity + reissue + counter-quiescence).
Two Anvil measurement campaigns retired it. Per-process redundancy
FALLS with process count (~P^-0.89) rather than growing as the
loopback extrapolation had suggested; a P=1 control showed the
residual ~100k redundant descents are unsavable no-edge verification
work (SEEN cannot mark searches that will not find an edge); and the
concentration histograms showed the redundancy that does grow with P
is maximally spread (73% of pairs seen once) — nothing for a parking
mechanism to grab. Verdict: the mechanism's payoff ceiling was ~1% of
walk core-seconds. Retired. The dual-tree walk (Sec. 4) later
collapsed redundancy concentration as a side effect (max descents per
pair 3,230 -> 32), burying the question permanently.

## 4. The dual-tree walk

The original walk was source-driven: every target partition's flat
leaf list swept against the global tree. Its cost had a hidden term —
each opened source node re-scanned the entire local target-leaf
vector — which produced superlinear scaling in N and grotesque
low-P costs. The replacement is a symmetric dual-tree co-descent
(both sides refine together), with two refinements found necessary in
v2: an 8-way alternating split-the-larger-side policy (the naive
64-way joint product was only ~5% selective at its leaf frontier —
20.4M leaf pairs for 994k survivors at 1M) and closest-child-first
ordering, so witnesses are found early and SEEN suppression covers
the sibling expansions.

80M A/B, walk seconds, transposed -> dual:

| P | 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| speedup | 20x | 15x | 7.6x | 2.9x | 1.6x |

Identical outputs everywhere. The dual walk became the default; the
transposed walk is retained permanently as an independent A/B oracle
(-w transposed). Failed sub-path recorded: dual v1 without the split
policy and ordering was SLOWER than transposed at 1M — fanout
selectivity, not tree symmetry, was the actual lever.

## 5. Phase-1 certificates and connectivity suppression

Phase-1's phaseA was the next wall: 25.7 s at P=1, scaling at only
4.4x on 16x nodes, with the slowest PE 3.4x above average — density
skew on clustered data. Two intra-process analogues of the walk
machinery attacked the work itself:

- **Positive certificates, hierarchical**: when a node pair's maxdist
  <= b, the entire cross product links; the node becomes a memoized
  fragment representative (first touch O(n), thereafter O(1)) — in
  Kale's terms, fragments form hierarchically before they are named.
- **Connectivity suppression**: a monotone per-node
  connected-with-representative memo (find on the representative
  remains valid across merges — node-granularity path compression).
  Any node pair whose sides are internally connected and already
  share a component is pruned at whatever level it is encountered;
  self-pairs of connected nodes likewise.

**Failed paths recorded**: both attempted variants of NEGATIVE
memoization measured worse — exact-epoch invalidation added 140% map
churn at subcritical density, and epoch backoff blocked fresh
suppressions (1.5x at b=0.8). Positives-only is the shipped design;
failed checks are cheap by construction. Also rejected: a laptop
attempt at density-weighted placement was deferred when the numbers
showed perfect placement would recover ~0.45 s of a then-6.4 s
pipeline.

80M result (the 07-24 sweep): phaseA 4.3-7.5x at every P (P=8:
7.34 -> 0.98 s), skew max/avg 3.38 -> 1.58 at P=8. Suppression
attacks skew from the work side: the hot PE's excess was largely
redundant re-proving of connectivity.

## 6. Sparse UF_2: enumeration-free tips and lazy vertices

After Sec. 5, a Projections trace showed the local phase dominated by
bookkeeping that touched every fragment though almost none needed it:
counting particles per process-tip (a per-PE map merged serially
under a process lock, ~1.5 s), enumerating ~3M fragments per process
to assign dense UF_2 vertex indices (~0.7 s, one PE per process), a
pre-allocated vertex array (~1-1.5 GB total), and O(V) scans inside
the union-find library — all proportional to 23.7M fragments, when
only ~7k process-tips are ever touched by a cross-process edge.

The fix has two halves. **Enumeration-free tip encoding**: a
process-tip is already globally unique, and every particle of a
fragment lives in one process, so the owner-decodable UF_2 vertex id
is a pure per-particle rewrite — (process << 43) | tip — no counting,
no merge, no enumeration (43/20 bit split; the sign bit stays clear
so the -1 sentinel and negative final labels remain disjoint). **Lazy
vertex storage** (UnionFindLib addition): vertices live in a hash map
created on first touch; the library reconstructs a vertex's full id
via a registered inverse; boss counting and labeling iterate the
touched set. Dense array storage is unchanged for existing clients
(regression: the library's explicit-graph example produces identical
counts under both library versions on all 14 test graphs).

80M result: tip encode 2.087 -> 0.036 s (58x). An unpredicted bonus:
upward pass 2.151 -> 0.108 s (20x) with its code untouched — its old
cost was collateral heap churn from the ~24M map allocations the
encoding step no longer performs. UF_2 0.170 -> 0.043 s. Laptop
memory: -215 MB/process at 8M.

## 7. Phase-1 concurrency structure: fair division and barrier removal

Two structural changes, validated together at 80M:

- **PhaseB fair division**: the old rule assigned each cross-PE
  subtree pair to the lower PE — triangular load, ~11x skew inside a
  process. Now each unordered subtree pair is walked by exactly one
  side, chosen by one bit of a symmetric hash of the two subtree root
  keys; every PE pair's ~64 subtree pairs split about in half with
  density mixing. (Vetting note: the plausible "even/odd of the
  smaller PE" rule is unbalanced — 3,0,2,1 at N=4.) 80M: phaseB wall
  0.34-0.42 -> 0.197 s.
- **Within-process chaining**: after registration, every dependency
  of phaseA -> phaseB -> merge -> relabel is process-local, so the
  four global reductions between them were replaced by per-process
  atomic deposit counters (one deposit per PE; the last depositor
  triggers the next stage by entry sends; the per-process serial
  merge runs inline on the last phaseB depositor; one global
  reduction remains at the end). No PE blocks. In the 120-core trace
  the phaseB/merge/relabel work of early processes visibly overlaps
  the phaseA tails of late ones.

## 8. Statistics off the critical path (and out of the traces)

Two harness costs had grown to pollute both timings and traces.
The fragments histogram — the only surviving consumer of per-tip
counting — became opt-in (-g). The components histogram was computed
by concat-reducing every PE's (label, count) pairs to PE 0: ~400 MB
re-copied at every level of the reduction spanning tree (visible as a
black all-PE overhead blob in traces) plus a ~24M-entry serial merge:
5.2 s at P=8. The replacement exploits the label sign: positive
labels are untouched fragments — process-local by construction — so
each process bins their exact sizes locally (an intra-process
reduce-scatter by label hash aggregates fragment sizes without a
locked merge) and a fixed 64-bin histogram is sum-reduced; only the
~7k touched components cross processes (~112 KB). 80M: 5.2 s ->
0.23-0.28 s (21x), and the trace blob is gone. General lessons
recorded: pre-merge large reductions at the process level; for large
concat payloads, direct sends beat spanning trees (trees amortize
per-message cost, ~2 us, and multiply per-byte cost, 0.1-0.5 ns/B).

## 9. Remote-particle slimming

Cache-shipped remote particles carried the full ~112-byte Particle;
the walk reads only position and group_number. An opt-in
pupRemoteParticle lets the FoF payload ship 20 bytes (5.6x wire
reduction on cache fetches; ~460 MB not shipped at 80M P=8 by
arithmetic — no byte counters exist). Laptop 8M 4-process walk:
2.58-2.84 -> 2.29 s. The 80M walk improvement (0.96 -> 0.72 s across
this period) conflates slimming with concurrent changes; it was not
isolated at scale. Cache MEMORY is deliberately unchanged — receivers
reconstruct full particles; slimming the stored copies requires
cached-leaf type parameterization, deferred until memory at ~2B
demands it.

## 10. The per-chare grid (cell certificates), and what its A/B taught

Kale's proposal: inside a dense chare, replace tree-walk linking with
a cell grid. With cell side c = b/sqrt(6), two guarantees hold with
no distance tests: all same-cell pairs link (diagonal b/sqrt(2)) and
all face-adjacent-cell pairs link (max separation exactly b). So a
dense chare's self-pair collapses to: union each cell into a clique
through a first-seen representative, union face-adjacent occupied
cells, and test pairs only across a residual stencil (offsets with
sum(((|d|-1)+)^2) <= 6), skipping already-connected cell pairs.
Per-chare (a chare root is a tight oct box and provides the density
gate n*c^3/vol); PBC-safe; falls back to the walk on degenerate
grids; -G <threshold> flag, 0 = off.

Results: correctness everywhere (forced onto every chare: exact
phase-1 tests, full checks including PBC, identical 80M output).
Laptop performance: parity to slightly worse at every reachable
density — at ~1-4 particles per cell the cliques are thin and the
~160-offset stencil costs more than the already-near-linear
suppressed walk. 80M performance at -G 4: slowest-PE phaseA
0.912 -> 0.816 s median (~10.5%), min and avg essentially unchanged;
thresholds 2 and 16 worse than 4.

The decomposition of that win reshaped the roadmap. Average phaseA
moved 1.9%; only the max moved. The grid is a skew COMPRESSOR, not a
work reducer — and skew compression is exactly what intra-process
work stealing does better (ceiling: max -> avg, ~30% at these
numbers, threshold-free, dataset-agnostic; 0.19 s of residual skew
survives even -G 4). Decision: default off; -G documented as a
per-dataset A/B option; stealing is the preferred next phaseA lever
when one is warranted. The tail-only signature also resolved a
methodological dispute: uniform subsampling with b scaled to mean
separation is scale-free in the MEAN (rho*b^3 invariant — an earlier
"80x b-unit dilution" claim was wrong and is retracted in the
records), so the laptop-vs-80M difference lives in the occupancy
TAIL (finer sampling resolves density cusps; the 80M snapshot is
also later and more clustered).

A multi-node follow-up completed the picture — the grid's advantage
fades with PE count at fixed N and inverts by 4 nodes (medians of
phaseA max):

| nodes | PEs | G0 | G4 | grid effect |
|---|---|---|---|---|
| 1 | 120 | 0.912 | 0.816 | -10.5% |
| 2 | 240 | 0.796 | 0.761 | -4.4% |
| 4 | 480 | 0.370 | 0.387 | +4.6% (loses) |

The trend is structural. Chare count scales with PEs, so particles
per chare fall from ~17k to ~4k across this table; the grid
accelerates only INTRA-chare linking, and surface-to-volume shrinks
that addressable fraction as chares shrink — while the grid's
per-particle overhead is scale-independent and the tail it
compresses shrinks absolutely (phaseA strong-scales regardless). The
controlling variable is therefore **particles per chare, roughly
N / (8 x total PEs)**, jointly with the occupancy tail: -G is worth
an A/B when that ratio is ~15k or more on a dataset with dense
cores, and not otherwise. In particular, LARGER DATASETS RESTORE THE
RATIO — anything from a few hundred million particles upward at
today's node counts, and certainly the ~2B target on proportional
resources, re-enters the regime where -G 4 won; it should be
re-tried at each significant dataset-size step, not written off. On
current 80M production configurations (2+ nodes) the default-off
choice stands.

## 11. Robustness work enabling the campaign (not speed, but load-bearing)

- **Array-map creation race**: "Local branch of array map is NULL!"
  at 32 processes traced to bindTo + fresh setMap declaring no
  dependency on the new map group; fixed with user group dependencies
  (CkEntryOptions::setGroupDepID) on the array creations — the
  message waits, no thread waits. Reproduced and validated locally on
  reconverse. Upstream charm documentation PR filed.
- **64-bit audit**: global particle counts and orders widened ahead
  of the ~2B dataset (int cliff at 2.147e9); exact wide dedup keys
  (the 32|32 packed tip-pair key collides above 4.29e9). Tipsy's
  header remains int32 by format — >2^31 particles requires NChilada.
- **htram aggregation for UF_2**: enabled on the grounds that
  frequent flushes bound the regression and multi-billion-particle
  runs should benefit; measured neutral-to-slightly-worse at current
  edge counts (~22k edges is fixed-overhead territory) with a known
  quiesce-loop timing variance (uf2 occasionally 0.3-1.0 s). Kept as
  scale insurance; its completion protocol (flush+QD+count loop)
  replaced a bare quiescence wait that would silently drop buffered
  unions.
- **fof1 single-process guard**: the strict phase-1 exactness test is
  only valid single-process (process-tips vs a global reference); it
  now aborts multi-process runs with an explanation. Recorded because
  the distributed full check cannot catch phase-1 under-merges — the
  phase-3 edge predicate repairs them silently — so fof1 is the only
  true phase-1 test.

## 12. Where the time goes now, and the frontier

At 80M P=8, the in-algorithm profile is two structures: phaseA
(~0.9 s wall: a ~0.64 s balanced-work floor that scales ~1/P, plus
~0.27 s of density-skew slack — the only part stealing or the grid
can recover) and the walk (~0.72 s, more than half of it a
remote-fetch/resume tail with a 51x min/max leaf-visit spread across
PEs). End-to-end, input and decomposition (~9.8 s: Tipsy read + flush
to subtrees) now dominate the iteration. The open research direction
is the walk's resumption phase: asynchronous control interleaving the
traversal with union-find (streaming edges into UF_2 during the walk
rather than walk -> quiesce -> drain), plus partition-level load
shaping — the regime where the remaining structure in the traces
lives, and where the ~2B dataset will press first.
