# Frontier zero-code A/B: compiler flags, node-pool chunk size, load-model calibration

Task: `design/frontier-cheap-knobs-ab.md`. Session 2026-08-15 morning, Frontier
login node, project csc710. Short companion listing every place reality
disagreed with the spec: `reports/relay4.txt`.

| | |
|---|---|
| paratreet2 commit | `c302aafe8e6d3d8e69e768008c73eef02d80d94a` ("Merge branch 'main' into phaseab-campaign"), branch `phaseab-campaign` |
| charm | PRODUCTION `~/software/charm`, `reconverse-specific-build` @ `3d1fdd89f` — not pulled, still the pinned provenance |
| working tree | clean apart from the `utility` submodule. Nothing patched, nothing pushed |
| jobs | **5273921** (stopped on the exactness gate), **5273936** (13 arms), **5273967** (12 arms) |
| dataset | `/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024` (76.8 GB) |
| gold | `components: 424897832` |

## Summary

1. **Experiment A: the binary really is SSE2, and vectorising it helps phaseA
   and nothing else.** `-march=znver3` alone is **not bit-exact** (424897833,
   6 arms of 6) because it enables FMA contraction. `-march=znver3
   -ffp-contract=off` **is exact 6/6** and gives a real, consistent
   **-3.5% on phaseA** — but that does **not** survive to Iteration 0 (-0.9%,
   3/6 paired wins). `-Ofast` is much bigger (-12.8% phaseA, -4.0% Iter0, all
   6/6) and is **not exact**.
2. **Experiment B is a clean null.** Every `PARATREET_POOL_ELEM_SIZE` value
   sits inside the drift of two identical default arms. Memory cost is small
   and monotone (+6.1% pool bytes at 65536).
3. **The load-model calibration is the most valuable result here.** Frontier
   reproduces Anvil's `sqrt(pair)` regression at **r = +0.871** (Anvil +0.872),
   and — new — **`sqrt(pair)` ranks the actual worst process #1 of 128**.
4. Three instrument defects found (section 6), one of which would have made
   experiment B look like it never ran.

`PARATREET_PREBUILD_LB` and every piece-migration arm were **not run**, per the
task's explicit instruction.

---

## 1. The build

Compiler behind the Cray wrapper: **g++ 13.3.1** (`PrgEnv-gnu`).
**`-march=znver3` compiles cleanly — the `-march=native` fallback was never
needed.** Frontier is EPYC 7A53 (Trento, Zen 3), so znver3 is the correct
target and the one used throughout.

Build script: `~/software/scripts/build-cheapknobs-ab.sh`. All three makefiles
append `$(MAKE_OPTS)` last, so no makefile edit was needed; `MAKE_OPTS` was
passed on the `make` command line to `src/`, `fof/`, `examples/fof3/`. All
staged binaries verified untraced with `nm -C | grep -ci TraceSummary` = 0, per
BUILDS.md, rather than trusted from the filename.

`htram`, `unionfind` and charm were rebuilt against production charm but
**without** `-march`. The hot phaseA/phaseB walk is entirely inside paratreet2,
so this isolates the change to the code under test — and makes these numbers a
lower bound on a fully recompiled stack.

Worth knowing when reading the `fast` arm: `src/Makefile` has **always** used
`-g -Ofast`, so `-ffast-math` was already on in the core library. `fof/` and
`examples/fof3/` use `-g -O3`, and that is where `FoFPhase1.h` / `TreePiece.h`
— the hot walk — are actually instantiated. So the `fast` arm's real change is
bringing `-ffast-math` to the walk for the first time.

### 1a. Recipe correction: charmc mis-parses `-march=X -Ofast` in that order

```
charmc ... -march=znver3 -Ofast -c mp.C -o mp.o
  -> cc1plus: error: bad value 'znver3 -Ofast' for '-march=' switch
```

