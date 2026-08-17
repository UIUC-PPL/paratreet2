# Where the phase-1 idle actually is, and why every attempt has been small
# Frontier, 2026-08-16. From job 5286573 counters + Kale's
# `timeProfile-2b-proto3-sumd-08-16.png`. 2B / 16 nodes, 128 processes,
# 14 PEs each, 1792 PEs.

Kale: the time profile shows ≥50% idle in phase 1, that idle is what motivated
the S3 campaign, and every attempt to recover it beyond the original
within-process phaseB balancing has failed. This is an attempt to say exactly
why, from the counters, and to separate what is closed from what is not.

## 1. The picture and the counters agree, to the decimal

The profile shows phase 1 at 100% for ~1 s, then a ~2 s tail at 5–15%
utilisation. The `load_model:` lines give the same thing as numbers:

| | total work | if spread over 1792 PEs | what actually sets the stage | efficiency |
|---|---|---|---|---|
| phaseA | 2061.0 PE-s | **1.150 s** | max `pa_max_s` = **2.016 s** | 57% |
| phaseB | 266.7 PE-s | **0.149 s** | max `pb_max_s` = **1.193 s** | 12% |
| phase 1 | 2327.7 PE-s | **1.300 s** | wall = **3.25 s** | 40% |

The 12% is the tail height in the picture. The blue `startPhase1Chain` block
integrates to ~1.15 s-equivalent, which is exactly the phaseA total — so
during the blue period the machine really is busy, and essentially all of the
idle is after it.

**The prize is 1.95 s of a 3.25 s phase 1 — phase 1 could be 2.5x faster.**
It splits almost evenly:

- phaseA imbalance: 2.016 − 1.150 = **0.87 s**
- phaseB imbalance: 1.193 − 0.149 = **1.04 s**

The campaign has spent itself entirely on the second one.

## 2. The two phases have DIFFERENT SHAPES, and that is the whole story

Per-process critical paths, sorted:

```
top-10 pa_max_s : 2.02 2.01 1.92 1.90 1.87 1.82 1.79 1.78 1.76 1.76   median 1.20
top-10 pb_max_s : 1.19 0.76 0.69 0.59 0.58 0.52 0.43 0.42 0.38 0.38   median 0.14
```

**phaseB is an OUTLIER problem.** #1 is 1.57x #2 and 8.5x the median; only 6 of
128 processes exceed 0.5 s. Removing the worst process takes the stage from
1.19 to 0.76 — a 36% cut. Targeted shedding is the right tool for this shape,
and it works: job 5286357's best arm measured exactly that (phaseB stage
1.269 → 0.822).

**phaseA is a PLATEAU problem.** The top ten are 2.02 … 1.76 — a flat shoulder,
not a peak. **Removing the single worst process gains 10 ms.** Removing the top
five gains 200 ms. Nineteen processes sit above 1.5 s. No targeted method can
touch this shape; you would have to level twenty processes at once.

That single contrast explains the campaign's results better than anything else:
every mechanism built so far is either an outlier tool (shedding) or a
phaseB-only tool (S3 stealing), and after the phaseB outlier is fixed the
binding constraint becomes a phaseA plateau that neither tool addresses.

Note the order of magnitude that follows: with a perfect shed of phaseB's #1,
the phase-1 critical path goes from 2.02 + 1.19 to 2.02 + 0.76 — the phaseA
term is untouched and now dominates by 2.7x.

## 3. Why the top-1 interventions were always going to be small

The worst process holds **5.4%** of all phaseB work. Cumulative shares:

| top-1 | top-5 | top-15 | top-25 |
|---|---|---|---|
| 5.4% | 18.0% | 35.5% | 48.8% |

Half the phaseB work lives outside the top 25 processes. Lowering the maximum
is worth doing because the maximum sets the phase, but it moves a few percent
of the work, and what it buys is bounded by wherever #2 happens to sit.

Two further discounts, both measured today, apply on top:
- **The 2:1 wall discount.** phaseA and phaseB are not barrier-separated, so a
  phaseB gain is roughly halved by the time it reaches the phase-1 wall
  (447 ms of phaseB stage → 233 ms of phase-1 wall, job 5286357 addendum A1).
