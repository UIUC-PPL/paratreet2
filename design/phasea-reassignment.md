# phaseA light reassignment: feasibility analysis

Kale's proposal (2026-08-11): let a PE run phaseA over TreePieces
registered to a sibling PE of the same process, without touching
Charm++'s location manager. Analyzed 2026-08-11 against main d775e86
(read-only agent audit; every claim carried file:line evidence in the
session record). Verdict: SOUND, with two one-line correctness
constraints and one gating measurement that does not exist yet.

## 1. Entry-method audit: the window is clean

Between registration and relabel, no Charm++ entry method targets a
TreePiece element and nothing depends on a piece's Charm++ home. The
Driver runs phase 1 inline on its [threaded] entry with the TreePiece
array quiescent (build QD + blocking collectMetaData precede it,
Driver.h ~296-315). The app's only TreePiece entries are
callPerTreePieceFn (registration — the hook at which assignment is
implicitly made today), upwardPass (after relabel), and startDual
(phase 3). Every send inside the phase-1 chain is either an FoFPhase1
group-element send indexed by PE (stage triggers), node-branch shared
memory, or a reduction. phaseABody itself sends nothing.

Exactly two pieces of bookkeeping key on the registration PE, and both
are the hook, not an obstacle:

- FoFPhase1::registerTreePiece pushes into the executing PE's branch
  list (decides whose flat index space / union-find / memos / label
  passes cover the piece);
- FoFPhase1Node::registerTreePiece buckets pe_treepieces by CkMyPe(),
  and buildPoolSlice enumerates ONLY cross-bucket pairs.

## 2. Correctness: the grouping theorem

Phase 1 computes, per process, the connected components of the
process's particles as: partition the pieces into groups -> phaseA
within each group -> phaseB pool over every cross-GROUP pair ->
mergeBody unions. This is correct for ANY partition into groups; "by
home PE" is merely today's choice. "PE-tips on the wrong PE" is not a
meaningful state: a tip is a global particle order stored in a
particle; the per-PE structures (uf_parent, flat_order, cert_rep,
roots/rep_label/root_counts) follow the branch that did the work by
construction; mergeBody and everything downstream (tip encoding with
CkMyNode — identical across the process's PEs, phase 3, the counter's
per-root deposits) are process-scoped.

Constraints:

1. EXCLUSIVE ownership per piece (unsynchronised uf_parent writes, the
   freeze's particle/node writes, and cert_rep's node-pointer -> flat
   index memo all demand it).
2. RE-KEY the pool to the assignment, not registration (FoFPhase1.h
   registerTreePiece bucket + buildPoolSlice iteration). Missing this
   silently under-merges within a process: two same-registration-PE
   pieces assigned apart are linked by NEITHER stage. Release-mode
   phase 3 repairs the final labels invisibly, so only fof1's
   phase-1-exact test catches it — keep that test in the gate.
3. Assign AFTER the registration barrier (registerTreePiece writes an
   unguarded vector on the executing PE), at the top of
   startPhase1Chain, deterministically from the frozen registry.
4. The branch's piece list must remain the assignment for the whole
   window — about ten downstream passes iterate it (relabel, encoding,
   label application, counting, verification, collect).
5. GEOMETRY-AWARE assignment is load-bearing, not a refinement: a
   PE's pieces are an SFC-contiguous spatial block, so adjacent-piece
   pairs are intra-group and cheap in phaseA (A_cross mean 5.2 us);
   scattering the assignment moves them into phaseB pool units
   (mean 13.1 us) of a process that is already hot. Claim own pieces
   first, then unclaimed sibling pieces adjacent by the pool's own
   mindist2 gate.
