# Phase-1 scaling: the flattening is phaseA density skew

## SUPPRESSION AT 80M: CONFIRMED, PLACEMENT DEFERRED (2026-07-24 sweep)

Ritvik's first sweep on merged main (certificates + connectivity
suppression + dual walk), 80M LAMBS, 15 PEs/proc, vs the 2026-07-23
pre-suppression baseline. phaseA wall (slowest PE) and max/avg skew:

| P  | phaseA base | phaseA new | speedup | skew base -> new |
|----|-------------|------------|---------|------------------|
| 1  | 25.70       | 5.96       | 4.3x    | 1.45 -> 1.20     |
| 2  | 13.94       | 3.17       | 4.4x    | 1.57 -> 1.30     |
| 4  | 9.23        | 1.59       | 5.8x    | 2.13 -> 1.36     |
| 8  | 7.34        | 0.98       | 7.5x    | 3.38 -> 1.58     |
| 16 | 4.30        | 0.80       | 5.4x    | 3.74 -> 2.26     |

phase1 total at P=16: 4.998 -> 1.462 s. merge 2-14 ms everywhere.
DECISION per the rule in the section below: skew at P=16 (2.26) is above
the ~1.5 bar, but perfect placement would recover at most max-avg ~= 0.45
s of a ~6.4 s pipeline (~7%) — DENSITY-WEIGHTED PLACEMENT DEFERRED;
revisit only if higher-P runs show phaseA max growing again.

THE FRONTIER MOVED: at P=16, upwardPass 1.94 s + tip_encode 1.67 s = 57%
of algorithmic time (phase1 23%, walk 0.67, uf2 0.46). Both scale ~10x
over 16x; P=1 values (18-19.5 s each) far exceed their nominal work —
suspects: upwardPass's canopy/recvTC path funnels through the Driver on
PE 0 plus a QD settle; tip_encode does per-particle hash lookups over 80M
particles against ~24M fragments. Secondary: phaseB wall ~flat 0.34-0.42 s
across P with ~11x PE skew inside it; relabel 0.23 s. NEXT WORK ITEM:
instrument upwardPass (local fold / canopy+recvTC / QD) and tip_encode
(countFragments / computeTipEncoding / applyTipEncoding) internally, then
optimize what the timers indict. Note: this sweep predates the map-race
fix and the cache: line — pull current main before any P>=32 run.

## NEXT ANVIL SWEEP (merged main, 2026-07-23) — the suppression-at-80M run [FULFILLED by the section above]

Main now carries everything: dual walk BY DEFAULT, phase-1 certificates +
connectivity suppression, htram-on, stage timers. One sweep, same command
as before, NO -w flag needed:

1. `git checkout main && git pull`; unionfind unchanged (keep the
   aggregation-on build); paratreet2: `make clean && make` in src/ AND
   examples/fof3 (headers/.ci changed — no header-dep tracking).
2. Sanity: any small run; the FOF3STAT config line must say `walk dual`.
3. Sweep: INPUT=<lambb.00500> PPN=15 PROCS="1 2 4 8 16" BFACTOR=0.2
   LAUNCH='srun --unbuffered --mpi=pmi2 -n {P} ./FoF3 -f $INPUT -d oct
   -u dist -b $BFACTOR +ppn {PPN}' ./redundancy_sweep.sh
   If the allocation permits, extend PROCS with 32 (and 64): the skew
   trend at higher P is exactly the placement-decision data.

READOUT (vs the 2026-07-23 pre-suppression baseline in the header above):
- CORRECTNESS: fragments/components lines must match that sweep's values
  at the same P, bit for bit.
