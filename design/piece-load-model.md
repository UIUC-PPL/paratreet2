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
