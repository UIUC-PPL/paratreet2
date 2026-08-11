# phaseb-pool vs main A/B at 4 nodes — RESULT (2026-07-27)

COMPLETE. Final numbers in "Interleaved result" at the bottom; the earlier
sections are the setup record. Started as mid-task resume notes.

## Interleaved result (job 19529535, authoritative)

One allocation, nodes a[668,677,680,688], alternating main/pool, 3 reps each.
`results-poolab/ab_{main,pool}.rep{1,2,3}.log`.

| variant | rep | reset | register | phaseA | **phaseB** | phaseB_s min/avg/max | phase1 |
|---|---|---|---|---|---|---|---|
| main | 1 | 0.015 | 0.013 | 0.268 | 0.252 | 0.000/0.014/0.252 | 0.553 |
| main | 2 | 0.001 | 0.001 | 0.265 | 0.251 | 0.000/0.014/0.251 | 0.519 |
| main | 3 | 0.001 | 0.001 | 0.266 | 0.263 | 0.000/0.014/0.263 | 0.539 |
| pool | 1 | 0.002 | 0.001 | 0.273 | 0.152 | 0.001/0.014/0.148 | 0.401 |
| pool | 2 | 0.001 | 0.001 | 0.274 | 0.151 | 0.002/0.015/0.149 | 0.411 |
| pool | 3 | 0.024 | 0.013 | 0.272 | 0.150 | 0.002/0.014/0.148 | 0.434 |

1. **Correctness: PASS.** All 6 logs byte-identical to the reference
   components line (`cmp` against the literal string).
2. **Primary: phaseB 0.255 -> 0.151 avg of 3, a 0.104 s / 41% cut**, far
   outside the run-to-run spread (main 0.251-0.263, pool 0.150-0.152; the two
   ranges do not overlap). BUT the laptop's leveling did NOT reproduce: the
   phaseB_s **avg is unchanged at 0.014-0.015** and max stays at the stage
   wall, so the spread is still ~10x, not ~1 ms. The pool halves the
   straggler without flattening it.
3. **reset/register: no inflation.** 0.001-0.002 s in both variants, with an
   occasional 0.013-0.024 blip that appears on main and pool alike (it is
   first-run-in-job noise, not variant-correlated). Expected result confirmed.

Side effects worth recording: phaseA is **+0.007 s consistently higher** on
pool (0.265/0.266/0.268 -> 0.272/0.273/0.274) — small, reproducible across all
3 reps, and it partially offsets the win. Net phaseA+phaseB 0.521 -> 0.424.
`phase1` total 0.519-0.553 -> 0.401-0.434.

Why the max/avg spread survives: phaseB_s max ~0.148 against an avg of 0.014
means one PE still carries ~10x the mean. The pool is **process-local**, so it
cannot move work across the 32 processes; the residue is either cross-process
skew or a single indivisible expensive TreePiece pair (the pool's unit of work is
one pair, which it cannot split). Distinguishing the two needs a per-process
phaseB breakdown or a Projections timeline — not done here.

## Projections traces of phaseb-pool at 4 nodes (job 19533532)

Submitted to look at the residual phaseB straggler. Two arms, both staged from
`phaseb-pool` 9b6bf65, same 32x15/480-PE layout as the timing A/B:

| binary | aggregation (htram) | check |
|---|---|---|
| `traced-bin/FoF3.agg` | ON | 472 trace syms, 732 htram syms |
| `traced-bin/FoF3.noagg` | OFF | 472 trace syms, **0** htram syms |

Script `trace-poolab.sbatch`; output under `results-trace-pool/{agg,noagg}/`,
stdout in `results-trace-pool/stdout_{agg,noagg}.log`. Launcher for one-off
runs: `traced-bin/run-noagg.sh`.

DONE, both arms exit 0, 480 `.log.gz` each (agg 670 MB, noagg 567 MB), and both
components lines match the reference.

| arm | phaseA | phaseB | phaseB_s min/avg/max | phase1 | max entries/PE |
|---|---|---|---|---|---|
| agg | 0.265 | 0.148 | 0.002/0.015/0.147 | 0.398 | 305,431 |
| noagg | 0.267 | 0.149 | 0.002/0.015/0.147 | 0.395 | 319,091 |

Two things these confirm:
- **Tracing overhead on phase 1 is negligible** — traced phaseB 0.148/0.149 vs
  untraced 0.150/0.151/0.152. The timeline is measuring the real thing.
- **htram is irrelevant to phase 1**, as expected: agg and noagg agree to
  ~0.002 s on every phase-1 stage. Aggregation only touches the unionfind
  phase-3 path.
- Entry counts stayed at ~31% of the 1M default, so no mid-run flush. Confirms
  the +logsize analysis above.

CAUTION: `uf2` reads 1.783 (agg) vs 0.061 (noagg) here, but that stage has huge
run-to-run variance in the untimed logs (0.158-2.329 across three main reps at
this same configuration), so a single pair proves nothing about aggregation's
effect on uf2. Needs interleaved repeats if that question matters.

