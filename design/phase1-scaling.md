# Phase-1 scaling: the flattening is phaseA density skew

**ANVIL 80M CONFIRMATION (2026-07-23, Ritvik's dual-tree-branch sweep,
PRE-certificate/suppression build = the clean baseline).** phaseA AVERAGE
scales as textbook 1/P (17.6 -> 8.9 -> 4.3 -> 2.2 -> 1.13 s over P =
1..16 at 15 PEs/proc) while the MAX PE goes 25.5 -> 4.2 s with max/avg
growing 1.45 -> 2.1 -> 3.4 -> 3.7 (max/min 13.5 at P=16): phase 1's wall
is one hot PE; total work is fine — pure density skew, exactly the laptop
diagnosis. merge() is 2-5 ms at EVERY P on 80M real data: the serial-step
hypothesis is dead. phaseB ~0.5 s flat. After the dual walk (0.64 s at
P=16), phase1 + tip_encode + upwardPass (~8.4 s) is ~90% of algorithmic
time at P=16 — the scaling frontier. The next main-branch Anvil run
measures the suppression layer against this baseline.

Context: the Anvil 80M sweep (step3.md 6h) showed phase 1 speeding up only
4.4x on 16x nodes and still flattening — now the dominant phase at scale.
Laptop investigation, 2026-07-23, 8M inputs, classic Converse (NOTE: netlrts
SMP spends one core per process on a comm thread, so all configs below fit
in 8 cores WITHOUT oversubscription; earlier same-day runs at 3-4 procs x 2
PEs were oversubscribed and are superseded by these).

Instrumentation added (rides both branches): runFoFPhase1 takes an optional
FoFPhase1Stages out-param; fof3 prints
`FOF3STAT time_s: phase1_stages reset/register/phaseA/phaseB/merge/relabel`
(barrier-to-barrier walls, so each stage = its slowest PE/process).

## Findings (8M Plummer unless noted)

1. **Phase 1 is phaseA.** Everything else — reset, registerFoF, phaseB,
   the per-process serial merge, relabel — is <= 0.03 s in every config.
   The suspected serial-merge bottleneck is ACQUITTED on Plummer (watch it
   on LAMBS via the balance lines: clustered data has more cross-PE edges).
2. **phaseA's wall is pinned to its slowest PE, and the skew is density.**
   Single process, 7 worker PEs (clean, no oversubscription):
   - Plummer:  phaseA_s 0.256/0.591/0.840 (max/min = 3.3)
   - Uniform:  phaseA_s 0.111/0.122/0.132 (max/min = 1.19)
   Same N, same config — only the density profile differs. Skew grows with
   PE count (1.05 at 2 PEs -> 1.4 at 4 -> 3.3 at 7). Subtrees are assigned
   to PEs by particle COUNT (balanced to ~9% here), but phaseA's cost per
   PE is the pair-distance work ~ local density; the PE holding the dense
   core bounds the wall. (Uniform is also ~6x cheaper in absolute phaseA
   time at equal N — pair work, not per-particle work.)
3. **Process count per se is irrelevant.** Fixed 4 worker PEs split as
   1x4 / 2x2 / 4x1 processes: phaseA wall 1.05 / 1.08 / 1.13 s — constant.
   (phaseB/merge/relabel go to exactly 0 at 1 PE/process: no intra-process
   pairs, as designed.)

## Prediction for Anvil (falsifiable with data Ritvik already has)

phase1 total ~= phaseA max-PE time, and `FOF3STAT balance: phaseA_s`
max/avg grows with P on LAMBS (clustered >> Plummer). The flattening floor
is the work of the PE holding the densest subtree(s): per-PE AVERAGE falls
like 1/P but the hot PE's work stops falling once the dense region no
longer splits. The phaseA_s min/avg/max lines in the existing sweep logs
test this directly.

## Fix directions (in rough order of leverage)

1. **Density-weighted subtree->PE assignment** — design note §7 already
   prescribes it: phase-1 cost is predictable BEFORE FoF starts from the
   built tree (sum over buckets of local pair estimate, e.g. n_i^2 within
   b-neighborhoods or 0.034*n_i with bucket volume); map subtrees to PEs by
   predicted WORK, not particle count. Static, no runtime machinery.
2. **Intra-process phaseA work sharing** — the old paratreet FoF had
   exactly this tail-imbalance problem and the shared-memory-parallel-help
   patch for it; a per-process work queue of subtree-pair walks lets idle
   PEs steal from the hot PE (design note §6.3d). Complements 1 (handles
   what prediction misses).
3. **Finer subtree granularity** in dense regions (bounded: a single hot
   subtree caps what redistribution can do; oct depth limits apply).

Note the same density-skew mechanism will apply to tip_encode/upwardPass
only weakly (they are per-particle, not per-pair); the balance data will
say whether they need anything.

## Positive certificates in the phase-1 walk (2026-07-23, Kale's proposal)

Kale's review asked: dense regions should be certifiable — apply the
phase-3 case-2 idea (maxdist <= b => every cross pair links) inside phase
1, hierarchically ("per-leaf fragments"). Implemented in walk():

- maxdist2(a, b) <= b^2 (a == b self pairs: box diameter) -> resolve the
  whole pair with NO distance tests and stop the descent. A spanning STAR
  of unions through one representative (O(n_a + n_b)) is correct without
  any internal-connectivity assumption: all cross pairs are genuine links,
  so a's particles connect through b's representative even when a alone is
  not a clique.
