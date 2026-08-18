# Cleanup validation: 0957364 -> 8e08843

Job **5304461**, 16 nodes, 2B, 2026-08-18. Binary `bin/FoF3.2b.cleanup`,
md5 `2607c277f1bc87da19a533e466b8398e`. Companion: `reports/relay15.txt`.

**Verdict: the cleanup is a null on the top-line number. Iteration 0 moves
-1.5 ms (-0.03%) against relay13's as-is arms, all three reps exact, and no
dark time anywhere. One thing did move and it is worth knowing about: the
race-dependent work counters, by +0.1 to +0.3%, together with the
within-process phaseA skew going the other way. Section 5.**

## 1. What was removed, and why this is a null test with a sharp prior

Six commits, -1789 lines: targeted shedding (`FOF_SHED_*`), pre-build LB and
the TreePiece cost-model load, `PARATREET_POOL_ELEM_SIZE`, cross-node helpers
(`FOF_S3_XNODE`), the section-27 donor reservation (`FOF_S3_RESERVE`), and all
of S3 stealing including the deleted `fof/FoFStealTypes.h`. Tag
`campaign-2026-08-stealing` recovers the old line.

Every one of these was **off** in the recommended config, so the expectation
is exact equality of the answer and no timing change. That is what makes any
movement informative rather than noise.

I read the diff outside the named subsystems, because `src/CacheManager.h`
(-34) and `src/TreePiece.h` (-172) are shared files:

* **CacheManager** collapses `max(env ?: config.pool_elem_size, 128)` to
  `max(config.pool_elem_size, 128)`. `config.pool_elem_size` is never
  assigned by any app, so the effective value was 128 before and is 128 now.
  The `PARATREET pool_elem_size:` startup line is gone with it -- a removed
  print, not a regression, and it is correctly absent from every log here.
* **TreePiece / Driver / common.h / the .ci files** -- every removed line is
  inside shedding or S3 (the ranking comparator, `drainForeign`, the
  migration-safety comment block). Nothing on the live path.

## 2. Build

**Full clean rebuild, and deliberately not just `make clean`.** The clean
targets do remove `*.decl*.h *.def*.h *.o *.a`, but a deleted header is
exactly the case not to trust them on, so all 39 artifacts were removed
explicitly first across `paratreet2/{src,fof,examples/fof3}`, `htram`,
`unionfind`, `unionfind/prefixLib`, plus the stale `FoF3` binary.
`FoFStealTypes.h` is gone from the whole filesystem (`find` over
`~/software`).

Gates, checked in the job script before any 2B time is spent:

| kept feature | count | removed knob | count |
|---|---|---|---|
| `pe_sets` | 1 | `FOF_S3` | **0** |
| `phaseA_stages` | 1 | `SHED` | **0** |
| `stage_pe` | 1 | `RESERVE` | **0** |
| `FOF_STEALA` | 2 | `XNODE` | **0** |
| `FOF_PB_PARTS` | 1 | `POOL_ELEM_SIZE` | **0** |
| `keep-alive` | 3 | `FoFStealTypes` | **0** |

TraceSummary 0, TraceProjections 0, vfmadd 0. Tree `8e08843`, 0 files
modified. The only compiler warnings are the two pre-existing
`Configuration.h` return-type ones.

## 3. Config

The standing recommendation **minus every removed knob**. `FOF_S3=0` is gone
from the environment rather than passed as a harmless no-op: passing a
variable that no longer exists would quietly imply it still does something.
Kept: `-u dist`, `FOF_PE_SETS=14 FOF_PE_SETS_MODE=1`, `FOF_STEALA`/`_GEO`,
`FOF_PB_PARTS=16`, `FOF_PB_M2KEY=1`, `FOF_PHASEB_SLICE_MS=2`, the shipped
`-E` default (no `-E` flag), `+ppn 14 +lci_ndevices 7
+backend_poll_thread 2`.

## 4. Result: exact, and inside the band

10k gate exact (3549). **All three reps exact: 424,897,832.** 128/128
`pe_sets` and `phaseA_stages` lines on every arm. stderr empty.

| arm | Iteration 0 | Pre-traversal | Tree traversal | phaseA |
|---|---|---|---|---|
| warmup (excluded) | 5144.8 ms | 2428.4 | 1454.2 | 1.985 s |
| rep1 | 5148.4 | 2441.4 | 1441.3 | 2.000 |
| rep2 | 5178.4 | 2407.1 | 1496.2 | 1.965 |
| rep3 | 5171.9 | 2428.7 | 1449.6 | 1.985 |
| **relay13 as-is band** | **5155-5192** | **~2410** | **1440-1500** | **1.966-1.979** |

Means against relay13's as-is arms:

