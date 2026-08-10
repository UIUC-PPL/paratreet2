# Predicting what a subtree pair costs — probe, queued jobs, how to finish

Written 2026-08-10 on the throwaway branch `cost-model-probe`. If you are
picking this up cold, this file is the whole handover: what is running,
where its output lands, and how to turn it into an answer. Delete the
branch once the study is done — the probe is not meant to ship.

## The question

phaseB's wall is imbalance, not volume: after the node uniformity
annotation it is about 1.3 s at 2B on 16 nodes against roughly 0.07 s of
perfectly balanced work, and it stays flat from 8 to 128 nodes. Nothing
tried so far repairs that DURING the stage — work movement is throttled by
whatever happens to be in the queue when a request arrives. The remaining
lever is placing work before the stage, and that needs a cost predictor.

Two predictors already exist in the tree and neither has ever been checked
against measured time:

- the phaseB pool's LPT key: raw box overlap volume, no densities;
- `unitCost` on the steal branch: `n_a * n_b`, the particle-count product.

## What the probe measures

`FOF_COST_PROBE=1` times every subtree pair in all three populations —
phaseA self pairs, phaseA cross pairs, phaseB pairs — against three
features:

```
m1 = n_a * n_b                    the naive particle-pair count (unitCost)
m2 = rho_a * rho_b * V_int * Vb   expected pairs within the linking length:
                                  densities times the overlap of a's box
                                  grown by b with b's box, times the b-ball
m3 = n_a + n_b                    descent size, a tree-walk proxy
```

Each process fits its own linear model in place — the normal equations are
a 4x4 matrix and a 4-vector, four multiply-adds per pair, nothing stored —
and prints coefficients with R2 as `FOF3COST` lines. Process 0 additionally
keeps up to 50k records per processor and writes them to `FOF_COST_FILE`,
so other model forms can be fitted offline without another run.

Off by default; the probe costs nothing when the variable is unset, which
was verified (identical 1M result, zero output).

## Jobs queued on Anvil (2026-08-10 evening)

```
19772653  cost-80m    4 nodes   80M lambb.00500, 2 reps
          log      $CF/cost-80m-19772653.log
          records  $CF/results-cost80m/records-rep{1,2}.csv
19772491  cost-2b    16 nodes   2B cosmo25cmb, 2 reps, est. start 08-11 09:40
          log      $CF/cost-2b-19772491.log
          records  $CF/results-cost2b/records-rep{1,2}.csv

$CF = /anvil/projects/x-asc050025/x-lkale/software/clusterfinding
binary: $CF/campaign-bin/FoF3.cost, built from this branch
```

The job scripts already summarise: per phase, the median coefficients and
the R2 spread across processes. So the primary result is readable straight
out of the job log — `grep FOF3COST` or just read the `===` blocks.

## Finishing the analysis

```
ssh anvil "cat <the job log>"                    # per-process fits, medians
scp anvil:<records csv> .                        # only if refitting
python3 results/fit-cost-records.py records.csv  # other model forms
```

`fit-cost-records.py` adds what the in-run fit cannot: each feature's
explanatory power alone, a log-log fit (is the cost a power law in this
feature), and how concentrated the time is — the "top 1% / 10% of pairs
hold N% of the time" lines, which say how much of the distribution a
predictor actually has to get right.

## What 1M on a laptop already showed

Small sample, one machine, but the shape is suggestive:

```
=== A_self   111 pairs   mean 2283 us
  m1 alone   R2 0.76      m2 alone R2 0.49      m3 alone R2 0.89
  all three  R2 0.93      log-log slope on m3: 1.36
  top 10% of pairs hold 54% of the time
=== A_cross  3265 pairs  mean 3.9 us
  m1 alone   R2 0.17      m2 alone R2 0.03      m3 alone R2 0.07
  all three  R2 0.17
  top 10% of pairs hold 97% of the time
=== B        850 pairs   mean 6.3 us
  m1 alone   R2 0.17      m2 alone R2 0.54      m3 alone R2 0.14
  all three  R2 0.58
  top 10% of pairs hold 59% of the time
```

Three readings to test at scale:

1. **The particle-count product is a poor predictor of phaseB cost.** It
   explains 17% alone, against 54% for the expected-pairs term, and in the
   joint fit its coefficient is near zero. `unitCost` uses exactly this
   quantity as its refinement threshold.
2. **Self pairs are predictable and cross pairs are not.** 0.93 against
   0.17. If that holds at scale, placement can be planned for self pairs
   and must be adaptive for cross pairs.
3. **The time is in the tail.** For cross pairs, a tenth of them hold 97%
   of the time. A predictor that only ranks the tail correctly would be
   enough; average-case accuracy is not what matters.

## What to do with the answer

If the expected-pairs term holds up at 2B, the immediate uses are:
replace `unitCost`'s threshold, replace the pool's LPT key (which uses
overlap volume WITHOUT densities — the fix may be as small as multiplying
by them), and use it to place work across processes before phaseB rather
than moving it during.

If nothing predicts cross pairs even at scale, that is the more important
result: it says pre-placement cannot work for the population that matters,
and the design has to be adaptive — which would send the next attempt back
towards movement, but informed about why the previous one failed
(design/phaseb-offload.md sections 16-17: the grant rate, not the cost of
moving a unit).
