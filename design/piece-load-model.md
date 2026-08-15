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

### v2 refinements (Kale, 2026-08-14 evening) — scope, conservatism, and
### the shadow copy

**1. DROP the physical-node restriction.** The v2 sketch above confined
destinations to the S3 block. The bandwidth evidence does not justify it:
relay1 item 12 measured LCI's intra-node path BEATING the shmem build
over most of the range, and found a 4.1x cliff between 16 and 32 MB in
BOTH builds — i.e. intra-node is not a specially fast path, and the sizes
we move sit past the cliff either way. So choose destinations by the two
things that actually matter — the m2 locality cost and the destination's
own load — and let them fall on whatever node they fall on.

**2. AIM AT THE PEAKS, NOT AT BALANCE.** Migration is not free: job
19932506's global pass cost 2.4-2.9 s, and a piece is ~n *
sizeof(Particle) ~ 2.9 MB at 29k particles. Moving 10-20 pieces is
~30-60 MB and ~0.1-0.2 s from one sender; moving hundreds is not
affordable. So the objective is explicitly PARTIAL: shave the top of the
distribution, accept the rest.
- Act only on the worst one or few processes (max/mean above a factor,
  with an absolute floor so a fast phase never churns).
- Cap BOTH the piece count and the bytes moved per process, and log both.
- Target something like a 30-50% cut in the peak, not the mean. On
  19932506's numbers that is 8.08 s -> ~4-5.5 s of phaseB on the
  straggler for ~0.1-0.2 s of migration: a good trade that does NOT
  require the model to be accurate, only to rank.
- Concentration is on our side: the cost probe found the top 1% of pairs
  hold 60-79.5% of the time, so a handful of well-chosen pieces should
  carry a disproportionate share of the excess.

**3. THE SHADOW COPY, and one correction to its premise.** Kale: since we
do not modify the piece, leave an inactive copy behind rather than
migrating it back.

The premise needs one amendment: phase 1 DOES write to the particles —
`group_number` (the tip/label) at three sites in FoFPhase1.h (2102, 2167,
2407), which phase 3 then reads. The tree structure and every other
particle field are untouched, and `vertex_id` is written only by the
union-find setup in Node.h, not by FoF's phases.

That makes the idea better rather than worse: the return trip is not
free, but it is LABELS ONLY — 8 bytes/particle against ~100 for a full
piece, so ~232 KB instead of ~2.9 MB per piece, a ~12x saving on the way
back. Shape:
- forward: full piece to the destination (unavoidable — it must be
  walked there);
- back: the group_number array, indexed by the piece's particle order;
- the origin keeps its untouched copy, so the HOST's distribution is
  never disturbed and no second migration is needed.

Note what this becomes: S3 stealing at PIECE granularity, decided before
phase 1 instead of during phaseB. That is a favourable trade on traffic —
S3 ships ~73 KB per stolen UNIT and ~11 GB per run because the same
subtree ships repeatedly across many units, whereas a piece ships once
and serves all of its pairs. The open question this design must answer is
the one m2 prices: the pairs the moved piece has with pieces left behind
become phase-3 cache-walk work, so the saving is real only when the piece
is chosen where that conversion is cheap.