- **The migration price**, 59 ms + 142 ms per million particles (addendum C3).

Multiply: a −36% phaseB-stage win becomes ~−0.2 s of phase-1 wall, less
~0.12 s of migration. That is the −49 ms net that the best ranked arm measured,
and it is not a failure of execution — it is what the arithmetic allows.

## 4. The structural blocker the campaign has hit three times

Every work-extraction mechanism built so far **taxes the CPU of the process on
the critical path**:

- S3 within-block: the donor packs the grant.
- S3 cross-node: the donor packs LARGER grants — 2.07x larger, 11.0 vs
  10.3 ms/call — which is precisely why it lost 3.5% (relay9). "Every PE that
  packs is a PE not walking."
- Targeted shedding: the victim pups and ships the pieces, 59 ms +
  142 ms/Mparticle, and it is the victim's process that pays it.

The straggler is asked to fund its own rescue. That is the recurring reason
these attempts land near zero, and it is a property of the mechanisms, not of
the machine.

## 5. The one mechanism that does NOT tax the donor — and today priced it

**Phase 3 is receiver-pull.** It is the CacheManager walk: a process that wants
remote work fetches subtrees through the cache. The owner serves cache
requests but does not package work units.

Job 5286357 put a number on its spare capacity, by accident:
**10.7% of the machine's entire phase-1 pair work was pushed across a process
boundary into phase 3, and phase 3 did not move above its own run-to-run
range** (total 0.884 s against a base range of 0.873–0.892; `phase3_walk`
0.278 → 0.289, i.e. +0.011 s bought −0.447 s of phaseB stage).

Scaling that rate: bringing the top ~25 processes down to the median means
moving roughly a third of phaseB work, ~5.2e10 of the machine's 1.517e11
`m2_cross`. At the measured rate that is on the order of **+15 ms of phase 3**
against **up to ~1 s of phase-1 tail**.

**The follow-up I would argue for is therefore: at the end of phaseB, hand a
straggler's REMAINING pairs to phase 3 in place, instead of executing them
locally or shipping them.** It is the only lever measured today that is big
enough to matter, and it is the only one that does not bill the straggler.

Three honest caveats:
1. The +10.7% measurement was a conversion **by migration** — the piece's data
   moved once and its pairs were then walked locally on the destination.
   In-place conversion means the walk pulls remote subtrees through the cache,
   which is different traffic and may miss in the cache. The measured cost is
   therefore a LOWER bound. This needs its own A/B before anyone believes the
   15 ms.
2. **Half the work cannot be moved this way at all.** Machine-wide,
   `m2_self` = 3.708e11 of 7.543e11 total pair work — 49% is self-pairs inside
   a single piece, which are tied to that piece and can only be relocated by
   migrating it. The offload idea can address `m2_intra + m2_cross` = 51%.
3. It attacks phaseA only insofar as phaseA's intra-PE pairs (31% of the total)
   are movable. The self-pairs half of phaseA is reachable only by migration.

## 6. What I would close, and what I would not

**Close — measured, small, and understood:**
- Shedding parameter tuning (k, rank mode, destination count). Ceiling is ~5%
  of phaseB work; best measured net is −49 ms of a 6.3 s Iteration 0.
- S3 cross-node stealing. Measured +3.5%, mechanism understood (donor packing).
- Compiler flags, SIMD, per-piece arena, global LB. All previously closed at
  ~0 on phaseB.

**Keep open, small but generalisable:**
- The phase-1 alignment anomaly (addendum C5): small k with concentrated work
  makes the phase-1 wall WORSE by 200–355 ms while every stage counter
  improves. Job 5287075 is tracing it. It is the same size as everything else,
  but it is the only measured effect nobody can currently explain, so
  understanding it may generalise.

**Keep open, and the only thing sized like the prize:**
- Phase-3 offload of straggler pair work, per section 5.
- **phaseA's 19-process plateau, 0.87 s, which nothing in the campaign has ever
  attacked.** It is not an outlier problem, so it needs a broad mechanism —
  which is the same conclusion section 5 reaches from the other direction.
