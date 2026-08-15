# Per-TreePiece FoF load model and pre-build migration (2026-08-14)

Kale's idea 2, implemented at eeb9294. Framing constraint (Kale): FoF is
a GUEST phase inside a cosmology host (ChaNGa-like) whose TreePiece
distribution is already balanced for the host's own phases. So this
MIGRATES pieces for phase 1 rather than choosing the map at creation —
the host owns the decomposition, we borrow the placement.

## The one legal migration window

`TreePiece::pup` ships `incoming_particles`, the proxies, `tp_key` and
`load` — and NOTHING ELSE: not `particles`, not the built tree. So a
TreePiece may migrate only between the flush (or `rebucket`, which
refills `incoming_particles`) and `buildTree`. The stock LB block at the
bottom of Driver's loop already relies on exactly this. What was missing
is that it runs AFTER an iteration, which never helps a run whose
interesting phase is iteration 0 — hence `PARATREET_PREBUILD_LB=1`,
which runs the same `pauseForLB()`/AtSync at the top of the loop.

Everything else was already in place: `usesAtSync` is on in single mode,
`pauseForLB`/`ResumeFromSync` exist, and `UserSetLBLoad` is the hook.

## The model

For a piece with n particles in occupied volume V:

    load = n^SELF_P  +  K * n^2 / V^(4/3)

- **self / phaseA term, n^1.2.** Measured directly: phaseA self-pair
  cost is predictable from piece size at R^2 0.85-0.90 with exponent
  ~1.2 (design/cost-model-probe.md). Sub-quadratic because the -G cell
  grid short-circuits dense self pairs into ~n log n — without the grid
  this term would be nearer n*rho.
- **pair / phaseB term, n^2/V^(4/3).** The validated expected-pairs
  estimate m2 = rho_a rho_b V_int V_ball (R^2 0.87 at 2B), summed over a
  piece's neighbours under a MEAN-FIELD approximation: neighbours of
  similar density contribute rho^2 * A * b * V_ball with A ~ V^(2/3),
  giving n^2 b^4 / V^(4/3). The b^4 is identical for every piece, so it
  folds into K and the model needs NO neighbour communication — each
  piece computes its own load from (n, V) alone.
- Only the RATIO of the two terms is free, hence a single knob
  `PARATREET_LB_K` (default 0 = self term only until calibrated).

Both inputs are available pre-build: n = `incoming_particles.size()`,
V from a bounding box over those particles (O(n), once).

Why a per-piece proxy for a pair term is defensible here: pieces are
SFC-contiguous, so a piece's neighbours are mostly its SFC neighbours
with similar density; the approximation fails exactly at density
gradients, which is where the instrument below should show it.

## The instrument (the gate; prints unconditionally)

    FOF3STAT load_model: node N pieces P n NP self S pair Q
                         pa_sum_s A pa_max_s B pb_sum_s C pb_max_s D

Per process: predicted self and pair sums against ACTUAL phaseA/phaseB
seconds (sum and max over the process's PEs; `depositPhaseBTime` is new,
deposited at the pre-phase3Stats barrier where relay3 established
helper-side numbers become trustworthy). Regress across the 128
processes:
- `pa_sum_s ~ a*self` — does the self term predict phaseA?
- `pb_sum_s ~ c*pair` — does the mean-field pair term predict phaseB?
- R^2 of each is the go/no-go; c/a calibrates K for the migration arms.

## What migration costs, and why it may still win

A migrated piece moves `incoming_particles` — ~n * sizeof(Particle),
about 3 MB for a 30k-particle piece. That is real, but note the
comparison: S3 currently ships ~11 GB per 2B run and ~73 KB per stolen
unit to buy a few percent, and it ships REPEATEDLY within a phase.
Migration moves each piece at most once and all subsequent work follows
it. If the model is good, this replaces stealing rather than
supplementing it — which is why one arm runs LB with S3 OFF.

## Risks

1. **Locality.** Phase 1 is "the complete FoF restricted to a process"
   (FoFPhase3.h): a piece moved away from its spatial neighbours
   converts cheap phaseB pairs into phase-3 cache-walk work. GreedyRefine
   was chosen because it minimises migrations, which bounds the damage,
   but the A/B MUST read phase3/merge/relabel, not just phaseB.
2. **Mean-field pair term** fails at density gradients (see above).
3. **Host disturbance.** In a real host we would migrate back, or let
   the host's own LB restore its balance. Out of scope for the
   measurement; worth stating in any writeup.
4. **GreedyRefineLB uses `setObjTime`** — our model is a relative
   number, not seconds. Fine for a strategy that only compares loads;
   would need calibration for one that reasons about absolute time.

## Status

Implemented and gated both runtimes (classic 10k base/LB/LB+forced, 1M
LB+loopback; reconverse 4-proc LB+forced) — all exact. Migration
verified non-trivial on the laptop: pieces 167/169 -> 214/122 at 1M,
result unchanged. Anvil 7-arm validation job queued: calibration
baseline x2, model-LB x2, old-inverse-volume-proxy LB (controls for
"any LB" vs "this model"), and an LB-without-S3 / base-without-S3 pair
(does a balanced map remove the NEED to steal?).