IMPLEMENTATION NOTE: a shadow copy is NOT chare migration — a chare array
element lives in one place. So this is a data-plane change (ship the
piece's particles + tree to a helper process, walk there, return labels),
which reuses the S3 transport rather than the LB machinery. The
PARATREET_PREBUILD_LB migration path stays useful as the simpler variant
to measure first, since it needs no new protocol.

### Location management under migration — corrected (Kale, 2026-08-14)

I had flagged migration + htram as a possible CORRECTNESS risk. Kale's
correction, and it is right: Charm++ routes to a migrated array element
through its HOME (a distributed hash of the index), which tracks the
current location and forwards; the sender is then told the new location
and installs it. So a PE sending many messages to one chare pays the
extra hop only on the first few, until the correction arrives. The
impact of migration on message delivery is PERFORMANCE, not correctness.

Checked the one place that could still have held a private map —
`UnionFindLib::boss_send` picking an htram destination PE
(unionfind/unionFindLib.C:130):
- it checks `ckLocal()` FIRST, with a comment that says explicitly this
  "avoids using a stale lastKnown cache entry if the element has
  migrated to this PE";
- otherwise it asks `arr->lastKnown(idx)` — Charm++'s own location
  manager — rather than computing a PE from a static map.
So the aggregation path defers to the runtime's tracker and is already
migration-aware. It is also a DIFFERENT chare array from TreePiece, so
shedding TreePieces does not relocate union-find elements at all.

Latent issue noted in passing, not triggered by this work: when
`lastKnown` returns -1 that function calls `CkAbort("Location not
found")`, and the recovery it clearly intended (`homePe` +
`requestLocation`) sits AFTER the abort as dead code.

Consequence for the design: the routing penalty of shedding a few pieces
is bounded by the number of (sender, moved-piece) PAIRS, not by message
volume — small, and it does not scale with the walk's traffic. It also
means job 19932506's 5-7x regression was NOT forwarding overhead: it was
locality destruction (phase-1 pairs becoming phase-3 walks), the 2.4-2.9 s
LB pass itself, and the phaseA imbalance from 0-2012 pieces/process.

### The map-consistency mechanism (Kale, 2026-08-14) — option 2 is
### directly supported, no charm change needed

Kale: Charm++ supports a mode for applications that need a map with no
lastKnown misses — centralized LB broadcasts the whole chare->PE map at
the end, and PEs then refrain from migrating outside it. Two ways to fit
targeted shedding into that: (1) write a centralized LB that performs
only our few migrations but broadcasts the new map, or (2) do our own
small round of migrations and then PATCH every local location map.

Checked against this charm build (~/software/clusterFinding/charm) —
option 2 has first-class support:

- `CkLocCache` is a GROUP with an **entry method**, declared
  `[expedited]` in `src/ck-core/CkLocation.ci`:
      entry [expedited] void updateLocation(const CkLocEntry& newEntry);
- `CkLocEntry` (cklocation.h:75) is `{CmiUInt8 id, int pe, int epoch}`,
  16 bytes and `PUPbytes` — so a patch for one element is 16 bytes and a
  whole shedding round is a few hundred.
- The receiver already does the right thing (cklocation.C:2371):
      if (newEntry.epoch > oldEntry.epoch) { oldEntry = newEntry;
                                             notifyListeners(...); }
  The EPOCH guard is what makes patching safe against races with the
  ordinary home-based updates: a stale patch cannot overwrite a newer
  entry, and listeners are notified on a real change.
- Reaching it from the app: the array's location manager holds the group
  id — `CkLocMgr::getLocationCache()` (cklocation.h:497), with the cache
  created alongside the loc mgr in `ckarray.C:657`.

So the plan is: perform the few chosen migrations, then broadcast one
`updateLocation` per moved piece to the `CkLocCache` group with an epoch
above the element's current one. Every PE's map is then correct without
a single home lookup, which is exactly the no-miss mode, at a cost of
~16 bytes x pieces-moved x PEs.

Option 1 (a bespoke centralized LB) is heavier for this purpose: LB
strategies live in charm's `src/ck-ldb`, so it means modifying and
rebuilding charm — and this campaign's charm is PINNED (3d1fdd89f) as
the provenance for every result. Worth revisiting only if we later want
shedding to compose with a host application's real LB, where being a
proper strategy is the honest integration point.

CAVEAT to verify when implementing: the epoch to use. It must exceed the
element's current epoch, and the migration itself may already bump it
(`recordEmigration` at cklocation.C:2382 updates the local entry). Read
the element's entry after migrating and patch with epoch+1, rather than
inventing a number.

## Outlier structure, measured on BOTH machines (2026-08-15) — and a
## correction to my own shedding estimate

Kale asked whether shedding-from-the-worst is overfitted to 2B, and
whether there is a BIMODALITY that would isolate true outliers. Answered
from the two 128-process load_model datasets:

| | Anvil | Frontier |
|---|---|---|
| max / median | 10.5x | 9.4x |
| gap #1 -> #2 | **1.59x** | **1.57x** |
| robust z of #1 ((x-med)/1.4826 MAD) | **10.4** | **10.0** |
| robust z of #2 | 6.2 | 6.0 |
| processes with z > 5 | 2 | 3 |

The SHAPE reproduces across machines to two digits, so it is not a
2B-on-one-machine artefact. But it is NOT bimodal: below the top two or
three the tail decays smoothly (Frontier 9.63, 8.90, 7.08, 6.75, 6.25,
5.16 ...). What exists is ONE clear outlier — z~10 with a ~1.58x gap
below it on both machines — over a heavy tail.

CRITERION, therefore: a robust-z (median/MAD) test, not a factor of the
mean. z > ~8 selects exactly the one true outlier on both machines,
adapts to whatever distribution appears, and needs no tuning constant
carried between problem sizes. A "2x the mean" rule would have flagged
12 processes on Frontier and is exactly the overfit to avoid.

CORRECTION TO MY EARLIER ESTIMATE. The v2 section above claimed shedding
the straggler's excess across its block-mates would take the max from
8.08 s to ~2.1 s. That was WRONG: it implicitly assumed a uniform
background, when #2 is already at 5.09 (Anvil) / 9.63 (Frontier). The
max cannot fall below #2. The honest ceiling of levelling the top k:

| level top k | Anvil | Frontier |
|---|---|---|
| 1 | -37% | -36% |
| 2 | -56% | -41% |
| 3 | -60% | -53% |
| 5 | -61% | -59% |

So shedding from the single worst process buys ~36-37% of the phaseB
max — reproducibly on two machines. Real and bounded, and still larger
than anything the compiler or layout experiments produced (both ~0 on
phaseB). Going further needs BOTH more migrations and a better ranker,
and relay4 showed the detector ranks #1 correctly but only 2/5 of the
top five — so what is reliably actionable (level 1, maybe 2) matches
what is reliably detectable. That alignment is the argument for the
conservative design, not a limitation of it.

## MEASURED v2 (Frontier relay5, 2026-08-15, 3 reps): the model works as a
## DETECTOR, the trigger must be lower, and the ceiling is -33%

Three repeats at 2B/16, commit abbd0d3, job 5274609. The actual-pairs
model (m2_self / m2_intra / m2_cross) against measured per-process time:

1. **It beats the mean-field proxy, but not on the comparison I posed.**
   Raw m2_cross gives Pearson +0.718 against sqrt(pair)'s +0.866 — which
   reads as WORSE and is not like-for-like, since sqrt(pair) is a tuned
   compression of a raw term that itself only reaches +0.761. Like for
   like: rank correlation (transform-free) m2_cross **+0.948** vs pair
   +0.857; best transform each, m2_cross^0.25 +0.929 vs sqrt(pair)
   +0.866. The +0.948 is the number to carry.
2. **The phaseA sign is FIXED**: -0.260 -> +0.398 Pearson, +0.782
   Spearman. Usable as an ordering, weak as a magnitude. Note m2_self
   ALONE (+0.460) beats m2_self+m2_intra (+0.398) — adding the intra
   term makes it slightly worse.
3. **Ranking, which is what shedding actually needs**: true worst found
   at #1 in 3/3 repeats (sqrt(pair) also managed this). The new model
   wins on the rest of the tip — top-5 overlap 4/5 vs 2/5, Spearman over
   the top 20 +0.59..+0.70 vs +0.42..+0.50, margin over #2 2.10x vs
   1.74x. Input stability: piece->process assignment is identical across
   all 3 repeats; 28 of 128 processes move >10% between repeats but ALL
   of them rank 11th or lower, and node 55 moves 0.35%.
4. **But it is a better DETECTOR, not a better MODEL.** Drop the single
   worst process and refit over the other 127: m2_cross +0.718 -> +0.62,
   sqrt(pair) +0.866 -> +0.80. Among ordinary processes the old proxy is
   still the better linear predictor; m2_cross earns its edge by getting
   the extreme right. Do not quietly reuse it as a general cost estimate.
   It also MISRANKS THE ACTUAL #2 (node 54 is #2 measured, #5 by
   m2_cross), so it cannot say who becomes the new worst after shedding —
   that has to be MEASURED in the A/B, not predicted.