- THE MEASUREMENT: phase1_stages phaseA and balance phaseA_s min/avg/max
  against baseline max 25.5/13.6/9.2/7.3/4.2 and avg 17.6/8.9/4.3/2.2/1.13
  (P = 1/2/4/8/16). Certificates + suppression should cut the hot-PE max
  (laptop: ~6x off the hot PE's excess at dense b); what survives at real
  80M density is the decision variable.
- Also expect: walk_s ~ the dual numbers (4.0 -> 0.64), uf2 ~0.3-0.9s,
  tip_encode/upwardPass roughly unchanged.

DECISION RULE for the next work item:
- phaseA max/avg collapses to ~1-1.5 => phase-1 skew is solved by
  suppression; the frontier moves to tip_encode + upwardPass (~3.4s at
  P=16) and structural items (FoF module extraction, htram tuning).
- max/avg stays >~2-3 => density-weighted subtree->PE placement (design
  note §7 static cost model, fix direction 1 below) is the next build,
  with intra-process work sharing as its complement.

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

## PhaseB fairness + within-process barriers (sparse-uf2 branch, 2026-07-25)

Two follow-on optimizations agreed after the sparse-uf2 work, both on this
branch (Kale, 2026-07-25). Vetting recorded before implementation.

### 1. Fair phaseB pair division (separate commit)

Current rule: for each PE pair (p, q) of a process, the LOWER PE walks all
subtree pairs spanning the two. Load is triangular: PE i of an N-PE
process walks pairs with N-1-i partner PEs; PE 0 carries N-1 partners,
the last PE none. Ritvik's 80M logs show ~11x phaseB skew inside a
process.

Options vetted:
- "Even/odd of the smaller PE" (walker = smaller PE if its number is
  even, else larger): UNBALANCED. At N=4 the pair counts per PE are
  3,0,2,1 — an odd-numbered PE never wins as the smaller side, so PE 1
  gets nothing and PE 0 keeps its full triangle row.
- (i+j) parity (walker = min if i+j even, else max): balanced pair
  COUNTS (each PE gets ~(N-1)/2 partners) — but pair COST varies with
  boundary density, which parity cannot see.
- Symmetric hash per SUBTREE pair (chosen): the work unit is the
  (sa, sb) subtree pair, not the PE pair. walker = min-or-max PE by one
  bit of a symmetric mix of the two subtree ROOT KEYS (Morton keys:
  stable, cheap, identical on both sides; pointers would work but vary
  run-to-run under ASLR). Each PE pair spans ~64 subtree pairs at the
  default 8 subtrees/PE, so each side gets ~half IN EXPECTATION with
  density mixing — finer-grained balance than any PE-level rule, and it
  is the subtree-level version of Kale's "base it on vertex id"
  suggestion.

Correctness invariants: every unordered subtree pair is examined by both
PEs and walked by exactly one (the hash is symmetric and both sides
compute it identically); the emitted edge SET is unchanged — only the
emitting PE changes. Cross-PE duplicate edges (same tip pair found from
different subtree pairs assigned to different walkers) already occur
under the lower-PE rule and are harmless: FoFPhase1Node::merge unions
are idempotent. Per-PE seen/cert_tip dedup keeps working per walker.

### 2. Within-process-only barriers (separate commit)

runFoFPhase1 currently drives six GLOBAL reductions: reset, register,
phaseA, phaseB, merge, relabel. Dependency audit: after registration
(pe_subtrees frozen), every stage reads and writes only process-local
data — phaseB reads phaseA's frozen tips of its own process's PEs;
merge folds the process's edge buffers; relabel reads the process's
tip_map. Tips are global particle orders taken from particle data, so
NO cross-process traffic exists anywhere in phaseA..relabel.

Change: keep the cheap reset/register global barriers (~1-2 ms
measured); replace the four stage barriers with a per-process chain on
FoFPhase1Node — each PE deposits stage completion (atomic counter);
the last depositor triggers the next stage on the process's PEs (merge
runs inline on the last-depositing PE — exclusive access holds because
all phaseB deposits are in). One global reduction remains, at relabel
end, carrying the per-process stage walls (max-reduced) so the FOF3STAT
phase1_stages line survives with changed semantics: per-stage values
become MAX OVER PROCESSES of process-local walls (no barrier latency),
and phase1 total becomes max over processes of the SUM of stages — a
dense process no longer holds every other process at each stage
boundary.

Expected effect at scale: phaseA's cross-PROCESS skew (max/avg 2.26 at
P=16 post-suppression) stops multiplying with the per-stage barrier
count; processes overlap their stages. Within-process skew is attacked
by item 1.

### Status (2026-07-25): both landed on sparse-uf2

Commit 62fd898 (fairness) and 041c67f (chain), branch pushed. Validated
on classic Converse (fof3 12-run matrix, 1M both b grid-verified, LAMBS
379,884, 8M histogram bit-identical; fof1 exact at 1/2/4 PEs single
process — the 4-PE run exercises both: 37 cross-PE edges, 30 remapped
tips) and on reconverse (fof1 4 PE, fof3 2-proc 10k + LAMBS, 32-proc
x4 all PASS). phaseB is sub-50 ms on the laptop, so the skew effect
itself is an Anvil measurement (balance phaseB_s line).

Trap recorded during validation: `charmrun +pN` WITHOUT `++ppn` on the
SMP build launches N processes x 1 worker PE (default ppn = 1), not one
process with N PEs. fof1 compares process-level phase-1 tips against a
global serial FoF and is therefore single-process-only; run it as
`+pN ++ppn N`. It now aborts with a clear message otherwise. (The
apparent "+p2 phase-1 bug" chased on 2026-07-25 was exactly this
configuration, reproducible unchanged back to phase 1's creation
commit.)

## Per-chare grid phaseA (branch phase1-grid, 2026-07-25)

Kale's cell idea, refined in discussion: cell side c = b/sqrt(6) gives
two TEST-FREE union guarantees — every same-cell pair is within b
(diagonal b/sqrt(2)) and every pair in face-adjacent cells is within b
(max separation exactly b) — so a dense chare's self pair is solved by:
one pass unioning each cell into a clique through its first-seen
representative, a face-adjacency pass unioning occupied neighbor cells
rep-to-rep, and residual pair tests only across the remaining stencil
(offsets with sum(((|d|-1)+)^2) <= 6, first-witness exit, skipped when
the components already match). Per-CHARE (a PE's chares are not a dense
cube; a chare root is a tight oct box and provides the density gate),
gated on occupancy = n * c^3 / volume at the chare root. Kale's
original bit-only variant (prove the whole cube face-connected, then
assign one parent in a second particle loop) is the special case that
skips particle lists when it fires; the representative version subsumes
it gracefully (per-cluster collapse, residual skipped via
find-equality) at ~zero extra cost. PBC-safe: euclidean guarantees only
shrink under minimum image; residual tests use periodicDistSq; chares
spanning half the box fall back to the walk.