NOTE `traced-bin/FoF3` (undated name, Jul 26) is from the OLD `phase1-grid`
branch — not this commit. Use the `.agg`/`.noagg` binaries for pool work.

### +logsize facts (checked in the charm source, 2026-07-27)

- Default is **1000000 log entries per PE** — `DefaultLogBufSize`,
  `src/ck-perf/trace-projections.C:23`, assigned at line 857.
- The flag is **`+logsize`** (single plus, a runtime arg next to `+ppn`), not
  `++logsize`; `++` prefixes are charmrun-level and we launch with srun.
- Units are ENTRIES and the buffer is reserved up front:
  `LogPool::LogPool` does `pool.reserve(CtrLogBufSize)` (line 160).
  `sizeof(LogEntry)` = **88 bytes** here (PAPI off, so no `papiValues`), so the
  default reserves **83.9 MB/PE** = 1.26 GB/process = ~10.1 GB/node of 257.
- Flush fires at `pool.size() == pool.capacity()` (line 499). The busiest PE in
  the earlier 4-node trace wrote 302,213 entries, i.e. 3.3x headroom, so the
  buffer never filled and no mid-run flush perturbed that timeline. Default is
  fine at this scale; no `+logsize` override used.

## No-aggregation three-way + pool2 trace (jobs 19533614, 19533645)

All arms built WITHOUT htram aggregation (0 htram symbols, verified).
`main` d6e66c9, `pool` 9b6bf65, `pool2` 70e36cb. 4 nodes / 480 PEs, grid off.
Logs: `results-poolab/ab3n_*.rep*.log`, trace in `results-trace-pool2/noagg/`.

Correctness: all 9 runs byte-identical to the reference components line.