6. FLOOR: the largest single piece's self-pair cost. The theorem
   permits sub-piece groups (a hot piece's root children) as the
   escape hatch, at the cost of a pool-build change. Record, do not
   build first.

## 3. The gating measurement (does not exist yet — DO FIRST)

Recorded phaseA skew is global over PEs and cannot distinguish
within-process from cross-process; only within-process skew is
reachable by shared-memory reassignment. Numbers on record: 80M
max/avg grows 1.28 (120 PEs) -> 2.41 (1920 PEs); 2B/1920 PEs
phaseA_s 0.271/1.009/2.811 = max/avg 2.79.

~15-line instrument (the p3_node_redundant / memoryStats idiom):
deposit per-process sum/max of t_phaseA on the node branch; report
within-process skew (max over processes of proc_max/proc_avg) and
cross-process skew (max proc_avg / global avg); their product
reproduces the 2.79. If within ~1, STOP — the item becomes
cross-process placement (or fewer, larger processes).

Also add per-PE sum(n_i^1.28) and max(n_i) beside density_x and
correlate against t_phaseA. Context: the density predictor
(sum n^2/V) collapsed at scale (r_phaseA 0.39 at 120 PEs -> -0.10 at
1920; optimization-sequence.md rejected prediction-push on that
basis) — but the cost probe says the predictive feature for SELF
pairs is SIZE, not density (m1=n^2 R2 0.86, m3=2n R2 0.87 alone,
log-log slope 1.28; the density-like m2 scores 0.14 on self pairs).
Suppression makes dense regions near-linear, which is exactly why
density fails and size should not. Never checked per PE; one
reduction line. Falsifiable prediction: the hot PE holds FEWER,
LARGER pieces (DecompArrayMap balances sum n, cost goes ~n^1.28).

## 4. Why this is a bin-packing problem with ample granularity

From the cost probe (80M/32 processes): phaseA SELF pairs carry 59.9
of ~67 phase-1 core-seconds and fit at R2 0.90; one self-pair record
is one TreePiece; ~600 pieces per process onto 15 PEs. A piece's self
cost is intrinsic (independent of assignment) — better than phaseB,
where a unit's cost depends on suppression history. Self cost is
predicted by the piece's particle count alone (already in hand as
s.n) — FragData::n_below is NOT a prerequisite for the phaseA half.

## 5. Design choice: dynamic claim, cost-ordered

Prefer reactive claiming (atomic flag per piece; own-first, then
geometry-adjacent unclaimed; self pair on claim; cross pass over the
realized set preserving self-pairs-first ordering; realized grouping
recorded for the pool build) with the predictor as the CLAIM PRIORITY
rather than a planned LPT assignment: the one recorded
prediction-push placement failed at scale, the one recorded dynamic
pool (phaseB, 9b6bf65) worked and is on main, and dynamic claiming
degrades gracefully when the predictor is wrong. NUMA note: phaseB's
"borrowed work was cheaper" finding does not transfer (those were
serialized copies rebuilt locally); stolen pieces read the owner's
first-touch pages.

## 6. Prior art

- The "no entry methods on TreePieces during phase 1" observation was
  NOT previously recorded anywhere; the nearest is
  phase1-scaling.md's within-process-chain dependency audit ("NO
  cross-process traffic exists anywhere in phaseA..relabel").
- phaseA work sharing proposed three times, never built:
  phase1-scaling.md fix direction 2 and the grid A/B decision
  ("perfect intra-process stealing bounds phaseA at ~avg REGARDLESS
  of the tail's cause — threshold-free and dataset-agnostic"; ~30%
  ceiling then, deferred until phaseA's share grew);
  opt-report-2026-07.md.
- Old paratreet's 0001-add-shared-memory-parallel-help patch
  (2026-06-20): helpers stole from a LIVE traversal queue and had to
  be read-only (no path compression, no dedup insert). phaseA's
  disjoint pieces make the present case strictly easier: the helper
  owns the piece and writes freely.

## 7. Implementation order and gates

1. Instrument only (section 3). Land alone, run at 80M + 2B.
2. If within-process skew is large: the claim pool (assignment vector
   on the node branch, atomic claim, re-keyed pool build), behind
   FOF_PHASEA_STEAL=0 as the A/B, everything else untouched.
3. Gates: fof1 4-PE exact (the only true phase-1 test), 1M full
   333889, 8M stats bit-identical, 80M components byte-identical.

## 8. Measured: 80M skew split (job 19782163, 4 nodes/480 PEs, 3 reps)

All reps correct. within 1.44/1.53/1.45, cross 1.16/1.20/1.14, global
1.46/1.49/1.56 — the phaseA skew at this scale is PREDOMINANTLY
WITHIN-PROCESS, which is the regime the light-reassignment scheme can
reach. size_r 0.178-0.200 and density r_phaseA 0.154-0.162: BOTH
per-PE predictors are weak at 480 PEs (the probe's R2 0.9 is per-pair,
not per-PE aggregate), which favors the dynamic-claim design over
planned LPT — the claim protocol needs no predictor to balance;
prediction is at most an ordering hint. max_piece_n 20,993 against
~167k particles/PE: the piece-granularity floor does not bind at 80M.
The decisive 2B point (global 2.79 recorded) rides job 19782161.

## 9. Measured: 2B at 16 Frontier nodes / 128 processes (Ritvik,
   traced run, 2026-08-11; scale label corrected same day)

