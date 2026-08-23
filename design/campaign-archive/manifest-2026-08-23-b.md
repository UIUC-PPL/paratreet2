# uploads -- 2026-08-23, the -s scaling test and the canopy collect gate

Two studies, both finished, both with every arm exactness-gated.
Nothing was pushed. No traces were taken.

## Read in this order

1. **relay96.txt** -- the discriminating `-s` sweep at 16 and 64 nodes, on 2B
   and on 80M. Jobs 5331703 / 5331704, both COMPLETED, 64 arms, zero gate
   failures. **Both pre-registered models for s* are refuted**, including
   mine. Replaced by a crossover hypothesis, flagged as a post-hoc fit that
   has predicted nothing yet.

2. **relay97.txt** -- item 3, your `canopy-collect-gate` f9faa0f, measured at
   128 nodes. Job 5332618, COMPLETED, all 9 runs EXACT.
   **recvTC 69,670 -> 1,170 exactly as your arithmetic predicted; sort_s
   9.73 -> 0.10 ms; Iter0 -8.3%, G faster 3 of 3 with separated ranges.**
   I under-predicted this at ~3% and say so in section 3.

## Supporting files

- `relay96-ssweep-scale.sbatch` -- the sweep, `DS` selects the dataset, one
  64-node allocation covers both scales via `srun --nodes=`.
- `relay97-collectgate-128n.sbatch` -- the A/B, including the run-time
  arm-identity gate on `raw_canopies`.
- `build-v89.sh` -- builds BOTH arms in one pass at 1040b63, asserts exactly
  one file differs between the refs and that the two binaries differ by md5.

## Three things to look at first

- **relay96 §3.** My fan-in model named cap 128 as the winner at every scale.
  At 64 nodes cap 128 is the *worst* arm measured. Retracted, not repaired.
- **relay97 §3.** The gate is worth 134.8 ms but `loadCache`'s own timer moved
  only 9.7 ms. ~125 ms of the gain is outside the phase being measured, and
  it is 3.6x the PE-0 CPU that relay93's trace attributed to recvTC. I have
  candidates but no measurement. A traced rep of both arms would settle it --
  on your word only.
- **relay97 §4.** The unset controls MU and GU are identical code paths and
  differ by 68.8 ms. That is the 128-node noise floor, measured rather than
  assumed. Single-rep deltas below ~70 ms mean nothing.

## Judgment calls I made, stated so you can overrule them

- **80M has no known gold.** I gated its 32 arms as bitwise-identical to the
  warm-up (31 MATCH + 1 REF). That proves self-consistency across caps, not
  correctness. Send a gold for `lambb.00500` and I will re-gate.
- **80M was run at `-l 32`**, the measured CPU optimum for *2B*. No evidence
  it is 80M's optimum. Its cross-cap A/Bs stand; its absolute times do not.
- **relay97 rebuilt both arms at 1040b63** rather than reusing v88, because
  main moved (9798a06, "avoid unnecessary reads for fof", touches Reader.C).
  So relay97's absolute walls are not comparable to relay93/94/96.
- **Clustering is untested.** I predicted it, size is what appeared, and one
  alternative dataset cannot separate the two. Neither confirmed nor refuted.

## Running now

Job **5333391** `relay98-collectgate-64n` -- the same A/B at 64 nodes, where
relay96 found capping to be a wash. If the collect gate is worth ~130 ms
there too, 64 nodes becomes a capping win and the relay96 crossover moves.
Reported separately when it lands.

## Merge decision waiting on you

`canopy-collect-gate` f9faa0f does what its commit message claims, holds
exactness at 128 nodes on 2B, and its no-op-when-unset guarantee is verified.
Its own commit message says NOT FOR MAIN until the 16/64-node sweeps land --
they have now landed (relay96).