### The trigger: z > 7, not z > 10

My spec quoted z=10.0 and a 1.57x #1->#2 gap. Three repeats give robust
z of 9.18/8.99/8.92 (mean 9.03) and gap 1.513/1.488/1.506 (mean 1.502) —
both ~10% under, consistently, with a repeat spread (0.27 in z) far
smaller than the gap to my quoted values. The earlier figure was a
single draw. **A trigger written as z > 10 would NOT have fired on a run
where the straggler is present, is the same process, and is 8.4x the
median.**
The robust discriminator is the SEPARATION, not the level: #1 at z~9.0
and #2 at z~5.6 in every repeat, and the same separation holds in both
earlier datasets. Any threshold in 6.5-8 separates them in all five
measurements across both machines. **Use z > 7.**

### Ceiling: -33.4%, not -36%

Levelling #1 to #2 on pb_sum_s: -33.9 / -32.8 / -33.6%, mean -33.4%.
(pb_max_s swings -32% to -45% because #2 is a different process in
rep2, so pb_sum_s is the stabler basis.)

### WHY node 55 is the straggler — and why every size-based proxy failed

Node 55 holds **403 pieces against a median of 512**, with the SAME
particle count as everyone else (1.55e7). It is not bigger; it has
FEWER, LARGER pieces, so its pair work is higher. Consistent with
particle count `n` having literally ZERO correlation with phaseA time
(r=+0.000 — every process is within a hair of the same n, by
construction of the decomposition). **Any model that reduces to particle
count cannot work here.** That is the structural reason the n^1.2 self
proxy failed, and it is worth remembering before proposing any future
"balance the counts" scheme.
