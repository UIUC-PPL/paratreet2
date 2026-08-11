# Optimization inventory: phase 1 vs ArborX FDBSCAN (Prokopenko et al. 2025)

Written 2026-08-11 for the comparison against "Advances in ArborX to
support exascale applications" (papers/advances-in-arborx.pdf): ArborX
+ HACC, FoF as the minPts=2 DBSCAN case, GPU BVH, ~37M particles per
rank clustered in 0.15 s on an A100. Their per-rank shared-memory scope
corresponds to OUR WITHIN-PROCESS PHASE 1 (lists a+b below); they have
no analog of our cross-process phase 3 in this paper (HACC handles
distribution outside ArborX). Structural philosophy difference up
front: ArborX embraces fine-grained atomics (CAS union-find, thousands
of threads, one point-query per thread); we avoid atomics on the hot
path via PHASE SEPARATION (frozen-phase pattern) and amortize with
NODE-PAIR granularity certificates. Both reach "no adjacency graph
ever stored" — they via callbacks fused into traversal, we by
construction (visitors union in place).

## (a) Within a PE (phaseA: per-PE union-find over its TreePieces)

1. DUAL tree-pair walk over piece pairs — node-pair granularity, both
   sides pruned together (ArborX traverses per POINT-query; their
   "pair traversal" halves point-pair work but is not a dual tree).
2. Negative certificate: mindist2(boxes) > b^2 prunes whole pair
   subtrees (their analog: per-query BVH node rejection).
3. POSITIVE certificate: maxdist2 <= b^2 accepts a whole pair — every
   cross pair is a guaranteed link — via memoized star-union (certRep:
   first touch O(n), repeats O(1)), with a branch-free size gate
   ((sum measures)^2/12 > b^2) cheapening the test. No ArborX analog
   at tree level; their DenseBox is the grid-cell cousin (below).
4. CONNECTIVITY SUPPRESSION: monotone per-node connected-with-rep
   memo; a pair both-connected and same-component prunes at ANY level;
   positives-only memoization (negatives measured worse both ways);
   single-witness early exit in the leaf loop; self-pairs-FIRST
   ordering so cross walks see maximal suppression. Component-aware
   pruning with no ArborX counterpart (their early termination is only
   the minPts counting kernel, vacuous for FoF).