The two tokens reach cc1plus joined into the `-march=` value. Reversing them
(`-Ofast -march=znver3`) works, and is how the `fast` binary was built. The
error names `-march=` and sends you hunting for an unsupported architecture,
which is not the problem.

### 1b. objdump vector-instruction counts

`objdump -d <bin> | grep -c -E "vfmadd|vmulps|vaddps|ymm"`:

| binary | MAKE_OPTS | ymm/vex | vfmadd | vmulps | vaddps | vmulpd | vaddpd | md5 |
|---|---|---|---|---|---|---|---|---|
| `FoF3.2b.base` | (none) | **0** | 0 | 0 | 0 | 0 | 0 | `8347348c5690` |
| `FoF3.2b.march` | `-march=znver3` | 5540 | 69 | 4 | 28 | 10 | 22 | `ee16508ca847` |
| `FoF3.2b.march3` | `-march=znver3 -ffp-contract=off` | 5479 | **0** | 4 | 28 | 10 | 22 | `96ea42bb8232` |
| `FoF3.2b.fast` | `-Ofast -march=znver3` | 5709 | 73 | 4 | 28 | 11 | 45 | `806c775011c3` |

The control has **literally zero** AVX/VEX instructions: the shipped Frontier
binary is SSE2, two float lanes, exactly as Anvil found qualitatively. The
*magnitude* does not transfer — Anvil reported 926 ymm refs for znver3, Frontier
gives 5540, six times as many. Treat 926 as an Anvil number only.

Note the `march3` row: forbidding FMA contraction costs 61 of 5540 vector
references (1.1%) and removes all 69 FMAs. Essentially all of the vectorisation
survives, which is why it is as fast as `march` (section 2c).

---

## 2. Experiment A — compiler flags

### 2a. The exactness gate fired (job 5273921) — stop-and-report

Job 5273921 ran `cmp-base-1` (exact), then `cmp-march-1`, which returned
**424897833**. Per the campaign rule that aborted every remaining arm — 10 of
12 — including the whole pool sweep, which runs on the base binary and could
not have been affected. Reported as required. Jobs 5273936 and 5273967 are the
follow-ups; 5273936 runs experiment B first so a compiler arm cannot cost it
again, and splits the gate: **hard** (abort) on base-binary arms, **soft**
(record, continue) on arms whose entire purpose is a non-default compiler flag,
where the component count *is* the measurement.

### 2b. The cause is FMA contraction, and a flag fixes it

GCC defaults to `-ffp-contract=fast`, letting it fuse `a*b+c` into one FMA with
a single rounding instead of two. **SSE2 has no FMA instruction**, so the base
binary physically cannot contract; enabling znver3 makes FMA available and GCC
starts using it. FoF's linking-length test is a float distance compared against
a threshold, so a pair sitting on the boundary falls the other way — one
component out of 425 million.

That predicts `-march=znver3 -ffp-contract=off` is bit-exact and still fast.
Both halves confirmed:

| binary | exact arms |
|---|---|
| `FoF3.2b.base` | **6 / 6** |
| `FoF3.2b.march` (`-march=znver3`) | 0 / 6 (all `424897833`, +1) |
| `FoF3.2b.march3` (`+ -ffp-contract=off`) | **6 / 6** |
| `FoF3.2b.fast` (`-Ofast -march=znver3`) | 0 / 6 (all `424897833`, +1) |

Neither count is "wrong" — both are defensible float answers. But the campaign's
contract is bit-reproducibility, so any adopted flag must carry
`-ffp-contract=off`.

### 2c. Timing — job 5273967, 4 reps, interleaved base/march3/fast