From Ritvik.s projections-traced dist-mode run on current main. The
banner "128 nodes" is Charm nodes = PROCESSES: 128 processes x 14 PEs
= 1792 PEs = 16 PHYSICAL nodes at his fixed 8 processes/node. Against
the untraced sweep.s 16-node row the walls match (phaseA 2.54 vs ~2.6,
phaseB 2.81 vs 2.88) — tracing overhead is minor here, an earlier
inflation claim retracted (Kale.s challenge: phaseA has too few
entries to trace for seconds of overhead — correct). The apparent
TreeCanopy-cache-loading anomaly (5.56 s) DISSOLVED on inspection
(Kale): the framework print brackets the whole preTraversalFn — all of
phase 1 + encoding + upwardPass + load (5.172+0.124+0.353+0.028 =
5.68 ~ 5.56 printed); the true cache load was 0.028 s. The print label
is fixed on main; any table column harvested from the old label is
preTraversalFn-inclusive. The skew point: `phaseA_skew: within 1.84 cross 1.40 global 1.57
size_r -0.031 max_piece_n 138233`. WITHIN-PROCESS SKEW STILL DOMINATES
at 128 nodes — the half-1 gate holds at the third scale point (80M/480
PEs: 1.44-1.53 vs 1.14-1.20; 2B/1920 Anvil PEs, ALSO 128 processes —
directly comparable — pending job 19782161).
size predictor again ~0 per PE: dynamic claiming confirmed. Watch
item: largest piece 138k vs ~31k average (4.5x) — with cost ~n^1.28
the single-piece floor may bind at this process count; the sub-piece escape
hatch (section 2 constraint 6) is the answer if it does. Same run:
phaseB max/avg 21x (0.004/0.134/2.812) with ONE pair at 0.634 s = 23%
of the stage wall — direct evidence for half 2's tail splitting.

## MEASURED AND RESOLVED (2026-08-18, relay12, jobs 5301010/5301011):
## the gating measurement exists, and it retires the barrier scheme

Section 3's gating measurement (within-process phaseA skew, per stage)
ran at 2B on 16 and 64 nodes. The stopping rule ("if within ~1, STOP")
does NOT fire at the critical process (1.226/1.262) — but the scheme is
retired anyway, for a better-diagnosed reason:

1. **The work is self, the skew is cross.** phaseA is 92-95% self work
   (self:cross 17.4:1 at 16 nodes, 11.3:1 at 64) — but the SELF stage's
   within-skew is only 1.09-1.10 median, while the CROSS stage is skewed
   2.65-2.78 (max 5.7-7.4). The stage any PE may take is already flat;
   the owner-bound stage carries the skew. The reverse of the scheme's
   premise.
2. **Because the self stage is ALREADY balanced by S1**, which is on in
   every run: 12.2-12.8% of claims are foreign, ~10.5 of 14 PEs per
   process steal. The 1.09 is the residual AFTER stealing — there is no
   untapped self imbalance for a new self-stealing scheme to harvest.
3. **The barrier variant is worth -1.6% of Iteration 0 at 16 nodes and a
   small LOSS at 64** (the barrier converts the cross stage's 2.7x skew
   into an additive cross_max term).
4. **A "no-barrier" model suggesting -7.1%/-3.5% was WRONG**, withdrawn
   after Kale challenged it: self and cross are not independently
   schedulable (a claimed piece carries both its self walk AND p-1 new
   cross pairs), and cross is owner-bound by an ADDRESSING constraint
   (certRep/connectedRep index the claimer's PE-local uf_parent), not by
   synchronisation. What that number actually measures is the ceiling of
   PERFECT PIECE-TO-PE ASSIGNMENT — a better claim policy.
5. **Critical-path decomposition**: 57-58% of phaseA's excess over the
   ideal floor is ACROSS processes (max/mean 1.43 at 16 nodes, 1.60 at
   64, growing with scale) — untouchable by any within-process scheme.

WHAT SURVIVES, in value order:
(a) ACROSS-process placement — the 57-58% — which is the piece-load-model
    territory again, now for phaseA self cost (recall: node 55 = fewer,
    LARGER pieces; particle count has zero correlation).
(b) A CLAIM PRIORITY that prices the marginal cross cost of taking a
    piece (p-1 new pairs against p held) — small, local, headroom
    -368 ms at 16 nodes / -98 ms at 64; and total cross work is CONVEX
    in pieces-per-PE, so equalising counts REDUCES it, not just
    redistributes. Missing input: a per-PE (pieces, self, cross) dump —
    a few lines in the existing deposit.