CORRECTNESS (laptop, complete): forced onto every chare (-G 0.0001) —
fof1 phase-1-exact at 4 PEs; fof3 full checks 10k / 1M b0.2 / 1M b0.8 /
LAMBS 1M / PBC 100k-uniform b0.8 all PASS with identical counts; 8M
b0.8 stats identical (332,466) grid-on vs grid-off — a new determinism
pair for that config.

PERFORMANCE (laptop): parity to slightly WORSE everywhere reachable —
8M Plummer b0.8 interleaved repeats: walk 1.07-1.12 avg vs grid(-G 4)
1.15-1.22; 1M b2.0 parity. Cause: at laptop-reachable occupancy (~1-4
particles per cell) the cliques are thin, face unions rare, and the
~160-offset residual stencil costs more than the certificate+
suppression walk, which is already near-linear here. The payoff regime
— cliques of many particles, face-clusters spanning halo cores — needs
1000x+ overdensity at production b. CORRECTION (Kale, 2026-07-25): the
old "1M subsample dilutes halo density ~80x in b-units" claim is WRONG
— under uniform subsampling with b scaled to mean separation, neighbor
counts within b and cell occupancies are INVARIANT (rho*b^3 is
scale-free). What actually differs at 80M: (1) cusp/substructure
resolution — occupancy MEANS are invariant but the TAIL grows, since
4.3x smaller cells/chares sit deeper in rho ~ r^-gamma cusps
(overdensity ~4.3^gamma higher at chare scale); (2) lambb.00500 is a
LATER snapshot than our lambs.00200 subsample — more evolved, denser
in b-units (confirm provenance with Ritvik); (3) empirically,
certificates+suppression gained 4.3-7.5x at 80M vs ~1.8x on 1M LAMBS —
under exact invariance those would match, so the tail/snapshot effects
are real. The grid A/B expectation is therefore UNCERTAIN, not
confident: it wins only if those effects push chare-scale occupancy
past the gate.

ANVIL A/B RESULT (2026-07-26, 12 runs at 80M P=8x15, run by Kale's
Anvil session): CORRECTNESS 12/12 — every components line byte-identical
to the reference. GRID WINS AT -G 4: phaseA max median 0.912 -> 0.816
(~10.5%), with min unchanged (~0.38) and avg ~1% — TAIL-ONLY
compression, exactly the density-gated mechanism predicted. G0
reproduced the prior baseline to the millisecond (0.912 vs 0.911).
Threshold ordering G4 < G2 < G0 ~= G16 (G2 fires on sparse chares
where the residual stencil costs; G16 rarely fires). Secondary:
component_histogram 5.2 s -> 0.23-0.28 s (~21x), confirming the
distributed histogram at scale. Observed uf2 spikes (0.27-1.0 s in 3
of 7 grid-on runs vs 0.032 s typical) are NOT a grid effect: htram-on
uf2 variance of the same magnitude (0.44-0.55 s) is on record from the
07-23 sweep, pre-grid — the htram quiesce loop's QD/flush alignment is
timing-sensitive and the grid merely perturbs upstream timing;
AGGREGATION-off would remove it.

DECISION (2026-07-26, revised after Kale's skew question): DEFAULT
STAYS OFF; -G is documented as an OPTION to A/B per dataset (start at
4), not a recommendation — the win is tail-only and regime-specific
(the SAME threshold measured slightly WORSE on laptop dense Plummer
b0.8), and the skew decomposition shows the grid is mostly SKEW
COMPRESSION, not work reduction: avg moved only 1.9% (0.640->0.628)
while max moved 10.5%; max/avg 1.43 -> 1.30. That reprioritizes STEP
2: perfect intra-process stealing bounds phaseA at ~avg REGARDLESS of
the tail's cause — ceiling ~30% at G0 (0.912->0.64), ~3x the grid's
realized gain, threshold-free and dataset-agnostic — and even after
-G 4 there remains 0.19 s of residual skew the grid cannot recover.
Stealing therefore SUBSUMES most of the grid's benefit and is the
right next phaseA investment when one is warranted; realistic yield
is between half and all of the ceiling (helpers lose per-PE memo
locality). Timing: all of this is ~0.2-0.3 s of a ~3.8 s iteration —
build stealing when phaseA's share grows (the ~2B dataset), not now.
(This A/B is also evidence in the scale-free question above: laptop
parity + 80M tail-win + invariant mean density localizes the
1M-vs-80M difference to the occupancy tail.)