5. CELL-GRID solver for dense SELF pairs (gridSelfUnion, -G occupancy
   gate at the chare root): bin + sort; same-cell clique unions AND
   face-adjacent-cell unions test-free (cell side b/sqrt(6) makes even
   face-adjacent point pairs guaranteed within b — one step stronger
   than DenseBox's intra-cell-only elimination); ~160-offset residual
   stencil with first-witness exit. DIRECT analog of FDBSCAN-DenseBox
   (their cell side eps/sqrt(3), intra-cell tests eliminated, dense
   cells become BVH leaf objects). Differences: they coarsen the
   INDEX with dense cells; we keep the tree and gate the SOLVER —
   and today only at the whole-piece self pair (see the question at
   the bottom).
6. Union-find mechanics: union-by-MIN-GLOBAL-ORDER (a total order —
   cycle-free lock-free attach, stronger than rank schemes; GPU-ready
   by construction), full path compression at the freeze.
7. Freeze FUSION: tip write + uniformity annotation + component
   counting in ONE pass over particles (items 10 + PR #2).
8. Representative-indirect relabeling: every later label map applies
   per frozen root (~thousands) + one indexed-load materialization —
   no per-particle hashing anywhere after phaseA.
9. SFC/Morton layout everywhere (64-bit keys — their 64-bit Morton
   fix is something we always had): pieces are spatially contiguous
   per PE, the locality their query pre-sorting manufactures.

## (b) Across PEs within a process (phaseB)

1. FROZEN-PHASE pattern: phaseA freeze makes all inputs read-only;
   per-PE private outputs; one serial idempotent merge; NO atomics or
   locks in any walk. (Their concurrency model is the opposite: global
   CAS union-find. Both are valid; ours also composes with
   message-driven overlap.)
2. PE-TIP COMPRESSION before cross-PE work: labels collapsed to
   fragment tips so all phaseB tests operate over compressed names —
   the same role their Union-on-discovery plays incrementally, done
   here as an explicit level (and the hierarchical design adds another
   level of exactly this).
3. UNIFORMITY annotation (min_frag == max_frag in the node, shipped
   with every copy): O(1) whole-pair certificates post-freeze — the
   single biggest phaseB win on record (~2x at scale). Component-STATE
   in the index; no ArborX analog (DenseBox is density-state).
4. The PAIR POOL: mindist-gated enumeration (pairs that cannot
   interact never become units), split-leveling (no unit hides a
   dense-boundary giant), LPT costliest-first order, atomic claim
   cursor = dynamic self-scheduling over the process's PEs, and the
   parallel pool build (per-thread stripes + sorts + round-robin
   merge). m2-ranked adaptive tail splitting is the measured next step
   (design/phaseab-balancing.md section 13).
5. EACH-PAIR-ONCE structurally: cross-bucket-only enumeration
   (ita < itb), the same idea as their pair traversal's i < j (theirs
   per point via start-at-own-leaf + ropes; ours per piece pair via
   the registry, and per tip pair via SEEN).
6. SEEN dedup: exact two-word TipPairKey, first-witness-wins,
   insert-adjacent-to-emit invariant (losing the table can only
   duplicate, never lose).
7. Star emission via certificates (emitSubtreeTips through the
   memoized representative): ONE edge per certified subtree instead
   of per particle pair — reduces union traffic, not just tests.
8. Deposit-chain termination: barrier-free, QD-free per-process stage
   sequencing (their equivalent is simply the kernel boundary).
9. Edge idempotence at the merge — duplicates harmless, which is what
   licenses cold-cache stealing and device sort-unique dedup.

## What they have that we should consider (the gap list)

1. PAIR TRAVERSAL for the self walk: their start-at-own-leaf + ropes
   guarantees each point pair once, structurally, no dedup table.
   VERIFY whether our self dual walk (a,a) enumerates unordered child
   pairs once or relies on suppression to kill the mirror; if the
   mirror is walked, the i<j guard is a cheap win. [action: check]
2. STACKLESS (rope) traversal: irrelevant on CPU (we recurse), but
   the right shape for the HIP kernel — feed into walk-unification
   stage 4's device driver (their Apetrei-with-Karras-ordering report
   is the reference).
3. DenseBox-as-index (dense cells become BVH leaves): our functional
   equivalent post-phaseA is the uniformity annotation (a merged
   dense region IS uniform = one object for certificates), so the
   index-coarsening as such is not missing — but their version acts
   in the FIRST pass too, which connects to the per-level grid
   question below.
4. Their performance anchor for the GPU port: ~37M particles fully
   clustered in 0.15 s on one A100 (FoF case). That is the number
   gridSelfUnion-as-kernel-one plus the device pair walk should be
   measured against per GCD.

Have already, no action: 64-bit Morton (always had); callbacks/fusion
(visitors by construction); O(n) memory (no adjacency graph, ever);
early termination (minPts counting is vacuous for FoF; our
single-witness exit is the analog that matters).

## The per-level grid question (Kale, 2026-08-11)

Today the grid applies ONLY at the whole-TreePiece SELF pair: the gate
(FoFPhase1.h, phaseABody pair loop) tests occupancy at the CHARE ROOT
(i == j, sa.n * c^3 / vol >= threshold, c = b/sqrt(6)) and cross-chare
pairs plus all sub-levels use the walk. Applying it at EVERY level
makes sense NOW and did not before, for a concrete reason: internal
nodes had no particle count (n_particles = -1 by design), so a
per-node occupancy gate was not computable — FragData::n_below (on
main since 13b1f08) makes it O(1) at every node. Design shape: the
self walk descends until a node passes the occupancy gate, then
grid-solves that subtree and prunes the descent; parents keep the
walk. What it buys: dense cores INSIDE mixed pieces — exactly the
occupancy-tail regime recorded at 2B (cusp/substructure resolution
grows the tail), where the root-level gate misses because the piece
average is diluted. The CPU-measured parity of the root-gated grid
(net neutral at reachable densities, default -G 4 on the A/B oracle)
can only improve under a more selective gate. Extension after that:
CROSS pairs of two overlapping dense nodes (bin both sides, stencil
across) — second step, more code. And per-level grid regions are the
natural GPU kernel batches (walk-unification stage 0's kernel one,
generalized). This also makes our grid and their DenseBox converge
from opposite directions: they coarsen the index globally by density;
we would switch solvers locally by density, keeping the tree.