| arm | components | phase1 | phaseA | phaseB | phaseB max | Pre-trav ms | Iter0 ms |
|---|---|---|---|---|---|---|---|
| cmp-base-1 | EXACT | 3.288 | 2.025 | 1.290 | 1.265 | 3678.3 | 6376.8 |
| cmp-base-2 | EXACT | 3.305 | 2.067 | 1.277 | 1.223 | 3696.9 | 6453.0 |
| cmp-base-3 | EXACT | 3.310 | 2.018 | 1.291 | 1.260 | 3694.1 | 6379.5 |
| cmp-base-4 | EXACT | 3.283 | 2.075 | 1.234 | 1.203 | 3669.3 | 6344.1 |
| cmp-march3-1 | EXACT | 3.154 | 1.980 | 1.231 | 1.165 | 3542.3 | 6323.1 |
| cmp-march3-2 | EXACT | 3.311 | 1.977 | 1.277 | 1.212 | 3693.8 | 6459.1 |
| cmp-march3-3 | EXACT | 3.182 | 1.992 | 1.273 | 1.244 | 3580.9 | 6332.8 |
| cmp-march3-4 | EXACT | 3.318 | 1.957 | 1.357 | 1.328 | 3708.8 | 6418.8 |
| cmp-fast-1 | 424897833 | 3.045 | 1.778 | 1.230 | 1.202 | 3433.6 | 6124.2 |
| cmp-fast-2 | 424897833 | 3.045 | 1.797 | 1.218 | 1.188 | 3431.5 | 6243.4 |
| cmp-fast-3 | 424897833 | 2.878 | 1.780 | 1.178 | 1.147 | 3273.1 | 6001.7 |
| cmp-fast-4 | 424897833 | 3.045 | 1.789 | 1.225 | 1.185 | 3436.5 | 6175.5 |

Pooled over both jobs (6 reps each of base / march3 / fast; `march` only has the
2 reps from 5273936). `wins` is paired — reps in which the arm beat base at the
same rep index. Tool: `~/software/scripts/cmpfit.py`.

| metric | base mean | base's own spread | march3 | delta | wins | fast | delta | wins |
|---|---|---|---|---|---|---|---|---|
| phase1 | 3.313 | 7.9% | 3.247 | -2.0% | 3/6 | 3.045 | **-8.1%** | **6/6** |
| **phaseA** | 2.049 | **2.8%** | 1.976 | **-3.5%** | **6/6** | 1.786 | **-12.8%** | **6/6** |
| phaseB | 1.275 | 12.5% | 1.276 | +0.1% | 3/6 | 1.237 | -3.0% | 5/6 |
| Pre-trav | 3701.5 | 7.2% | 3635.7 | -1.8% | 4/6 | 3432.9 | **-7.3%** | **6/6** |
| Iter0 | 6418.7 | 5.5% | 6363.0 | -0.9% | 3/6 | 6165.2 | **-4.0%** | **6/6** |

(`march` at 2 reps: phaseA 1.977, i.e. identical to `march3`'s 1.976 — the 61
lost vector references cost nothing. It is simply the non-exact form of the
same binary and is not a candidate.)

**Read:**

- **phaseA is the one clean signal for the exact form.** Base's own rep-to-rep
  spread there is only 2.8%, and march3 beats it in **6 of 6 paired reps** at
  -3.5%. That is SIMD doing exactly what the hypothesis said it would: the
  float distance tests get faster.
- **It does not survive to the top line.** phaseA is 2.05 s of a 6.42 s
  Iteration 0, so -3.5% of it is ~0.072 s ≈ 1.1% of Iter0 — and Iter0's own
  base spread is 5.5%. march3 wins Iter0 in only 3 of 6 reps. **There is no
  end-to-end result for the exact form.**
- **phaseB does not move at all** under either binary. The straggler — the
  thing the campaign is chasing — is untouched by vectorisation. That is
  consistent with section 34: the residual is serialization and walk execution
  on the hot process, not arithmetic throughput.
- **`-Ofast` is the real effect, and it is not free.** -12.8% phaseA, -8.1%
  phase1, -7.3% Pre-traversal, -4.0% Iteration 0, every one of them 6/6. But
  0/6 exact. The extra over `march3` (-9.6% more on phaseA) is `-ffast-math`,
  not wider registers — both binaries have ~5500 vector references.