## A/B design correction (2026-08-14, found while setting up job 19932506)

The intended "did the COST MODEL matter, or would any LB have done?"
control was `PARATREET_LB_MODEL=0` (the historical inverse-bounding-box
proxy). It cannot serve that role in the pre-build window: that proxy
reads `local_root`, which is NULL before the tree is built, so it
assigns no load at all. The old proxy simply has no inputs this early —
that is a fact about it, not a bug in the arm. Read arm 5 of job
19932506 as "LB with an unset load vector", i.e. whether calling the
balancer at all does anything, and nothing stronger.

`PARATREET_LB_MODEL=2` (added at 667686a) is the control the question
wants: uniform load, so the balancer equalises PIECE COUNT. A win for
the model over uniform is then attributable to the model rather than to
the mere fact that a balancer ran. Use it in the follow-up job in place
of mode 0.

Also fixed at 667686a: `printLoadModel` hardcoded the 1.2 exponent while
`UserSetLBLoad` read `PARATREET_LB_SELF_EXP`, so a non-default exponent
would have validated one formula while migrating by another. They agree
at the default, which is what 19932506 runs.

Laptop observation worth carrying into the reading of the results: the
LB's decision is NOT perfectly reproducible run-to-run at 1M (the same
model config gave 214/122 and later 171/165 pieces per process),
presumably because the Charm++ balancer folds measured background load
in with `setObjTime`. On a dedicated allocation this should be tighter,
but it is a reason the job carries two reps of every LB arm.

## MEASURED: Anvil job 19932506 (2B/16, 7 arms, all exact) — mechanism
## works, STRATEGY is catastrophic, and the model as shipped had no signal

| arm | phaseA | phaseB | Iteration 0 | pieces/proc min/avg/max |
|---|---|---|---|---|
| baseline | 1.045 | 0.733 | 4.296 | 396/531/688 |
| baseline-rep2 | 1.030 | 0.653 | 4.258 | 396/531/688 |
| lb-self | 2.241 | 0.445 | **20.265** | **0**/531/1543 |
| lb-self-rep2 | 2.503 | 0.722 | **22.241** | **0**/531/1893 |
| lb-oldproxy | 2.566 | 0.475 | **30.283** | **0**/531/2012 |
| lb-noS3 | 2.525 | 1.581 | **22.227** | **0**/531/1786 |
| base-noS3 | 1.032 | 1.704 | 4.954 | 396/531/688 |

1. **THE STRATEGY IS THE DISASTER, NOT THE MECHANISM.** Migration itself
   is correct (7/7 exact, and the pre-build window held). But
   GreedyRefineLB has no notion of locality, and phase 1 IS locality —
   "the complete FoF restricted to a process" (FoFPhase3.h). It scattered
   pieces from a 1.7x spread (396-688) to 0-2012 per process, leaving
   processes with NO pieces at all, and Iteration 0 went 4.3 s -> 20-30 s
   (5-7x). phaseA doubled; the rest of the damage is tree build + the
   phase-3 walk paying for shattered locality. The LB call itself also
   costs 2.4-2.9 s (68k chares with their particles).
   CONCLUSION: a generic Charm++ strategy cannot be used here. The
   assignment must preserve SFC contiguity by construction — which is
   what design/simd-and-piece-mapping.md proposed and what this job's
   convenience shortcut skipped.
2. **THE MODEL AS SHIPPED WAS NEVER TESTED**, because with K=0 it carries
   no signal: `self` (sum n^1.2) spreads only **1.06x** across the 128
   processes while actual phaseA spreads 1.83x. Oct decomposition
   equalises particle counts, so any near-linear function of n is
   near-constant per process. The three LB arms are statistically
   indistinguishable (phaseA 2.24/2.50/2.57, Iter0 20/22/30), including
   the arm that sets NO load at all — direct confirmation that the
   balancer was not acting on our model.
3. **WHERE THE IMBALANCE ACTUALLY IS**: phaseA spreads 1.83x; phaseB
   spreads **88.8x** (pb_sum_s 0.091-8.082 s). phaseB is the target, and
   phaseB is exactly what the K=0 model ignored.
