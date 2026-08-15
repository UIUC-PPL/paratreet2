# Two orthogonal ideas, assessed (Kale, 2026-08-14)

Both grounded in code reading + the campaign's existing measurements.
Neither implemented; this note says what is known, what is guessed,
and what the cheapest discriminating experiment is.

---

# 1. SIMD in phaseA / phaseB

## What the inner loops actually are

| site | shape | trip count | early exit |
|---|---|---|---|
| `leafLeafUnion` (phaseA) | n_a x n_b distance tests | <= 12 x 12 (`max_particles_per_leaf = 12`) | returns on first witness when both sides internally connected |
| `leafLeafEmit` (phaseB) | n_a x n_b tests -> tip edges | <= 12 x 12 | returns immediately if both leaves uniform + same frag; returns after first edge if both uniform |
| `gridSelfUnionRange` cell assign | n divisions + clamps | n (whole dense subtree) | none — branch-free |
| `gridSelfUnionRange` residual | tests between non-connected stencil cells | small | first witness merges |
| `walk` / `mindist2` | per node pair | — | certificates + connectivity suppression |

`periodicDistSq` is ~15 flops (3 sub, 3 round/mul/sub for PBC, 3 mul,
2 add, 1 compare). `Real` is **float** (USE_DOUBLE_FP is not defined
anywhere in the makefiles) — so 8 lanes/AVX2, 16/AVX-512, 4/NEON.

## The three structural facts that decide this

1. **THE COMPILER IS NOT ALLOWED TO USE ANY OF IT.** `fof/Makefile`
   and `examples/fof3/Makefile` pass `-g -O3`, `src/Makefile` passes
   `-g -Ofast` — and NO `-march=`. On x86-64 that is baseline SSE2:
   the auto-vectorizer cannot emit AVX2/AVX-512 on Frontier's or
   Anvil's EPYC. Any loop that would vectorize is capped at 2
   float lanes today. **This is a zero-code-change experiment.**
2. **PARTICLES ARE FAT AoS.** `Particle` is ~96-104 B (mass, density,
   potential, soft, FOUR Vector3D, two Reals, group_number,
   vertex_id, type). The inner loop reads 12 B of position out of
   every ~100 B. A compiler that CAN vectorize still has to gather.
   The enabling change for real SIMD is a **packed SoA position
   array** (x[], y[], z[] — 12 B/particle, per piece, built once),
   after which the inner loops are contiguous-float loops that
   auto-vectorize with no intrinsics.
3. **THE COST MODEL ALREADY ARGUES THE TESTS DOMINATE — FOR phaseB.**
   m2 = expected PAIRS predicts phaseB cost at R^2 = 0.87 at 2B,
   while the particle-count product explains 0.04
   (design/cost-model-probe.md). A predictor that is essentially
   "number of distance tests" explaining 87% of variance is direct
   evidence that phaseB time is test-bound, not traversal-bound.
   And section 34 puts the residual straggler exactly there:
   phaseBChained on the hot process, unchanged by every S3 change.

## Where SIMD will NOT pay: the grid

The grid is the DENSE path (phaseA self pairs only — `grid_ctx_` is
set only around phaseA). But its own hot spots are not distance tests:
most unions are TEST-FREE by construction (same cell, face-adjacent),
and residual tests are skipped when the two cells are already
connected. What remains per grid solve is
`std::sort` of n (key, index) pairs, a binary search (`findOcc`) per
occupied cell per stencil offset (~50 offsets), and the union-find
traffic. Those are sort/search/pointer-chasing costs. The right
levers there are ALGORITHMIC — radix sort on the packed cell key,
and replacing the per-offset binary search with a hash or a direct
index — not SIMD. The one genuinely vectorizable grid loop is cell
assignment (n divisions + clamps, branch-free), which is O(n) against
an O(n log n) sort.

Note also that in a dense core `leafLeafEmit` mostly returns at the
top (both leaves uniform, same fragment), so phaseB's real work is at
FRAGMENT BOUNDARIES, which is what m2 measures.

## Recommendation

