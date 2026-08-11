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