4. **THE PAIR TERM IS A GOOD PREDICTOR ONCE COMPRESSED.** Against actual
   per-process phaseB, over the 128 baseline processes:

   | form | Pearson r | spread |
   |---|---|---|
   | pair (mean-field, as implemented) | +0.746 | 30208x |
   | **sqrt(pair)** | **+0.872** | 174x |
   | pair^0.25 | +0.862 | 13x |
   | log(pair) | +0.760 | 1.2x |
   | (actual pb) | — | 89x |

   sqrt(pair) is both the best correlated AND the right magnitude
   (174x predicted vs 89x actual, against the raw term's 30000x). The
   mean-field form over-predicts the tail because it assumes every
   neighbour is as dense as the piece itself; the square root is the
   empirical correction. r=0.87 also matches the m2 heritage (R^2 0.87).
5. **THE SELF TERM NEEDS THE GRID IN IT.** A uniform-piece proxy
   P*(N/P)^e correlates with phaseA at r=+0.67 for ANY e>1.2 (with N
   nearly fixed per process the ranking is set by P), and e~2.1
   reproduces the observed 1.83x magnitude. But the EXACT sum
   `sum n_i^1.2` correlates at only -0.187 — WORSE than the crude
   uniform approximation. That gap is piece-size heterogeneity being
   anti-correlated with time, which is the -G grid doing its job: a
   process holding a few big dense pieces sends them down the O(n log n)
   grid path instead of the walk. Any usable self term must branch on
   the same occupancy gate the grid uses, not apply one exponent to
   every piece.

### Revised plan

- DROP the generic-LB shortcut. Build the SFC-contiguous assignment:
  order pieces by key (they already are), partition into 128 contiguous
  runs balancing the calibrated weight, migrate only pieces whose run
  changed. Contiguity bounds the locality damage by construction, and
  the migration count is far below GreedyRefine's.
- Weight for v2: `w = grid_aware_self(n, V) + K * sqrt(pair)`, with the
  pair term dominant (it owns the 89x) and K calibrated from c/a on this
  job's data.
- Keep the instrument as is: it is what produced every number above.

## v2 DESIGN: targeted shedding, not global rebalance (Kale, 2026-08-14)

Kale's correction after 19932506: do not rebalance globally. Identify the
WORST-AFFECTED process from a rough cost model and move just a FEW pieces
off it, each to a process that is spatially close (touching pieces /
nearest centroid, preferably a combination of both).

This repairs the exact defect the measurement exposed. GreedyRefineLB
failed not because balancing is wrong but because a global strategy with
no locality term reassigned everything (0-2012 pieces/process) and paid
5-7x in wall clock. Targeted shedding bounds both quantities that went
wrong: how many pieces move, and how far.

### Why it should be enough

phaseB per process spreads 88.8x (0.091-8.082 s) with a mean of 1.14 s.
Shedding the straggler's excess (~6.9 s) across its 7 physical-node
block-mates raises each by ~1 s and drops the max from 8.08 to ~2.1 —
a ~3.8x cut in the term that sets the phase, without touching anyone
outside the block. Work is also concentrated (the cost probe: top 1% of
pairs hold 60-79.5% of the time), so a handful of pieces should carry
most of the excess.

Cost comparison, which is the argument for doing this at all: a piece is
~n * sizeof(Particle) ~ 3 MB at 29k particles, so shedding tens of pieces
moves ~100 MB ONCE. S3 ships ~11 GB per run, repeatedly, to chase the
same imbalance. Same objective, ~100x less traffic, and the work follows
the data instead of round-tripping.

### The destination rule (quantifying "close")

A piece P moved from process A to process B converts every pair
(P, piece-on-A) from phase-1-local into phase-3 cache-walk work, and
converts every (P, piece-on-B) the other way. Both are priced by the
SAME m2 already used everywhere else. So:

    net locality cost(P -> B) = sum_{q in A} m2(P,q) - sum_{q in B} m2(P,q)

Choose B minimising that (equivalently: the process where most of P's
interaction weight already lives). This is exactly "touching pieces and
nearest centroid" made quantitative — m2 is nonzero only for pieces whose
grown boxes overlap (touching), and falls off with separation (centroid
distance). Restrict B to the physical-node block, the same scope S3
already steals within: migration stays intra-node and the phase-3
conversion stays small.

Special case worth exploiting: pieces are SFC-ordered and processes hold
contiguous runs, so a piece at either END of a run has most of its
neighbours in the adjacent process already. Shedding from the ends is
nearly locality-free and merely moves the run boundary. Prefer end pieces
when their load is comparable; pay the m2 accounting only for hot pieces
buried mid-run.

### Algorithm sketch (per physical-node block, before the tree build)

1. Every process computes its own predicted load from the calibrated
   model (below) and posts it to the block coordinator — the S3
   coordinator already exists and already polls exactly this kind of
   quantity.
2. Coordinator finds max vs block mean. If max < FACTOR * mean (2.0,
   with an absolute floor so a near-idle block never churns), do nothing.
3. Straggler ranks its pieces by predicted load, walks them heaviest
   first, and for each computes the best destination and net locality
   cost by the rule above; accepts the move if it sheds more load than
   it adds phase-3 work (scaled by a measured phase3-per-m2 constant).
4. Stop when the straggler is within FACTOR of the mean or no move
   passes. Migrate with migrateMe in the pre-build window
   (PARATREET_PREBUILD_LB machinery, but our assignment, no
   Charm++ strategy involved).

### Calibrated weight (from 19932506's 128 processes)

    w(piece) = grid_aware_self(n, V) + K * sqrt(pair(n, V))

sqrt(pair) is the empirical winner: r=+0.872 against actual phaseB and a
174x spread against the actual 89x, versus the raw mean-field term's
+0.746 at 30208x. The self term must branch on the -G occupancy gate
rather than use one exponent (see the measured section above). K from the
c/a ratio in the same data.

Note the model only has to RANK pieces and processes here, not predict
seconds — a much weaker requirement than global rebalancing imposed, and
one this model already meets.
