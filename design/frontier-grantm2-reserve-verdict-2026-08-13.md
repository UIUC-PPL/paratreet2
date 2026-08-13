# Frontier report: the GRANT_M2 strangle + reservation verdict at 2B (relayed 2026-08-13 morning)

Provenance: Frontier session, paratreet2 0f30988, 2B/16 nodes, jobs
5253386 (GRANT_M2 ladder, 6 runs) and 5253475 (best-cell head-to-head,
4 runs), 21 2B runs total plus a 4-arm 10k gate — all exact
(424897832 / 3549). Relayed by Kale as frontier-inbox/temp2.txt;
stored verbatim below. Acted on same morning: defaults changed at
1996ebd (GRANT_M2 1e10, GRANT_UNITS_PER_PE 128, RESERVE off).

---

The diagnosis is arithmetic, not speculation:

- Pool: tot_m2 = 6.201e12 over phaseB_units total = 2,321,034
  -> mean = 2.67e6 m2/unit
- FOF_S3_GRANT_M2 default = 5e7 (fof/FoFPhase1.h:508-511)
  -> the m2 budget exhausts after ~18 average units
- Count cap = FOF_S3_GRANT_UNITS_PER_PE(32) x CkNodeSize(14) = 448 units
  (fof/FoFPhase1.h:472-479)
- Both are tested per unit in the grant collection loop (lines 811, 829);
  whichever binds first ends the grant.
- So the m2 budget is ~25x tighter than the count cap. The count cap is
  dead code at 2B.

Cross-check: out_m2 / out_ships = 7.0e7 (reserve) and 1.05e8 (noreserve)
-- one budget's worth per shipment, i.e. essentially every grant terminates
on the m2 budget. (Both overshoot 5e7 because the budget is tested BEFORE
the unit is added, and the first unit always ships.)

Isolation: the s3-noreserve arm is functionally "v2 + hybrid grant + reader
fix". It ships 5.6 units/grant vs v2's 206-224. So the collapse is NOT
caused by reservation -- it was already there, introduced by 0963ded.
Section 24 proposed FOF_S3_MAX_GRANT_M2 ~ 1e8; the shipped default is half
that, and both are far below what this pool needs.

aba7833 (reader windowed flush) is exonerated: this build's BASE arm
(Pre-trav 5.30-5.34 / Iter0 8.05 / phaseB 3.21) matches the pre-v2 unsliced
base 5249400 (5.32 / 8.01 / 3.30). Only the S3 benefit vanished, not
baseline throughput.

CONFIRMED by the FOF_S3_GRANT_M2 ladder (job 5253386, 6 runs, all exact).
noreserve @ GRANT_M2=1e11 (budget effectively off, count cap binds)
reproduces v2 (job 5250364) on all five metrics:

  metric            v2 b210b6f 5250364    noreserve @1e11, 0f30988
  units per ship    206-224               220.1
  phaseB            2.08-2.18             2.067
  phaseB_s max      1.983-2.113           2.029
  Pre-traversal     4.47-4.53             4.581
  Iteration 0       6.98-7.24             7.303

FOF_S3_GRANT_M2 = 5e7 is the ENTIRE regression. Nothing else between
b210b6f and 0f30988 costs anything.

Reserve-arm ladder (phaseB_s max, 0.25 s floor):
  5e7   -> 3.144   (units/ship 10.4)
  2e8   -> 3.153   (24.0)
  1e9   -> 3.188   (69.9)
  5e9   -> 2.748   (176.1)
  1e11  -> 2.225   (307.2)

Reservation head-to-head at matched budget (phaseB_s max):
  5e7   reserve 3.144  vs noreserve 3.165   tie (both strangled)
  1e9   reserve 3.188  vs noreserve 3.232   -0.04 s, noise
  1e11  reserve 2.225  vs noreserve 2.029   noreserve WINS by 0.196 s
                                            (reservation 9.7% WORSE)

Once grants can actually be sized, reservation loses on every axis at once:
fewer units moved (84,182 vs 93,542), less work moved (24.7% vs 34.7% of
pool m2), worse wall-clock (Pre-trav 5.093 vs 4.581), and 6.6x the
returned-edge traffic (20,441 vs 3,106) for the donor to re-dedup.

CLOSING RUN (job 5253475, 4 runs, all exact): best cell GRANT_UNITS_PER_PE=128
+ PB_PARTS=16 with GRANT_M2=1e11. Reservation is not marginally worse here --
it is severe.

  metric            noreserve (mean)   reserve (mean)   cost
  Pre-traversal     4.453              8.225            +85%
  Iteration 0       7.160              10.999           +54%
  phaseB            1.712              3.148            +84%
  phaseB_s max      1.605              2.159            +35%
  out_m2/tot_m2     38.0%              22.2%            -42% work moved
  ret_edges         3,066              20,509           6.7x

The bigger the grants, the more damage reservation does -- 66-79% of shipped
units come from the cursor window at this cell. Reservation ships LARGER
grants (861-946 units vs 551-566) carrying LESS work (22.2% vs 38.0% of pool
m2): grants inflated with average-cost units. That is the composition
inversion showing up in wall-clock rather than histograms.

Section 6's best cell reproduces at 0f30988 once the budget is lifted:
  metric          5250425 grant128-parts16 (b210b6f)   best-noreserve @0f30988
  phaseB          1.698                                1.681 / 1.742
  phaseB_s max    1.529                                1.572 / 1.637
  units/ship      531.2                                566.4 / 550.9
  Pre-traversal   4.584                                4.422 / 4.483
  Iteration 0     7.277                                7.138 / 7.182

BEST CONFIG FOUND: GRANT_UNITS_PER_PE=128, PB_PARTS=16, GRANT_M2 lifted,
FOF_S3_RESERVE=0 -> phaseB_s max 1.572 s (6.3x the 0.25 s floor, vs 12.6x for
the campaign as specified) and Iteration 0 7.138 s, the best of the campaign.

Final ranking, phaseB_s max (0.25 s floor):
  1.572  GRANT=128 PARTS=16 M2=1e11, no reserve      <- best
  1.529  GRANT=128 PARTS=16, b210b6f (5250425)*
  1.838  GRANT=64 PARTS=16, b210b6f (5250425)
  2.029  GRANT=32 PARTS=32 M2=1e11, no reserve
  1.983-2.113  v2 b210b6f (5250364)
  2.159  GRANT=128 PARTS=16 M2=1e11, WITH reserve
  3.148-3.221  unsliced base (5249400)
  3.144  the campaign as specified (reserve, M2=5e7 default)   <- 2nd worst
  * same config as row 1, measured either side of the m2 budget; read as 1.53-1.64

Recommendation: raise FOF_S3_GRANT_M2 default to 5e9-1e10 (or express it
relative to measured pool m2 rather than as a bare constant), adopt
GRANT_UNITS_PER_PE=128 + PB_PARTS=16, and default FOF_S3_RESERVE=0 pending
redesign. Section 27's premise needs revisiting: if reservation is retried,
the target should be DISPLACING the costliest-partition path rather than
pre-empting it -- the current code drains the cursor window BEFORE `range`,
which is exactly what trades 1.89e7 m2/unit for 6.75e6.

All 21 2B runs today exact at 424,897,832 (plus a 4-arm 10k gate at 3549),
so reservation is correct -- it is a performance verdict, not a correctness one.