### 2d. Recommendation, and the open follow-up

- **Adopting `-march=znver3 -ffp-contract=off` is defensible but not
  compelling.** It is exact, it is free, and it makes phaseA measurably faster
  — but it buys nothing you can see at Iteration 0 today. Take it as hygiene
  (the binary should not be SSE2 in 2026), not as a lever.
- **Do not adopt `-Ofast`** as it stands: it breaks the exactness contract.
- **OPEN, and cheap to settle.** `-Ofast` is `-O3` plus `-ffast-math` plus a
  few others, and `-ffast-math` is itself a bundle. The member that plausibly
  delivers most of the speed here — `-fno-math-errno`, which only stops GCC
  preserving `errno` across `sqrt` and thereby lets it inline and vectorise —
  is **semantically safe and should be bit-exact**. The reassociation
  (`-funsafe-math-optimizations`) is the part that is not. One more 12-arm job
  (`-march=znver3 -ffp-contract=off -fno-math-errno`, 4 reps against base and
  fast) would separate them, and could plausibly recover a large part of that
  4% Iteration 0 **exactly**. Not run — awaiting the word.

---

## 3. Experiment B — `PARATREET_POOL_ELEM_SIZE`

Run on `FoF3.2b.base` in job 5273936. The spec says to run B "on whichever
binary A shows fastest"; taken literally that is `fast`, which is not exact, so
B ran on base — which also keeps it comparable to every prior campaign number.

### 3a. The spec's proof-the-knob-took line does not exist — a better one does

The spec says the binary prints `PARATREET pool_elem_size: N nodes (K KB/chunk)`
at startup and to check it in every arm. **It prints on no process at all.**
The string is in the binary. The guard is `if (CkMyPe() == 0)` inside
`CacheManager::initialize`, and `src/paratreet.ci` declares
**`nodegroup CacheManager`** (`GROUP_CACHE` is not defined in any makefile). A
nodegroup branch entry method can be delivered to any PE of the process, and on
process 0 it did not land on PE 0. Confirmed in the logs: nothing appears
between `* Initializing cache managers.` and `* Initialization`.

Anyone following the spec literally would conclude the knob did not take. It
did — from `FOF3STAT cache: pool_MB`, which is
`chunks x pool_elem_size x sizeof(FullNode)`, i.e. allocated capacity, exactly
the quantity chunk size changes:

| arm | chunk | pool_MB (128 procs) | avg MB/proc | max MB/proc | vs default |
|---|---|---|---|---|---|
| pool-default-1 | 128 (27 KB) | 237990.4 | 1890.5 | 2243.1 | — |
| pool-4096 | 4096 (864 KB) | 238888.6 | 1897.6 | 2251.2 | +0.38% |
| pool-16384 | 16384 (3.4 MB) | 241668.8 | 1919.3 | 2282.7 | +1.55% |
| pool-65536 | 65536 (13.8 MB) | 252635.5 | 2004.9 | 2347.0 | **+6.15%** |
| pool-default-2 | 128 (27 KB) | 237997.7 | 1890.6 | 2235.9 | +0.003% |

Monotone in chunk size, and the two default arms agree to **0.003%**. That is
the proof the knob took, and it is a better instrument than the intended print.
Fix upstream: guard on `CkMyNode() == 0 && CkMyRank() == 0`, or print from
`Driver`.

### 3b. Timing — a clean null

| arm | components | phase1 | phaseA | phaseB | phaseB max | Pre-trav ms | Iter0 ms | RSS max MB | RSS mean MB |
|---|---|---|---|---|---|---|---|---|---|
| pool-default-1 | EXACT | 3.257 | 2.066 | 1.216 | 1.189 | 3646.5 | 6335.2 | 10217 | 7983 |
| pool-4096 | EXACT | 3.245 | 2.049 | 1.253 | 1.225 | 3631.4 | 6385.8 | 10166 | 7961 |
| pool-16384 | EXACT | 3.376 | 2.019 | 1.239 | 1.211 | 3763.0 | 6431.5 | 10219 | 7978 |
| pool-65536 | EXACT | 3.261 | 2.005 | 1.251 | 1.224 | 3642.6 | 6329.4 | 10366 | 7916 |
| **pool-default-2** | EXACT | 3.317 | 2.041 | **1.412** | **1.386** | 3703.0 | 6374.1 | 10426 | 7981 |