| row | relay13 (n=2) | relay15 (n=3) | delta |
|---|---|---|---|
| Iteration 0 | 5167.7 ms | 5166.2 ms | **-1.5 ms (-0.03%)** |
| Pre-traversal | 2411.5 | 2425.7 | +14.2 (+0.59%) |
| Tree traversal | 1486.8 | 1462.4 | -24.5 (-1.65%) |
| phaseA | 1968.5 | 1983.3 | +14.8 (+0.75%) |

**Stated honestly, because the band was built from two samples:** two
individual values sit just outside it -- rep1's Iteration 0 at 5148.4 ms is
6.6 ms below the low end, and rep1's Pre-traversal at 2441.4 ms is 27 ms
above relay13's higher arm. The sub-rows move by +14 and -25 ms in opposite
directions and cancel; Iteration 0, the number those rows add up to, is flat
to 0.03%. A two-sample band is not a tolerance, and nothing here survives as
a timing effect.

**Stall detector (wall minus instrumented parts), the standing check:**

| arm | Pre-traversal residual | Tree traversal residual |
|---|---|---|
| warmup | 229.4 ms | 1.2 ms |
| rep1 | 230.4 | 1.3 |
| rep2 | 231.1 | 0.2 |
| rep3 | 229.7 | 0.6 |

Tree traversal is accounted for by its own instruments to within 1.3 ms on
every arm. The Pre-traversal residual is the same constant block of
uninstrumented canopy/cache setup as always, and it is 1-2 ms LOWER than
relay13's 231-235 ms. No dark time appeared anywhere.

## 5. The one thing that did move: race-dependent work volume

This is not a wall-clock finding and it does not affect the answer, but it is
a real difference and it should not be filed as noise without saying so.

| counter | relay13 (n=3 as-is/warmup) | relay15 (n=4) | delta |
|---|---|---|---|
| edges emitted | 1,656,265 | 1,658,915 | **+0.160%** |
| leaf_visits | 11,548,243 | 11,569,158 | **+0.181%** |
| prunes negative | 313,226,403 | 313,551,162 | +0.104% |
| suppression | 16,082,299 | 16,135,768 | +0.332% |
| `phaseA_skew: within` | **1.35-1.36** | **1.25** | **-0.10** |

The whole vector moved together: about 0.1-0.3% more walk work, and better
within-process balance. Those two cancel, which is why Iteration 0 did not
move.

**What this is.** Every one of these counters is a function of WHICH PE
claimed WHICH piece, and the claim pool is a live race. Edge counts vary
run to run inside a single job by about the same amount as the between-job
difference (relay13 spread 3,054 edges over 7 runs; relay15 spread 2,779
over 4). What is striking is the pattern: within a job these numbers are
near-constant, between jobs they shift as a block.

`phaseA_skew: within` makes that plain, and it is now measured at three
values on three jobs:

| job | code state | within-skew |
|---|---|---|
| 5301010 (relay12) | `61685b7` | 1.23 |
| 5302846 (relay13) | `0957364` | 1.35-1.36 |
| 5304461 (relay15) | `8e08843` | 1.25 |

Relay15 sits BETWEEN the two earlier jobs, so the ordering does not track the
code at all. Relay13 caveat 25 already flagged this statistic as tracking the
allocation or the claim race rather than the code; this is the third point
and it supports that reading. The most likely driver is node placement --
each job gets a different set of Frontier nodes, and the claim race resolves
against whatever the local timing is.

**What would settle it**, if it ever matters: interleave a `8e08843` arm with
a `campaign-2026-08-stealing` arm inside ONE allocation. That removes node
placement as a variable and is the only clean way to attribute a 0.2% work
shift to code. It is not worth a job on its own -- the answer is exact and
Iteration 0 is flat -- but if these counters are ever used as a regression
signal, they must be compared within a job, never across jobs.

## 6. Caveats

* Three reps against a two-rep band. Adequate for the 0.03% conclusion on
  Iteration 0, not adequate to resolve a sub-1% effect in the sub-rows.
* The warmup arm was NOT cold this time (Pre-traversal 2428 ms, in line with
  the reps), unlike relay12's first arm at 4290 ms. It is still excluded from
  every mean above, but it agrees with the reps and can be read as a fourth
  sample.
* Only the recommended config was run. The removed knobs cannot be tested for
  "still off" because they no longer exist; what was tested is that the
  binary contains none of their strings and that the default path is
  unchanged.

## 7. Files

```
reports/cleanup-validation.md          this report
reports/relay15.txt                    the relay
sbatch/cleanup-validate-2b-16n.sbatch  the job
scripts/stall-attrib-analyze.py        the standing wall-minus-parts detector (unchanged)
```

Raw logs: `/lustre/orion/csc710/scratch/lvkale/s3ab/5304461`.
Comparison job: `/5302846` (relay13).
