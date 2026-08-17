# `-u dist` UNBLOCKS THE MACHINE-WIDE SPLIT: Iteration 0 −17.0%, phase 1 −38%
# Jobs 5288750 (19 arms) and 5288783 (5 traced arms). ALL 24 ARMS EXACT.
# `bin/FoF3.2b.pesets4` (md5 2bd50769105e), patches/0010-0012.

## 1. The result

Means of 2 reps. Baseline = `-u serial`, no split, S3 on.

| arm | phase1 | phaseB | phase3_walk | **uf2** | Iter0 | vs base |
|---|---|---|---|---|---|---|
| serial base | 3.374 | 1.343 | 0.316 | 0.433 | 6480.1 | — |
| dist base | 3.328 | 1.292 | 0.326 | 0.462 | 6306.2 | −174 |
| serial top6 s=14 | 2.613 | 0.489 | 0.366 | 0.463 | 5828.3 | −652 |
| dist top6 s=14 | 2.607 | 0.483 | 0.340 | 0.474 | 5615.3 | −865 |
| serial all s=2 | 2.402 | 0.451 | 0.313 | **1.464** | 7032.7 | +553 |
| dist all s=2 | 2.465 | 0.427 | 0.315 | **0.568** | 5601.3 | −879 |
| serial all s=14 | 2.092 | 0.001 | 0.368 | **2.046** | 7591.8 | +1112 |
| dist all s=14 | 2.117 | 0.001 | 0.362 | **0.654** | 5413.5 | −1067 |
| dist top6 s=14, S3 off | 2.470 | 0.582 | 0.344 | 0.397 | 5420.0 | −1060 |
| **dist all s=14, S3 off** | **2.092** | **0.001** | 0.370 | **0.638** | **5379.4** | **−1101** |

**Recommended: `-u dist`, `FOF_PE_SETS=14` machine-wide, `FOF_S3=0`.**
Iteration 0 **6480 → 5379 ms, −17.0%**; phase 1 **3.374 → 2.092 s, −38.0%**;
phaseB **eliminated** (1.343 → 0.001 s). Exact, no migration, no stealing.

## 2. What dist actually fixed — the isolation is clean

The serial and dist arms differ in ONE stage. From the traced job, two runs at
the identical split (`sets=14`, S3 off), differing only in `-u`:

| | phase1 | phase3_walk | **uf2** | relabel | Iter0 |
|---|---|---|---|---|---|
| `-u serial` | 2.014 | 0.352 | **2.094** | 0.237 | 7552.1 |
| `-u dist` | 2.012 | 0.379 | **0.692** | 0.128 | 5373.7 |

Phase 1 is the same to 2 ms. The walk is the same. **The entire 2.2 s
difference is the union-find**, exactly the term flagged as the binding cost.
Serial gathers every edge to the root and unions there, so the split's 3.4x
edge inflation lands on one process; dist distributes it and absorbs the same
inflation for +0.2 s over its own baseline.

So issue 1 of `open-issues-explained.md` is now **closed by measurement**: the
union-find was the thing preventing a machine-wide split, and `-u dist` is the
answer. Note dist is worth only −174 ms on its own (6306 vs 6480) — its value
here is almost entirely that it makes the split affordable.

## 3. Turning phaseB stealing off is now a small WIN, not just free

| | S3 on | S3 off | Δ |
|---|---|---|---|
| dist top6 s=14 | 5615.3 | 5420.0 | −195 |
| serial top6 s=14 | 5828.3 | 5610.3 | −218 |
| dist all s=14 | 5413.5 | 5379.4 | −34 |

Earlier (job 5287653) S3-off was neutral at top-6; here it is 195–218 ms
better, consistently in both union-find modes. The nos3 arms are single reps,
so read the direction rather than the magnitude — but with the split in place
the S3 machinery is at best free and looks like small overhead. On the
unsplit baseline S3 is still worth 1.82 s, so this is substitution, not
refutation.

## 4. Traced runs of the recommended configuration — job 5288783

All 5 exact, and they reproduce the untraced numbers (traced `sumd-rec` Iter0
5373.7 against 5379.4 untraced), so the traces are of the real configuration.

| tarball | size | what |
|---|---|---|
| `pesets-sumd-rec-frontier.tar.gz` | ~250 MB raw / 3585 files | **the recommended config**: `+sumDetail`, full machine, `-u dist` sets=14 S3=0 |
| `pesets-sumd-base-frontier.tar.gz` | 3585 files | serial, no split, S3 on — the comparison |
| `pesets-sumd-rec-serial-frontier.tar.gz` | 3585 files | **the isolation control**: same split, `-u serial`. Identical phase 1, uf2 2.094 vs 0.692 |
| `pesets-proj-rec-frontier.tar.gz` | 165 files | projections, 182-PE subset, recommended config |
| `pesets-proj-base-frontier.tar.gz` | 169 files | projections, same subset, baseline |

Subset = PEs 672–797 (processes 48–56) and 1400–1455 (processes 100–103).

**What the profile should now show**, against
`timeProfile-2b-proto3-sumd-08-16.png`: the ~2 s phaseB tail at 5–15%
utilisation should be gone, because phaseB is gone. What remains is the
phaseA block, and the honest expectation is that its own tail is now the
visible one.

## 5. What is left, with numbers

phase 1 is now 2.092 s against a perfect-balance floor of 1.150 s
(`phase1-idle-structure.md`). **All 0.94 s of the remaining phase-1 waste is
phaseA**, which this mechanism cannot touch by construction — a set boundary
is a boundary between PEs, and phaseA is same-PE work. Its shape is a
19-process plateau, so no targeted method applies either. That is issue 3, and
it is now the whole of what remains in phase 1.

Issue 2 (the victim list) is moot for the recommended configuration: machine-
wide splitting needs no victim list at all. It only matters if the top-N
variant is preferred for some other reason — and at 16 nodes it is not, since
machine-wide now wins by 236 ms over top-6.

## 6. For the 64/128-node scaling runs

Three predictions worth writing down before the data exists, so they can be
checked rather than rationalised afterwards:

1. **Serial-mode union-find should break down further.** 4x the processes means
   4x the fragment contributors for a machine-wide split, all gathering to one
   root. If the serial/dist gap at 16 nodes is 2.2 s, it should widen.
2. **dist should improve relative to serial** for the same reason — Kale's
   "communication currently dominates in dist mode" is a statement about a
   regime where dist has too little work to amortise; the split gives it 3.4x
   more edges, which is exactly the load that should make it look better.
3. **phaseA per process should fall** at fixed problem size (fewer particles
   per process), but whether the PLATEAU SHAPE survives is the open question,
   and it decides whether anything further is worth attempting on phase 1.
