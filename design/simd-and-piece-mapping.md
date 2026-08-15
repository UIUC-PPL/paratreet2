
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
