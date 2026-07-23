# Phase-1 scaling: the flattening is phaseA density skew

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

MEASURED (laptop; quiet machine, interleaved A/B): correctness everywhere
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