**No effect.** The whole sweep spans 3.245-3.376 on phase1 (4.0%), while the two
*identical* default arms differ by 1.8%. On phaseB the drift control is the
**worst arm of the five** (1.412 against default-1's 1.216, a 16% gap between
two runs of the same configuration). The drift is larger than any signal.
5/5 exact.

### 3c. Why the memory cost is far smaller than the spec expected

The spec warned that "chunks are allocated whole, so large values waste up to
one chunk per lane per piece-tail". That over-states it. `FullNodePool`
(`src/TreeCache.h`) is a bump allocator over a **list** of chunks: when a
piece's nodes run past the end of a chunk a new one is allocated, but the
**next piece continues in the same chunk**. Waste is therefore at most one
partial chunk **per pool**, not per piece — measured, +114 MB/process at
13.8 MB chunks, 6.1%.

The same structure explains the null. A piece's nodes were **already**
bump-allocated in depth-first preorder with nothing interleaved; all the knob
removes is a chunk boundary every 128 nodes. Evidently that boundary is not
where the time is.

### 3d. RSS

`/usr/bin/time -f WRAPRSS=%M` was wrapped around every rank, gated first by a
2-node 10k run (passed, 3549 components, wrapper lines present). This was
necessary because the spec's readout, `PARATREET vmhwm_mb`, is printed **after
decomposition — before `buildTree` allocates any pool chunk** — so it cannot
move with this knob, and it did not (8508-8553 MB across every arm of every
job). Per-process peak RSS is flat too: 10.1-10.4 GB max, 7.9-8.0 GB mean, with
no ordering by chunk size. The +114 MB/process of pool growth is inside the
run-to-run RSS noise.

---

## 4. Load-model calibration — 128 verbatim lines

From `cmp-base-1` of job 5273921 (an exact arm), all 128 processes. Verbatim
copy: `~/software/reports/load_model-frontier-2b-128proc.txt` (original at
`/lustre/orion/csc710/scratch/lvkale/s3ab/5273921/load_model-cmp-base-1.txt`).
Analysis: `~/software/scripts/loadmodel_fit.py`.

### 4a. Spread over the 128 processes

| quantity | min | max | spread |
|---|---|---|---|
| pieces | 382 | 639 | 1.67x |
| n (particles) | 1.5395e7 | 1.5577e7 | 1.012x |
| `self` (sum n^1.2) | 1.311e8 | 1.390e8 | 1.06x |
| `pair` (sum n^2/V^(4/3)) | 2.365e20 | 1.025e25 | 4.33e4x |
| **actual phaseA** (pa_sum_s) | 11.078 | 23.447 | **2.12x** |
| **actual phaseB** (pb_sum_s) | 0.121 | 15.144 | **125x** |

### 4b. phaseB predictors — Frontier reproduces Anvil to three digits

| form | Pearson r (Frontier) | spread | Anvil r (job 19932506) |
|---|---|---|---|
| `pair` (mean-field, as implemented) | +0.783 | 4.33e4x | +0.746 |
| **`sqrt(pair)`** | **+0.871** | **208x** | **+0.872** |
| `pair^0.25` | +0.835 | 14.4x | +0.862 |
| `log(pair)` | +0.716 | 1.23x | +0.760 |
| n^2 (no volume) | -0.043 | 1.02x | — |
| pieces | -0.303 | 1.67x | — |
| (actual pb_sum_s) | — | 125x | — |