| variant | phaseB (3 reps) | phaseB_s min/avg/max | phaseB_maxpair_s | phase1 |
|---|---|---|---|---|
| main | 0.251/0.251/0.251 | 0.000/0.014/0.251 | — | 0.520-0.690 |
| pool | 0.151/0.150/0.149 | 0.002/0.015/0.149 | — | 0.399-0.515 |
| pool2 | 0.106/0.102/0.102 | 0.001/0.014/0.099 | 0.000/0.004/**0.062** | 0.353-0.356 |

phaseB **0.251 -> 0.102, -59% vs main** (pool alone -41%; the split adds 0.048).
Geometry-gated enumeration is free: it runs AFTER `stage_tA` is recorded, so its
cost is inside the 0.102, and pool2's phaseA is the most stable of the three.

### Where the residual actually is (from the trace, PE 373)

Trace certified flush-clean: 0 type-8 markers on all 480 PEs, max 329,763
entries vs the 1M default. Traced numbers match untraced (phaseB 0.101,
maxpair 0.062 identical), so tracing overhead is not a factor.

Per-PE longest `phaseBChained()` (entry 399), 480 PEs, in ms:

    max 99.4 (PE 373)   p99 66.2   p95 36.7   median 10.9   min 1.1
    only 10 of 480 PEs are above 50 ms

Per PROCESS (15 PEs each), total phaseB work:

| | process | sum | avg/PE | max PE |
|---|---|---|---|---|
| heaviest | 24 | 692.9 ms | 46.2 | 99.4 (PE 373) |
| | 26 | 579.7 ms | 38.6 | 65.4 |
| | 31 | 493.6 ms | 32.9 | 70.6 |
| lightest | 18 | 26.8 ms | 1.8 | 3.2 |

**26x spread in total phaseB work BETWEEN processes.** This decomposes the
99.4 ms wall on PE 373 into three separable parts:

- **46.2 ms — cross-process floor.** Process 24's per-PE average. A
  process-local pool cannot go below this no matter how well it schedules,
  because the work is not in the right process to begin with.
- **62 ms — the largest single unit**, which alone EXCEEDS that floor. Another
  split level is justified: not to fix packing, but because one unit is bigger
  than a perfectly-leveled process share.
- **37 ms — packing tail** (99.4 - 62): work stacked on the same PE as the
  giant. LPT / largest-first claiming addresses this.

Actionable reading: split + LPT should take phaseB from ~99 ms toward the
~46-50 ms floor, roughly halving it again. Below that the binding constraint
becomes cross-process imbalance, which needs work moved between processes
(global pool, or a better TreePiece->process assignment) — more splitting will
not help there. Global per-PE average is 14 ms, so process 24's floor is 3.3x
the ideal; that gap is the whole remaining opportunity.

## Setup record

## Task

A/B `paratreet2` branch `phaseb-pool` against `main` on lambb.00500 (80M) at
4 nodes, **32 processes x 15 PEs = 480 PEs, grid off**. Readout asked for:

1. Correctness gate: `FOF3STAT components:` byte-identical to the reference
   line `23707197 max_size 1519203 log2_histogram: 0:18867099 ...`
2. Primary: `FOF3STAT balance: ... phaseB_s min/avg/max` and the
   `phase1_stages` **phaseB** value vs the main baseline. Laptop result to
   compare against: the pool leveled phaseB to a ~1 ms spread with zero
   synchronization overhead; question is whether it collapses multi-process
   skew at 15-PE processes the same way.
3. Also note `phase1_stages` **reset** / **register** — on the laptop's 2x4
   shape those walls inflated unexpectedly; expected no effect at scale.

## What is already done

- `paratreet2` fetched. `phaseb-pool` = `9b6bf65`, which is **exactly
  `origin/main` (`d6e66c9`) + 1 commit**, touching only `src/FoFPhase1.h`
  (dynamic phaseB pair pool, chunk size 8, atomic claim index).
- **The old `results-grid-ab/n4_G0.rep{1,2,3}.log` are a code-valid main
  baseline**: `git diff a9d0dea origin/main -- src examples` is EMPTY, so the
  commit that produced them has byte-identical application code to main.
- Both binaries are built and **staged** (production charm, `$RECHARM/charm`):
  - `results-poolab/bin/FoF3.pool` (phaseb-pool `9b6bf65`)
  - `results-poolab/bin/FoF3.main` (main `d6e66c9`)
- Working tree is back on `phaseb-pool` and rebuilt (src + examples/fof3).
- Job **19529529** (3 pool reps, cross-job vs old baseline) COMPLETE —
  logs in `results-poolab/n4_pool.rep{1,2,3}.log`, driver `pool-19529529.log`.
- Job **19529535** (interleaved main/pool, 3 reps each, ONE allocation)
  SUBMITTED and pending as of this writing. Script `run-poolab2.sbatch`.
  Logs land at `results-poolab/ab_{main,pool}.rep{1,2,3}.log`, driver
  `results-poolab/ab-19529535.log`.

## Why job 19529535 exists (do not skip it)

Job 19529529's pool reps beat the old baseline on **phaseA** too
(0.339-0.396 -> 0.271-0.274), plus `phase1` (0.67-0.76 -> 0.40) and
`edge_gather`. The pool commit cannot touch phaseA, so that older allocation
was globally slower — a cross-job comparison is not trustworthy for a ~0.2 s
effect. 19529535 runs both staged binaries alternately in one allocation.

## Results so far

Correctness gate: **PASS** — all three pool reps byte-identical to the
reference components line (verified with `cmp`).

Old baseline, main code, 4 nodes / 480 PEs (`results-grid-ab/n4_G0.rep*`):

| rep | reset | register | phaseA | phaseB | phaseB_s min/avg/max | phase1 |
|---|---|---|---|---|---|---|
| 1 | 0.034 | 0.013 | 0.396 | 0.349 | 0.000/0.021/0.348 | 0.764 |
| 2 | 0.023 | 0.027 | 0.370 | 0.338 | 0.000/0.019/0.338 | 0.744 |
| 3 | 0.001 | 0.001 | 0.339 | 0.330 | 0.000/0.016/0.330 | 0.666 |

Job 19529529, pool (`results-poolab/n4_pool.rep*`):

| rep | reset | register | phaseA | phaseB | phaseB_s min/avg/max | phase1 |
|---|---|---|---|---|---|---|
| 1 | 0.002 | 0.001 | 0.271 | 0.150 | 0.000/0.014/0.147 | 0.400 |
| 2 | 0.001 | 0.001 | 0.274 | 0.154 | 0.002/0.015/0.151 | 0.402 |
| 3 | 0.001 | 0.001 | 0.274 | 0.150 | 0.002/0.015/0.148 | 0.401 |

Reading (provisional, pending 19529535): phaseB max ~0.33-0.35 -> ~0.15, and
the phaseB_s max collapses onto the stage wall in both, i.e. the stage is one
straggler PE either way; pool halves that straggler. reset/register show NO
inflation at scale (item 3: expected result confirmed) — they sit at 0.001-0.002 s.

## How to finish

    squeue -j 19529535            # or: sacct -j 19529535
    cat /anvil/projects/x-asc050025/x-lkale/software/clusterfinding/results-poolab/ab-19529535.log

Then compare, per variant across the 3 interleaved reps:

    cd /anvil/projects/x-asc050025/x-lkale/software/clusterfinding/results-poolab
    for f in ab_main.rep*.log ab_pool.rep*.log; do echo "-- $f"; \
      grep -E '^FOF3STAT (time_s: phase1_stages|balance: phaseA_s|components)' $f; done

Correctness gate on the new logs: every `FOF3STAT components:` line must equal

    23707197 max_size 1519203 log2_histogram: 0:18867099 1:3692940 2:775348 3:221410 4:79607 5:34408 6:17022 7:9013 8:4691 9:2569 10:1399 11:753 12:405 13:237 14:130 15:69 16:51 17:24 18:12 19:7 20:3

If the job died or the allocation never came, resubmit:

    sbatch /anvil/projects/x-asc050025/x-lkale/software/clusterfinding/run-poolab2.sbatch

The staged binaries are what it runs, so no rebuild is needed to resubmit.

## Environment reminders that mattered here

- `source $PROJECT/x-lkale/software/recharm/env.sh` first; `CHARM_HOME=$RECHARM/charm`.
- `--cpus-per-task=15` is mandatory or every timing is meaningless.
- `-p wholenode -N 4`; `shared` is capped at 1 node.
- unionfind/htram untouched by this branch — no rebuild there.