Two cheap experiments before any SIMD code, in this order:

1. **`-march=native` (or `-march=znver3` on Frontier/Anvil) rebuild,
   A/B at 2B.** Zero code change. Also try `-Ofast` in fof/ to match
   src/. If some loops already auto-vectorize, this is free; if the
   answer is ~0%, that itself proves the AoS gather is the blocker
   and sizes the case for step 2.
2. **A sampling profile INSIDE the phases** (perf on the cluster, or
   Instruments on the laptop at 16M-80M): we have never once looked
   at where phaseA/phaseB time goes below the entry-method level.
   The split we need is distance tests vs traversal vs sort/search
   vs union-find, separately for the grid path and the walk path.

Only if (2) says tests dominate: add the packed SoA position array
per piece and let the compiler vectorize; write intrinsics only if
the generated code disappoints. Expected ceiling, honestly stated:
if tests are ~50% of phaseB and the packed loop runs 4-6x, phaseB
improves ~1.4x. Worth having, but see part 2 — it is the smaller of
the two ideas.

---

# 2. Per-TreePiece load estimate, and mapping instead of migrating

## Short answer: yes, we already have the estimator, and this is the
## higher-value idea — it targets the whole residual gap.

## What the work decomposition actually is

- **phaseA** = pairs of pieces on the SAME PE (self pairs pass 0,
  cross pairs pass 1) — `phaseABody`.
- **phaseB** = cross-PE piece pairs WITHIN the process, pooled and
  claimed by any PE — `buildPoolSlice` walks `pe_treepieces`.
- **phase3** = everything CROSS-PROCESS, via the CacheManager walk
  against the global tree. FoFPhase3.h states the invariant: "phase 1
  is the complete FoF restricted to a process."

So a piece's home process determines which of its pairs are cheap
(phase 1, local, no communication) and which are expensive (phase 3,
cache fetches). **Moving a piece away from its spatial neighbours
converts phaseB pairs into phase3 work.** Any mapping objective must
carry that, or it will fix phaseB by inflating phase3.

## The estimator exists and is already computed

- **Pair cost**: `m2 = (n_a/V_a) * (n_b/V_b) * V_int(a grown by b, b)
  * V_ball` — FoFPhase1.h ~line 2489. Validated R^2 = 0.87 at 2B as a
  phaseB cost predictor. It needs only each side's BOX and COUNT, so
  it evaluates at piece-root level exactly as it does at unit level.
- **Self cost**: design/cost-model-probe.md — phaseA self pairs are
  predictable at R^2 = 0.85-0.90 from piece size, power ~n^1.2.
- Missing: a common scale. Both are relative predictors; combining
  self + pair terms into one "seconds" number needs two calibration
  constants, obtainable from one instrumented run (we already print
  per-PE phaseA_s / phaseB_s).

## It is a MAPPING problem, not a migration problem

`Driver.h` order is: `assignKeys` -> `findSplitters` ->
`CProxy_TreePiece::ckNew` -> Reader flush. **Splitters are known
before any piece exists and before any particle moves.** A splitter
IS a key range, i.e. an oct box, and the decomposition already
counts particles per range. So both inputs to the cost model
(box, count) are available BEFORE the flush.

Therefore: supply a `CkArrayMap` at `ckNew` that places pieces by the
cost model. The particles are already being sent to their pieces —
sending them to a piece that lives elsewhere costs nothing extra.
No chare migration, no PUP path, no post-hoc rebalancing. This
sidesteps the migration machinery entirely (and the known array-map
race).

## The formulation

Graph partitioning, with a locality term already priced by the model:

- vertex P: weight = self(P) = A * n_P^1.2
- edge (P,Q): weight = m2(P,Q), nonzero only for spatially near pairs
- objective: minimise the maximum over processes of
  [ sum of vertex weights + sum of INTERNAL edge weights ]
  while keeping cut edge weight (which becomes phase3 work) bounded.