- MEMOIZED per node (cert_rep / cert_tip): the first certificate touching
  a node star-unifies it once — the node becomes a fragment — and every
  later certificate involving it is a single unite(rep, rep). This is the
  hierarchical-fragment formulation; without it a hot node with k
  certified partners re-walks its particles k times.
- Conservative size gate before the maxdist2 test (maxdist2 >=
  (sum of box measures)^2 / 12), so subcritical regions don't pay for a
  test that cannot fire. PBC skips certificates (maxdist2 not periodic),
  same exclusion as phase 3.
- phaseB analog: star-EMIT deduplicated (rep_tip, tip) edges.

LATENT FRAMEWORK TRAP found by this work (and the cause of a phase-1 spin
on LAMBS): local-tree INTERNAL nodes carry n_particles = -1 BY DESIGN
(Node.h "non-leaves will have this as -1"); only leaves have counts, and
empty regions are EmptyLeaf(0). Any consumer descending by "child with
particles" must test n_particles != 0, NOT > 0 — with > 0, LAMBS's deep
dense chains (7 EmptyLeaf + 1 Internal(-1) per level) never advance.
firstFlat/firstTip now carry the != 0 rule plus a loud CkAbort tripwire
for genuinely inconsistent trees. Recorded in design/charm-notes.md.

## Connectivity suppression (2026-07-23, same session — the phaseA win)

The certificate analysis showed the residual cost is the SHELL (mindist <=
b < maxdist pairs) — and almost all shell work re-proves connectivity the
UF already knows. Fix (Kale's hierarchical-fragment idea completed): a
monotone per-node "internally connected, representative r" memo (shares
cert_rep — certified nodes are born connected; find(r) stays valid across
later merges = path compression at node granularity), maintained by
`connectedRep`: leaves check directly (early-exit on first root mismatch,
~2 finds when negative), internals consult only their CHILDREN'S memo
entries (~1 hash lookup when negative), so connectivity percolates upward
lazily as the walk revisits nodes. Three uses:
1. walk-level PAIR SUPPRESSION (the phase-3 SEEN analog): both sides
   connected + same root -> prune the pair, any level, no descent. A
   connected node's SELF pair prunes the same way.
2. single-witness early exit in leafLeafUnion when both leaves are
   connected fragments (phase 3's uniform-leaf shortcut).
3. self-pairs-first ordering in phaseA (local assembly populates the memo
   before cross pairs consult it).
Negative-memo experiments both LOST: an exact-epoch negative cache cost
+140% on subcritical uniform (map churn > the cheap checks it avoided) and
a backoff cache blocked fresh suppressions (1.5x at b0.8). Failed checks
are cheap by construction; memoize positives only.

RESULTS (8M +p7, quiet machine; phaseA seconds):

| input            | pre-cert | cert-only | +suppression |
|------------------|----------|-----------|--------------|
| Plummer b0.8     | 12.08    | 10.5-10.9 | **1.4-2.0 (~7x)** |
| LAMBS-1M b0.2    | 0.20-0.26| 0.20      | 0.10-0.12 (~1.8x) |
| Plummer b0.2     | 0.88-0.96| 0.94-0.96 | 1.11 (+~20%) |
| uniform b0.2     | 0.13-0.16| 0.16-0.17 | 0.23 (+~45%) |

3-6.5M pairs suppressed per PE at b0.8. AND THE SKEW COLLAPSES: b0.8
phaseA_s min/avg/max went 1.37/6.8/12.1 (max/min 8.8) -> 1.0/1.3/2.0
(max/min 2.0) — the hot PE's excess WAS the redundant re-proving in its
dense region, so suppression attacks the scaling problem from the work
side, complementing (and reducing the need for) placement fixes. The
subcritical overhead (+20-45% of a small number) is the price of the
per-pair connectivity checks; acceptable given production data is
clustered, but a future tuning knob if it ever matters.

Correctness: 12-run matrix + 1M b0.2 (333,889) + b0.8 (41,315) + LAMBS 1M
(379,884) grid-verified + PBC (98,264) after every variant, and the 8M
b0.8 stats line is bit-identical across cert-only and suppression builds.

MEASURED for certificates alone (laptop; quiet machine, interleaved A/B):
correctness everywhere
(12-run matrix, 1M b0.2 333,889 / b0.8 41,315 grid-verified, LAMBS 1M
379,884 grid-verified, PBC runs unchanged). Performance: PARITY at
laptop-reachable densities — certificates fire heavily (~300k fragments/PE
at 8M Plummer b0.8; ~150k at LAMBS-1M b0.2) but the certified interior was
not the dominant cost at these scales; the remaining time is the SHELL
(mindist <= b < maxdist pairs needing real tests) plus mean-density
genuine-neighbor work. The 1M LAMBS SUBSAMPLE dilutes real halo density
~80x in b-units, so the deep-overdensity regime (b spanning 4-20 local
spacings, design note §4 case 2's target) is NOT reachable on the laptop.
The structural claim (near-linear certified interior) is landed and
correct; whether it pays at production density is an ANVIL measurement —
the phase1_stages line in the sweep output decides it, on the same runs
that decide the density-skew question. The skew fixes (density-weighted
placement / work sharing) remain the primary phase-1 lever regardless:
certificates cut the hot PE's work only where it is deeply overdense, not
where it is merely dense.