`sqrt(pair)` wins on both machines, at the same r to three digits, and again has
roughly the right magnitude (208x predicted against 125x actual, versus the raw
term's 43000x). **This is now a two-machine result, not an Anvil artefact.**

### 4c. phaseA predictors — the self term is still worse than useless

| form | Pearson r | spread |
|---|---|---|
| `self` (sum n^1.2) | **-0.263** | 1.06x |
| n (particles) | +0.007 | 1.012x |
| pieces | **-0.588** | 1.67x |
| (actual pa_sum_s) | — | 2.12x |

Anvil measured -0.187, Frontier -0.263. Same sign, same conclusion, slightly
stronger: the exact `sum n^1.2` is **anti-correlated** with actual phaseA.
`piece-load-model.md` section 5's reading holds — piece-size heterogeneity is
anti-correlated with time because a process holding a few big dense pieces
sends them down the O(n log n) `-G` grid path instead of the walk. Any usable
self term must branch on the same occupancy gate the grid uses.

`pieces` at -0.588 is the strongest single correlate of phaseA here, and its
sign says the same thing: *more* pieces means *less* phaseA time.

### 4d. Calibration

Least squares through the origin, 128 processes:

```
phaseA ~ a * self        a = 1.21146e-07
phaseB ~ c * sqrt(pair)  c = 4.65493e-12
K = c/a = 3.8424e-05
```

Treat `K` as provisional: it calibrates the pair term against a self term that
section 4c shows carries no usable signal, so the ratio inherits that.

### 4e. The operational result — `sqrt(pair)` finds the straggler at rank 1

This is what targeted-shedding v2 needs, and it was not measurable on Anvil
where the straggler is mild.

```
pb_sum_s over 128 processes: min 0.121  median 1.579  mean 2.030  max 15.144
  max/median 9.6x   max/mean 7.5x   max/min 125x
  2nd worst 9.626 (6.1x median)   3rd 8.905
  top 3 processes hold 13.0% of all phaseB work
```

The worst process is **node 55** — the process the campaign has been calling
out, identified here independently — at 15.144 s against a 1.579 s median.
(The prompt's "14.9x the median" does not reproduce: it is 9.6x the median,
7.5x the mean, 125x the min.)

| rank | predicted by sqrt(pair) | its actual pb_sum_s | | actual worst | its sqrt(pair) rank |
|---|---|---|---|---|---|
| 1 | node 55 | 15.144 | | node 55 (15.144) | **#1** |
| 2 | node 87 | 8.905 | | node 54 (9.626) | #6 |
| 3 | node 71 | 4.181 | | node 87 (8.905) | #2 |
| 4 | node 115 | 3.668 | | node 80 (7.083) | #7 |
| 5 | node 41 | 3.659 | | node 83 (6.752) | #33 |

- **Rank 1 is correct.** The model's single worst-predicted process *is* the
  actual straggler. For a scheme that sheds a few pieces off the worst process,
  that is the property that matters.
- Below rank 1 it degrades: top-5 overlap **2/5**, top-10 **5/10**. So
  `sqrt(pair)` is a good **detector** of the extreme and a poor **ranker** of
  the merely-bad. A v2 that sheds from the top one or two processes is
  supported by this data; one that rebalances a top-N is not.
- Ceiling: phaseB wall is set by the max process at 15.144 s against a mean of
  2.030 s — perfect phaseB balance is worth **7.5x** on that phase.

---

## 5. Exact srun line

Identical on every 2B arm; only the binary and `PARATREET_POOL_ELEM_SIZE`
differ.

```
srun -N 16 --ntasks-per-node=8 -t 15:00 --mpi=cray_shasta --network=job_vni \
     --unbuffered --cpu-bind=none --distribution=block:block \
     /usr/bin/time -f WRAPRSS=%M \
     $BIN -f /lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024 \
     -d oct -u serial +traceoff +ppn 14 \
     +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
     +lci_ndevices 7 +backend_poll_thread 2
```

Environment on every arm (the campaign's current best cell):

```
LCI_ATTR_BACKEND=ofi  FI_CXI_RX_MATCH_MODE=hybrid  PMI_MAX_KVS_ENTRIES=4194304
FOF_STEALA=1 FOF_STEALA_GEO=1 FOF_PB_M2KEY=1
FOF_PROCS_PER_PNODE=8 FOF_PHASEB_SLICE_MS=2 FOF_PB_PARTS=16 FOF_S3=1
```

sbatch files kept for provenance: `~/software/sbatch/cheap-knobs-16n.sbatch`
(5273921), `cheap-knobs2-16n.sbatch` (5273936), `cheap-knobs3-16n.sbatch`
(5273967).

---

## 6. Anomalies and defects

1. **`-march=znver3` alone is not exact** (+1 component, 6/6, two jobs).
   FMA contraction. Fix: `-ffp-contract=off`. Not a code bug, but it breaks the
   bit-reproducibility contract.
2. **`PARATREET pool_elem_size:` never prints** (3a). `CkMyPe() == 0` is the
   wrong guard for a nodegroup branch. The spec tells you to use this line as
   proof the knob took; following it would give the wrong conclusion.
3. **`PARATREET vmhwm_mb` is printed too early to measure this knob** (3d).
4. **`CacheManager::cacheStats` is dead code** — no caller in `src/`, `fof/` or
   `examples/`. The `FOF3STAT cache:` line that does print comes from
   `examples/fof3/FoF3.C:761` by another path.
5. **charmc mis-parses `-march=X -Ofast`** in that order (1a).
6. **Anvil's 926 ymm refs do not transfer** — Frontier gives 5540 (1b).
7. **The prompt's "proc 55 at 14.9x the median" does not reproduce** — node 55
   is confirmed as the straggler, but at 9.6x the median (4e).
8. `+traceoff` draws `WARNING: +traceoff ... was not parsed by the RTS` on every
   run. Harmless on an untraced binary; it is in the standard idiom; it is noise.
9. Two arms logged 127 WRAPRSS lines instead of 128 (a rank's line lost in
   interleaved stdout). Cosmetic; means/maxima are over what landed.
10. The local paratreet2 tree was not clean before the pull — it carried an
    uncommitted FLATSPLIT-timer + flatten-bulk-insert edit. Upstream had taken
    the bulk insert and dropped the timers, so the local copy was discarded to
    land on `c302aaf`, after preserving it as
    `patches/0007-s3-flatsplit-and-bulk-insert-SUPERSEDED.patch` and
    `patches/wip/flatsplit.FoFPhase1.h`. Nothing lost.

---

## 7. What this changes

- **SIMD is real but small in its exact form.** `-march=znver3
  -ffp-contract=off` is exact and gives a consistent -3.5% phaseA, and that is
  worth taking as hygiene. It does **not** move Iteration 0 and it does
  **nothing** for phaseB, so it is not the lever the campaign is looking for.
- **The interesting number is `-Ofast`'s -4.0% Iteration 0**, repeatable 6/6 —
  and the open question is how much of it is recoverable exactly. Section 2d
  proposes the one job that would answer it.
- **Drop `PARATREET_POOL_ELEM_SIZE`** as a lever: a null, and 3c explains why
  the structural argument for it was weaker than it looked. This removes
  another prop from `treepiece-contiguous-build.md`, which section 34 had
  already re-scoped — chunk-level contiguity is not where the time is, so the
  remaining case for a per-piece arena rests on walk locality at a granularity
  this knob does not reach.
- **`sqrt(pair)` is validated on two machines**, and on Frontier it identifies
  the extreme process at rank 1 of 128. Targeted-shedding v2 should shed from
  the top one or two processes, not rebalance a top-N.
- Section 34's picture is unchanged and slightly reinforced: phaseB did not
  move under any flag tested here, so the straggler residual remains
  serialization and walk execution on the hot process, not arithmetic
  throughput.