The current map is a pure space-filling-curve assignment: it
minimises cut beautifully and ignores balance completely. That is
exactly the failure we measure — proc 55 at 14.9x the median process
(section 31). A first implementation does not need METIS: sort
pieces along the existing SFC (preserving locality), then do LPT /
chains-on-curve bin-packing over the cost model, which keeps
contiguity while equalising load.

## Why this is the higher-value idea

Section 31 measured proc 55's local phaseB exec at 23.9 s against a
1.60 s median process. If the map equalised that, the straggler goes
to roughly the mean — i.e. the phaseB_s max moves from today's
1.23-1.32 s toward the 0.25 s granularity floor. **That is the entire
residual gap (4.9x), which no S3 lever has been able to close.**
And it does so by never shipping anything: S3 currently moves ~11 GB
per run and ~73 KB per stolen unit to buy a few percent. Prevention
strictly dominates cure here, if the prediction is good enough.

S3 remains the safety net for whatever the model mispredicts — which
is the right role for it.

## Risks and open questions

1. **Prediction error.** R^2 0.87 / 0.85-0.90 is good, not exact.
   LPT tolerates error far better than exact packing; and S3 catches
   the residual. Measure predicted-vs-actual per process before
   trusting the map.
2. **The cut/phase3 term is unmeasured.** We know phase3's cost from
   run totals but not its sensitivity to cut weight. A map that
   improves phaseB but doubles phase3 is a loss. The A/B must report
   phase3/merge/relabel, not just phaseB.
3. **Does the estimator hold at piece granularity?** It was validated
   on pool UNITS (subtree pairs). Piece roots are bigger and more
   heterogeneous. Cheap check, offline: instrument one run to dump
   per-piece (box, n) and per-process actual phaseA/phaseB seconds,
   then regress. NO code change beyond a print, and it can ride any
   scheduled run.
4. **Interaction with S3 and with -G.** A balanced map may change
   which nodes pass the grid occupancy gate, and will certainly
   reduce steal volume. Keep S3 armed but expect (and report) fewer
   grants.

## Suggested order

1. Offline validation of the piece-level cost model (a print + a
   regression; no algorithm change). This is the gate.
2. If it holds: SFC-order-preserving LPT map behind a flag, A/B at
   2B reading phaseB_s max, phaseA_s max, phase3/merge, S3 grant
   count, and predicted-vs-actual per process.
3. Only then consider a real graph partitioner.

## PART 1 ANSWERED — measured on Frontier (relay4, 2026-08-15)

The premise was right and the payoff is not where we wanted it.
- The control binary really is SSE2-only: ZERO AVX/VEX instructions,
  confirmed on both machines. `-march=znver3` emits 5540 vector
  references on Frontier (926 on Anvil — the magnitude is
  compiler/machine-specific, do not carry either number across).
- EXACTNESS TRAP, and it is the important find: `-march=znver3` returns
  424897833, one component OVER gold, reproducibly 6/6. gcc defaults to
  `-ffp-contract=fast`; SSE2 has no FMA so the base binary cannot fuse,
  znver3 makes FMA available, and fusing changes the rounding of the
  linking-length test so a boundary pair falls the other way.
  `-ffp-contract=off` restores exactness 6/6 and keeps 5479 of 5540
  vector refs. ANY -march work must carry it.
- The result at 2B, 6 reps: exact-SIMD gives phaseA -3.5% (6/6 paired
  wins) but phaseB +0.1% and Iteration 0 -0.9%. `-Ofast` gives -12.8%
  phaseA and -4.0% Iter0 but is 0/6 exact, so that win belongs to
  -ffast-math, not to vectorisation.

VERDICT: PHASEB — the straggler, the entire target of this campaign —
DOES NOT MOVE under vectorisation. Hand-written SIMD with packed
positions could beat the autovectoriser, but the measured ceiling on the
phase that matters is ~0, so that work is not justified. Part 1 is
closed as a lever, with one cheap loose end worth picking up:
-Ofast's advantage may be mostly `-fno-math-errno` (semantically safe),
which one 12-arm job would separate and which could plausibly recover
much of the 4% Iteration 0 EXACTLY.
