# S3 transport A/B at 2B / 16 nodes — arena rebuild vs per-node malloc

Frontier, 2026-08-13 afternoon. Executes `design/frontier-s3-transport-ab.md`
(spec `e7de95e`), following `design/phaseab-balancing.md` §30–31 and both §31
addenda.

**STATUS: COMPLETE, §1–§25. 41 of 41 2B runs exact (424,897,832) and 14 of 14
10k gates at 3549**, across jobs 5255130 / 5255137 / 5255613 / 5255652 /
5255653 / 5255724 / 5256055 / 5256164. Queue empty. Only open item: the
§12 analyses them with a reader written for the purpose.

**Bottom line up front:**

1. **The transport change (`b797e73`) works, and it is not the lever.** It
   cuts `s3Shipment` by a stable **13–16% per shipped unit** — measured at two
   grant sizes — but end-to-end it is **not distinguishable from noise**: in
   the corrected A/B the single best run of the job is a *prewire* run, and
   prewire's own rep-to-rep spread (0.366 s) is four times the prewire-to-wire
   mean gap (0.081 s). Shipped volume, fan-out, helper staggering and the
   straggler are all unchanged. This is the spec's own "flat" branch —
   **the binding constraint is elsewhere.**
2. **§31's addendum diagnosis was ~15% right.** `s3Shipment` was called
   allocator-bound; one arena allocation plus a linear placement pass removed
   one seventh of it. The remaining ~99 ms/call at ~500 units/grant is
   unexplained, and should be instrumented before option (A) (lazy per-tree
   rebuild) is built on the same assumption.
3. **`FOF_S3_GRANT_M2=1e10`, shipped this morning, should be raised to
   `1e11`.** It costs ~1.05 s on the straggler (**37%**), leaves a third of
   the movable work unmoved, and doubles protocol overhead-to-value. **The
   error is mine** — my previous report's budget number came from a
   PARTS=32/GRANT=32 ladder while its recommended cell (PARTS=16/GRANT=128)
   was only ever measured at 1e11; the two were adopted together but never
   tested together. §6.
4. **Next lever, on this evidence: §26 lever 3** (multiple outstanding grants
   per helper). Helper staggering is the one thing that got *worse* at the
   correct budget, the order→ship→rebuild→drain→return chain is untouched by
   transport, and `s3ShipOrder` — the largest single S3 cost, on the *donor's*
   critical path — is unaffected by anything done so far.

Two conclusions in §5 were later corrected by §7 once the budget confound was
found; both corrections are marked in place rather than rewritten away.

Companion report: `~/software/reports/s3-reserve-2b.md` (this morning's
grant-m2 / reservation verdict, now §30 of the campaign doc).

---

## 1. Code

```
cd ~/software/paratreet2 && git fetch && git checkout phaseab-campaign && git pull
```

Landed on **`957f2d18417ea0674cf453c04e3f401b3fb2a0ce`** — "Merge branch 'main'
into phaseab-campaign". The tree was 17 commits behind and fast-forwarded.
Queue was empty at pull time, per the standing pull policy.

The spec's "4600a60 or later" is satisfied — `4600a60` is an ancestor of the
tip, as is the task's stated `957f2d1` (they are the same lineage; `957f2d1`
is 3 commits later).

### The A/B is clean — transport is the only delta

`d5c24f4..HEAD` contains exactly one code commit:

| commit | subject |
|---|---|
| `957f2d1` | Merge branch 'main' into phaseab-campaign |
| `e7de95e` | spec: Frontier S3 transport A/B *(doc only)* |
| `4600a60` | Merge branch 'main' into phaseab-campaign |
| `d9e9994` | campaign: §31 addendum *(doc only)* |
| **`b797e73`** | **S3 transport: offset-carrying wire format + arena rebuild** |

Code diff over that range, excluding `design/`:

```
 fof/FoFPhase1.h     | 111 +++++++++++++++++++++++++--------------
 fof/FoFStealTypes.h |  25 +++++++--
 2 files changed, 86 insertions(+), 50 deletions(-)
```

I verified the knob defaults are **byte-identical at both commits**, so the
A/B isolates transport rather than confounding it with the morning's defaults
change (`1996ebd`, which is an ancestor of `d5c24f4`):

| knob | d5c24f4 | 957f2d1 |
|---|---|---|
| `FOF_S3_GRANT_UNITS_PER_PE` | `128` | `128` |
| `FOF_S3_GRANT_M2` | `1e10` | `1e10` |
| `FOF_S3_RESERVE` | off (`e && atoi(e)!=0`) | off (same) |
| `FOF_S3_RESERVE_FACTOR` / `_FRAC` | 2.0 / 0.5 | 2.0 / 0.5 |

Symbol-level confirmation that the two binaries really differ in transport:
`paratreet::StealTree<FragData>::WireNode` is present in `wire` and absent
from `prewire`.

---

## 2. Build — two staged binaries

Production charm throughout
(`CHARM_HOME=$HOME/software/charm/reconverse-linux-x86_64`, pinned
`3d1fdd89f`). Script: `~/software/build-transport-ab.sh`.

`htram`, `unionfind/prefixLib` and `unionfind` are unchanged between the two
commits and were already production-linked from this morning's rebuild, so
per the spec only `paratreet2/{src,fof,examples/fof3}` were clean-relinked for
each commit — `make clean` at every stage (no header dependency tracking).

| staged binary | commit | TraceSummary | md5 |
|---|---|---|---|
| `~/software/FoF3.2b.wire` | `957f2d1` | **0** | `dfd3d52f4cff0d778c2cf5e620a773a7` |
| `~/software/FoF3.2b.prewire` | `d5c24f4` | **0** | `2eeb7470bc73b61fbdf935101b690e30` |

Tree returned to `phaseab-campaign` / `957f2d1` afterwards.

**Build reproducibility caveat.** Rebuilding `957f2d1` a second time produced
a *different* md5 (`0e1630ed…`) with an **identical symbol set**. So the build
is not byte-reproducible — some build stamp varies — and md5 equality cannot
be used as a "same code" proof here, only md5 *inequality* as a "different
binary" proof. It does not affect the A/B: `wire` and `prewire` were built
back to back in one script run under identical conditions.

### Traced pair (the optional second job)

`tracedcharm` was intact at `90f05d8cb` (the self-closing shutdown fix), so
the traced stack was convenient. Because `htram`/`unionfind` were
production-linked, the **whole** stack had to be relinked against tracedcharm
first (BUILDS.md: partial rebuilds silently relink stale libraries), then
relinked back. Script: `~/software/build-transport-traced.sh`.

| staged binary | commit | TraceSummary | closeSummaryOnPe | md5 |
|---|---|---|---|---|
| `~/software/FoF3.2b.wire.sumd` | `957f2d1` | 524 | 28 | `3c0a1c97c0e2…` |
| `~/software/FoF3.2b.prewire.sumd` | `d5c24f4` | 524 | 28 | `45d024c3f4a8…` |

`closeSummaryOnPe = 28` on both confirms the reconverse trace-shutdown fix is
linked in — without it `+sumDetail` writes zero files (the 5248902 incident).

**The app tree was restored to production charm afterwards** and re-verified
(`TraceSummary = 0`), so the next session inherits a production tree, as
BUILDS.md expects. Rebuilding in place was safe for the queued A/B because
that job runs the *staged* copies, never `examples/fof3/FoF3`.

---

## 3. Jobs

| job | what | runs | §
|---|---|---|---|
| 5255080 | first A/B attempt | **FAILED in 4 s — my bug** | §3a |
| 5255130 | untraced transport A/B @ default 1e10 | 5 + 2 gates | §4 |
| 5255137 | sum-detail pair @ default 1e10 | 2 + 2 gates | §5 |
| 5255613 | `GRANT_M2` ladder at best cell + base anchor | 7 | §6 |
| 5255652 | untraced transport A/B @ **1e11** | 5 + 2 gates | §7a |
| 5255653 | sum-detail pair @ **1e11** | 2 + 2 gates | §7b |
| 5255724 | `1e11` vs `1e12` head-to-head, interleaved | 6 | §9 |

Every 2B run in every job above was exact at 424,897,832; every 10k gate at
3549. Traces: `~/software/sumd2b-transport-frontier.tar.gz` (@1e10, 50 MB) and
`~/software/sumd2b-transport11-frontier.tar.gz` (@1e11).

### 3a. Job 5255080 — a scripting bug, not a code or machine fault

Recorded because it is in the job history and would otherwise look like a
transport failure. The batch script died after the divergence guard with

```
slurm_script: line 98: label: unbound variable
```

Cause, mine: `local label=$1 bin=$2 log=$OUT/gate10k-$label.log`. Bash expands
**all** arguments of the `local` builtin before any of its assignments take
effect, so `$label` was still unset when referenced, and `set -u` aborted.
`bash -n` cannot catch this. Fixed by splitting the declaration; I then
grepped the whole script (and the traced one) for the same pattern and
smoke-tested both end to end against a stubbed `srun` before resubmitting. The
failure cost 4 seconds and no 2B run.

---

## 4. Untraced A/B — job 5255130 (5 runs + 2 gates, 1 min 53 s)

10k gates: **prewire 3549, wire 3549**. All five 2B runs **exact at
424,897,832**. Divergence guard clean.

### Exact srun line (every 2B run)

```
srun -N 16 --ntasks-per-node=8 -t 10:00 \
     --mpi=cray_shasta --network=job_vni --unbuffered \
     --cpu-bind=none --distribution=block:block \
     $HOME/software/FoF3.2b.{wire,prewire} \
     -f /lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024 \
     -d oct -u serial +traceoff +ppn 14 \
     +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
     +lci_ndevices 7 +backend_poll_thread 2
```

Env, identical on every arm:
`FOF_PB_PARTS=16 FOF_PHASEB_SLICE_MS=2 FOF_STEALA=1 FOF_STEALA_GEO=1
FOF_PB_M2KEY=1 FOF_PROCS_PER_PNODE=8`, with `GRANT_M2` / `GRANT_UNITS_PER_PE`
/ `RESERVE` left at their new commit defaults (1e10 / 128 / off).

| arm | binary | phaseA | phaseB | **phaseB_s max** | Pre-trav | Iter0 | out_ships | units/ship | out_m2/tot_m2 | declines |
|---|---|---|---|---|---|---|---|---|---|---|
| base-wire | wire | 1.956 | 3.397 | 3.371 | 5.545 | 8.219 | — | — | — | — |
| s3-prewire-1 | prewire | 1.974 | 3.012 | 2.798 | 5.552 | 8.228 | 412 | 284.9 | 26.2% | 734 |
| s3-wire-1 | wire | 1.980 | 2.843 | 2.743 | 5.188 | 7.882 | 432 | 303.2 | 30.9% | 676 |
| s3-prewire-2 | prewire | 1.959 | 2.897 | 2.865 | 6.578 | 9.240 | 408 | 340.1 | 25.6% | 686 |
| s3-wire-2 | wire | 1.960 | 2.938 | 2.902 | 5.400 | 8.083 | 434 | 277.6 | 27.7% | 751 |

Arm means (S3 arms only):

| metric | prewire | wire | delta |
|---|---|---|---|
| phaseB | 2.955 | 2.891 | −2.2% |
| **phaseB_s max** | **2.832** | **2.823** | **−0.3% (nil)** |
| Pre-traversal | 6.065 | 5.294 | −12.7% |
| Iteration 0 | 8.734 | 7.982 | −8.6% |
| units/ship | 312.5 | 290.4 | −7% |
| out_m2/tot_m2 | 25.9% | 29.3% | +3.4 pt |

**Verdict on the doc's hypothesis: the flat branch.** The spec framed it as
"if the serial rebuild was throttling grant turnaround, s3-wire ships MORE
volume (higher out_m2/tot_m2, lower declines) at similar or better wall; if
numbers are flat, the binding constraint is elsewhere." Shipped volume is
within run-to-run scatter (units/ship overlaps in both directions; out_m2
+3.4 pt with the arms interleaved), and **the straggler does not move at all**
(2.832 → 2.823). The wall-clock rows favour wire, but `s3-prewire-2` carries a
Pre-traversal outlier (6.578 vs 5.552 for its twin) that inflates the prewire
mean; dropping it leaves Pre-traversal 5.552 vs 5.294, about 5%.

So: **transport is not the binding constraint.** §5 measures why directly.

### 4a. The prewire arm does NOT reproduce the spec's expected reference

The spec predicted the prewire arm would land on job 5253475's best-noreserve
(`phaseB_s max` 1.572/1.637, Iter0 7.138/7.182, 566/551 units-per-ship, 38% of
pool m2). It did not, and not by a little:

| metric | 5253475 best-noreserve | 5255130 prewire | gap |
|---|---|---|---|
| phaseB_s max | 1.572 / 1.637 | 2.798 / 2.865 | **+76%** |
| Iteration 0 | 7.138 / 7.182 | 8.228 / 9.240 | +22% |
| units/ship | 566.4 / 550.9 | 284.9 / 340.1 | **−45%** |
| out_m2/tot_m2 | 38.4% / 37.6% | 26.2% / 25.6% | **−12 pt** |

This is not machine drift: today's `base-wire` (Pre-trav 5.545) is only ~4%
off this morning's base arms (5.302 / 5.335), while the prewire arm is 25% off
its reference.

The cause is a configuration difference, not the transport. The only code
commit between `0f30988` (which built 5253475's binary) and `d5c24f4` is
**`1996ebd`, the defaults change itself**. Job 5253475 set
`FOF_S3_GRANT_M2=1e11` **explicitly**; `d5c24f4` and later **default it to
1e10**. Grants at the best cell are ~4x larger than at the PARTS=32 cell the
ladder explored, so they need a proportionally larger budget — my
`s3-reserve-2b` report recommended "5e9–1e10" from the **PARTS=32 /
GRANT_UNITS=32** ladder while separately recommending the **PARTS=16 /
GRANT_UNITS=128** cell, which I had only ever measured at 1e11. Those two
recommendations were never tested together.

That is exactly the caveat that report flagged and that §30's addendum carries
as an open TODO ("express the grant m2 budget relative to measured pool m2
rather than any bare constant"). It bit on the first job after adoption.
**Job 5255613 (§6) tests it directly.**

Consequence for this A/B: both arms ran at roughly half the optimal grant
size. That weakens the transport test in the direction of *understating* wire,
since a smaller shipment means a shorter serial rebuild to remove.

---

## 5. Sum-detail pair — job 5255137 (2 runs + 2 gates, 1 min 21 s)

Both 10k gates 3549; both 2B runs exact. 3585 trace files each, **no mid-run
flush** in either (so the timings are trustworthy). Traces packaged at
`~/software/sumd2b-transport-frontier.tar.gz` (50 MB).

Analysed with the new `.claude/skills/sumdetail-analysis/` tool. **Validation
first:** re-running it on the 5250364 trace still on disk reproduced §31's
published figures exactly — `s3Shipment` 27.80 s / 473 calls / 58.8 ms,
`s3ShipOrder` 29.88 s / 4569 / 6.5 ms, `drainForeign` 55.93 s, `s3Return`
0.17 s. So the numbers below are from a tool verified against a known answer.

### 5a. Entry-method costs, three generations

| entry | 5250364 `b210b6f` GRANT=32 | prewire `d5c24f4` GRANT=128 | wire `957f2d1` GRANT=128 |
|---|---|---|---|
| `s3Shipment` (helper rebuild) | 27.80 s / 473 / **58.8 ms** | 35.43 s / 413 / **85.8 ms** | 30.55 s / 417 / **73.3 ms** |
| `s3ShipOrder` (donor collect) | 29.88 s / 4569 / 6.5 ms | 37.87 s / 2461 / **15.4 ms** | 37.67 s / 2465 / **15.3 ms** |
| `drainForeign` (stolen exec) | 55.93 s / 18016 / 3.1 ms | 44.42 s / 16824 / 2.6 ms | 47.18 s / 17369 / 2.7 ms |
| `s3Return` | 0.17 s / 473 | 0.16 s / 413 | 0.18 s / 417 |
| `s3Report` / `s3Publish` / `s3Drained` / `s3Declined` | — | 0.75 / 0.00 / 0.01 / 0.02 s | 0.76 / 0.00 / 0.01 / 0.02 s |

Control plane is free, as the roster expects. The data plane is the story.

### 5b. THE HEADLINE: the arena rebuild delivers ~15%, not the serial head

`s3Shipment` **85.8 → 73.3 ms/call, −14.6%** (total 35.43 → 30.55 s, −13.8%).
Normalised per shipped unit, since the two runs shipped slightly different
volumes (301.6 vs 306.1 units/ship):

| generation | ms per shipped unit |
|---|---|
| 5250364 (per-node malloc, GRANT=32) | 0.267 |
| prewire (per-node malloc, GRANT=128) | 0.284 |
| **wire (offset wire + arena, GRANT=128)** | **0.239** |

**−15.8% per unit.** Real, reproducible in the right direction, and far short
of what §31's addendum aimed at. The addendum's diagnosis was that the entry
was "allocator-bound" — "one `new FullNode` per node, allocator-bound". If
per-node allocation were the dominant term, replacing it with a single arena
allocation plus a linear placement-new pass should have removed most of the
85.8 ms. It removed one seventh. **The remaining ~73 ms/call is something else
inside `s3Shipment`** — candidates, none yet measured: message deserialization
(the pup itself), the linear placement pass being memory-bandwidth/cache
bound rather than allocator bound, unit enqueueing, or the PE wake fan-out.

Note also that `s3Shipment` scales close to linearly with grant size across
generations at fixed transport (58.8 ms at ~220 units/ship → 85.8 ms at 301.6,
i.e. 1.46x for 1.37x the units). The addendum's prediction that the serial
head "grows toward 0.25–1.7 s per grant" at GRANT=128 did **not** materialise
— it grew to 86 ms, because grants at the shipped 1e10 default are half the
size they should be (§4a).

### 5c. Donor-side collection is now the largest protocol cost

`s3ShipOrder` more than doubled per call (6.5 → 15.4 ms) while calls nearly
halved (4569 → 2461), so the **total rose 29.9 → 37.9 s** and it has overtaken
`s3Shipment` as the single most expensive S3 entry. The wire change does not
touch it (37.87 vs 37.67 s — flat, as expected: the flatten side changed
format but not cost).

This matters more than the rebuild, because §31 established that
`s3ShipOrder` runs **on the donor** — critical-path time on the hottest
process — whereas the rebuild is paid by an otherwise-idle helper.

### 5d. Protocol overhead per unit of work moved has got worse

§31 measured "~1 second of overhead per second of work moved" at GRANT=32.
Summing donor collect + helper rebuild + return against stolen execution:

| generation | overhead (order+ship+return) | value (drainForeign) | ratio |
|---|---|---|---|
| 5250364 GRANT=32 | 57.85 s | 55.93 s | **1.03 : 1** |
| prewire GRANT=128 | 73.46 s | 44.42 s | **1.65 : 1** |
| wire GRANT=128 | 68.40 s | 47.18 s | **1.45 : 1** |

Bigger grants were supposed to amortise the ~125 ms/grant fixed costs (§31
lever (a)). At the shipped defaults they appear instead to have made the
protocol *less* efficient per unit of work moved.

> **CORRECTION (added after §7): this conclusion is an artifact of the wrong
> `GRANT_M2`, not a property of GRANT=128.** At the corrected budget the ratio
> returns to **0.97:1 (prewire) and 0.93:1 (wire)** — better than §31's
> 1.03:1. Bigger grants *do* amortise as lever (a) predicted; the 1.65:1 above
> is what happens when the budget clips grants to half size while the
> per-grant fixed costs stay. See §7c. The rows above stand as measurements,
> but do not read them as a verdict on grant size.

### 5e. What did NOT change

- **Fan-out is fine and unchanged.** `drainForeign` runs on 13.1/14 PEs per
  process (prewire) and 13.5/14 (wire), top-PE share 0.12 / 0.11 — §31's
  13.4/14 finding reproduces. The parallel foreign drain is not the problem.
- **The straggler is the same process.** `phaseBChained` top process is
  **proc 55** in both, 34.11 s vs 34.05 s of local phaseB exec, top/median
  21.5x vs 20.7x. Identical to §31's proc-55 finding (23.9 s at GRANT=32,
  14.9x). Transport does not touch it.
- **Helper arrival staggering is unchanged.** First-`drainForeign` spread
  1808 ms (prewire) vs 1689 ms (wire). (Absolute offsets differ — 19.8 s vs
  10.2 s — purely because the prewire run's 76 GB load took 31 s of wall
  against wire's 19 s; that is I/O variance, not phaseB.) §31's
  order→ship→rebuild→drain→return serialization (lever 3) is untouched, which
  is consistent with §5b: removing one seventh of one stage of that chain
  cannot unstagger it.

---

## 6. Confound check — job 5255613: the shipped `GRANT_M2=1e10` default is ~half of what the best cell wants

7 runs, all exact. `wire` binary throughout, PARTS=16, `GRANT_UNITS_PER_PE`
and `RESERVE` at defaults, `GRANT_M2` laddered and interleaved. A no-S3 base
anchor tests machine drift.

| arm | phaseB | **phaseB_s max** | Pre-trav | Iter0 | out_ships | units/ship | out_m2/tot_m2 | m2/ship |
|---|---|---|---|---|---|---|---|---|
| base (S3 off) | 3.383 | 3.357 | 5.521 | 8.224 | — | — | — | — |
| **1e10 — SHIPPED DEFAULT** | 2.865 | **2.839** | 5.051 | 7.731 | 416 | 299.4 | 27.7% | 4.01e9 |
| **1e10 — SHIPPED DEFAULT** | 2.762 | **2.735** | 4.937 | 7.622 | 433 | 308.3 | 30.8% | 4.36e9 |
| 3e10 | 2.449 | 2.309 | 5.298 | 7.977 | 350 | 388.4 | 34.3% | 5.85e9 |
| 1e11 | 2.004 | **1.803** | 4.499 | 7.202 | 314 | 520.0 | 40.8% | 7.85e9 |
| 1e11 | 1.898 | **1.715** | 4.602 | 7.332 | 327 | 479.4 | 40.2% | 7.38e9 |
| 1e12 | 1.653 | **1.578** | 4.621 | 7.366 | 295 | 511.8 | 42.4% | 8.52e9 |

Monotonic, large, and reproducible across interleaved reps.

- **Machine drift is ruled out.** The base anchor (Pre-trav 5.521, phaseB_s
  max 3.357) matches this afternoon's `base-wire` (5.545 / 3.371) and this
  morning's bases (5.302 / 5.335) to within a few percent. The spread below is
  the knob, not the machine.
- **The shipped default costs ~1.05 s on the straggler.** 1e10 → 2.735–2.839;
  1e11 → 1.715–1.803. A **37% regression** on the campaign's headline metric,
  against the value job 5253475 actually measured.
- **It also leaves a third of the movable work unmoved:** `out_m2/tot_m2`
  27.7–30.8% at 1e10 vs 40.2–40.8% at 1e11; `units/ship` 299–308 vs 479–520.
- **The knee is at ~1e11, and above it the budget stops binding.** 1e12 gives
  511.8 units/ship against 1e11's 479–520 — the same within scatter — so
  beyond ~1e11 the partition size and the 1792-unit count cap take over, which
  is the regime the budget is *supposed* to sit just above. 1e12's 1.578 s
  reproduces job 5253475's best-noreserve 1.572 s exactly.

### Why the recommendation went wrong — my error, not the adopter's

My `s3-reserve-2b` report made two recommendations that were never tested
together:

- rec 1: "the knee sits between 1e9 and 5e9; **5e9–1e10** restores v2
  behaviour" — derived from the ladder at **PARTS=32 / GRANT_UNITS=32**;
- rec 4: adopt **PARTS=16 / GRANT_UNITS=128**, which I only ever measured at
  **1e11**.

At the rec-4 cell grants are ~4x larger and need a ~4x larger budget. The
rec-1 constant was carried to a cell it was never measured at. `1996ebd`
adopted both faithfully; the *combination* is what is wrong, and that is on
the report, not on the adoption.

The same report's rec 1 also warned the budget should be "expressed relative
to measured pool m2 rather than as a bare constant, or it will silently
re-break at the next problem size", and §30's addendum carries it as an open
TODO. It did not need a new problem size — changing the *cell* was enough.

**Recommendation: raise the `FOF_S3_GRANT_M2` default to `1e11`**, and treat
the relative formulation as the actual fix — e.g. `k x mean_unit_m2 x
count_cap` with k ~ 1, which at this pool (mean 2.67e6 m2/unit, cap 1792)
lands near 5e9 per count-cap-sized grant and scales with both the pool and the
cap instead of clipping silently whenever either grows.

### Consequence for the transport A/B

§4 and §5 measured transport at **half the intended grant size**. §31's
addendum predicts the serial rebuild grows with grant size, so the transport
benefit should be *larger* at the correct budget — the A/B as run understates
wire. Jobs **5255652** (untraced A/B) and **5255653** (sum-detail pair) re-run
both at `GRANT_M2=1e11`; results in §7.

---

## 7. The corrected A/B — jobs 5255652 / 5255653, at `GRANT_M2=1e11`

Identical to §4/§5 in every respect except the budget. Both 10k gates 3549 in
each job; **all 7 2B runs exact**; traces 3585 files each, no mid-run flush.
Traces packaged at `~/software/sumd2b-transport11-frontier.tar.gz`.

### 7a. Untraced, job 5255652

| arm | binary | phaseA | phaseB | **phaseB_s max** | Pre-trav | Iter0 | out_ships | units/ship |
|---|---|---|---|---|---|---|---|---|
| base-wire | wire | 1.920 | 3.349 | 3.323 | 5.456 | 8.152 | — | — |
| s3-prewire-1 | prewire | 1.913 | 1.735 | **1.535** | 4.370 | 7.082 | 307 | 502.3 |
| s3-wire-1 | wire | 1.984 | 1.826 | 1.608 | 4.370 | 7.054 | 338 | 458.7 |
| s3-prewire-2 | prewire | 1.943 | 2.106 | 1.901 | 4.731 | 7.436 | 303 | 477.8 |
| s3-wire-2 | wire | 1.955 | 1.845 | 1.666 | 4.398 | 7.091 | 331 | 474.2 |

| metric | prewire mean | wire mean | delta |
|---|---|---|---|
| phaseB_s max | 1.718 | 1.637 | −4.7% |
| Pre-traversal | 4.551 | 4.384 | −3.7% |
| Iteration 0 | 7.259 | 7.073 | −2.6% |

**The A/B verdict does not change: still flat.** Wire's mean is nominally
better on all three rows, but **the best single run of the whole job is a
prewire run** (1.535), and prewire's own two reps span 1.535–1.901 — a spread
(0.366 s) four times the prewire-to-wire mean gap (0.081 s). Run-to-run
scatter dominates the effect. With 2 reps per arm this is not a measurable
end-to-end win.

The config itself is validated: 1.535–1.901 reproduces job 5253475's
best-noreserve 1.572/1.637, and confirms §6 — the same binaries at the shipped
1e10 default gave 2.798/2.865.

### 7b. Traced pair, job 5255653 — `s3Shipment` at the right grant size

| entry | prewire (496.5 units/ship) | wire (546.7 units/ship) |
|---|---|---|
| `s3Shipment` | 32.34 s / 310 / **104.3 ms** | 30.20 s / 304 / **99.4 ms** |
| `s3ShipOrder` | 34.78 s / 2358 / 14.7 ms | 37.46 s / 2352 / 15.9 ms |
| `drainForeign` | 69.70 s / 19714 / 3.5 ms | 73.02 s / 20850 / 3.5 ms |
| `s3Return` | 0.20 s / 310 | 0.18 s / 304 |

Per-call the gap is only **−4.7%**, but wire shipped 10% more units per grant,
so normalise:

| grant size | prewire ms/unit | wire ms/unit | delta |
|---|---|---|---|
| ~300 units (§5, 1e10) | 0.284 | 0.239 | **−15.8%** |
| ~500 units (here, 1e11) | 0.210 | 0.182 | **−13.4%** |

**The transport benefit is a stable ~13–16% of `s3Shipment`, and it does not
grow with grant size.** §31's addendum predicted the serial head would grow
toward 0.25–1.7 s per grant at GRANT=128 and that removing it would matter
more there. Neither happened: the head is 104 ms at ~500 units/grant, and the
arena rebuild takes the same fraction off it at both sizes. The remaining
~99 ms/call is still unexplained (§5b).

Note the per-unit cost *falls* with grant size (0.284 → 0.210 for prewire),
so `s3Shipment` does carry a real per-shipment fixed cost that bigger grants
amortise — it is simply not the allocator.

### 7c. Protocol overhead is fine at the correct budget — §5d corrected

| generation | overhead (order+ship+return) | value (drainForeign) | ratio |
|---|---|---|---|
| 5250364, GRANT=32 (§31) | 57.85 s | 55.93 s | 1.03 : 1 |
| prewire @ 1e10 (wrong budget) | 73.46 s | 44.42 s | 1.65 : 1 |
| wire @ 1e10 (wrong budget) | 68.40 s | 47.18 s | 1.45 : 1 |
| **prewire @ 1e11** | 67.32 s | 69.70 s | **0.97 : 1** |
| **wire @ 1e11** | 67.84 s | 73.02 s | **0.93 : 1** |

§5d's "overhead has got worse" was an artifact of the clipped budget. At the
correct budget, overhead per second of work moved is **better than §31's
baseline**, and §31 lever (a) (fewer/bigger grants amortise fixed costs) is
confirmed rather than contradicted. `drainForeign` — the value side — rises
44 → 70 s, which is the whole point: more work actually moved.

### 7d. And the straggler halves

| metric | @ 1e10 (§5e) | @ 1e11 |
|---|---|---|
| proc 55 local phaseB exec | 34.11 / 34.05 s | **19.46 / 17.76 s** |
| top/median process | 21.5x / 20.7x | **12.0x / 11.3x** |
| drainForeign fan-out | 13.1 / 13.5 of 14 | 13.7 / 13.5 of 14 |
| first-drainForeign spread | 1808 / 1689 ms | 2927 / 2827 ms |

The straggler is still proc 55 and still the same shape, but nearly half its
local work now gets stolen. Fan-out is unchanged and healthy. Helper arrival
staggering *widens* (1.7 → 2.9 s) — expected, since each grant now takes
longer to rebuild and drain, which is precisely §26 lever 3 (one outstanding
shipment per helper) and remains the untouched serialization.

---

## 8. Conclusions

1. **The transport change (`b797e73`) works and is worth keeping, but it is
   not the lever.** It cuts `s3Shipment` by a stable **13–16% per shipped
   unit** at both grant sizes tested. End-to-end it is **not measurable**
   against run-to-run scatter (best single run in the corrected A/B is a
   *prewire* run), and it moves nothing else: shipped volume, fan-out,
   staggering, and the straggler are all unchanged. This is the spec's own
   "flat" branch — **the binding constraint is elsewhere.**
2. **§31's addendum diagnosis was only ~15% right.** `s3Shipment` was
   described as allocator-bound; replacing per-node `new` with one arena
   allocation plus a linear placement pass removed one seventh of it. The
   other ~99 ms/call at 500 units/grant is unexplained and is the obvious next
   thing to instrument before building option (A) (lazy per-tree rebuild) on
   the assumption that the rebuild is the cost. Candidates: the pup itself,
   cache/bandwidth limits on the placement pass, unit enqueueing, PE wake
   fan-out.
3. **`FOF_S3_GRANT_M2=1e10` (shipped this morning) should be raised to
   `1e11`.** It costs ~1.05 s on the straggler (37%), leaves a third of the
   movable work unmoved, and doubles the protocol's overhead-to-value ratio.
   The error is mine: two recommendations from my previous report — a budget
   measured at PARTS=32/GRANT=32 and a cell measured only at 1e11 — were
   adopted together though never tested together. The durable fix is the one
   that report already flagged and §30's addendum carries as an open TODO:
   express the budget relative to measured pool m2 (e.g. `k x mean_unit_m2 x
   count_cap`, k ~ 1) instead of as a bare constant.
4. **Next lever, on this evidence: §26 lever 3 (multiple outstanding grants
   per helper).** Helper arrival staggering is the one thing that got *worse*
   at the correct budget (1.7 → 2.9 s spread), the order→ship→rebuild→drain→
   return chain is untouched by transport, and `s3ShipOrder` — 15–16 ms/call,
   ~37 s total, the largest single S3 cost and the one that runs on the
   *donor's* critical path — is unaffected by anything done so far. §31's
   lever (c) (get donor-side collection off the critical path) is the other
   half of the same story.
5. **Standing gap:** `phaseB_s max` 1.535–1.666 s against the 0.25 s
   granularity floor — still ~6x. Unchanged by transport; only the grant-size
   correction moved it.

---

## 9. `1e11` vs `1e12` — job 5255724, and a correction to §6

Kale's question on reading §6: does the ladder show 1e12 is better? The
ladder's 1e12 point was a single rep, so this settles it — 6 runs, interleaved
3+3, `wire` binary, best cell. All exact.

Pooling every run at this cell on the `wire` binary across jobs 5255613 /
5255652 / 5255724:

| `GRANT_M2` | n | phaseB_s max (all runs) | mean | range | units/ship |
|---|---|---|---|---|---|
| 1e11 | 7 | 1.803, 1.715, 1.608, 1.666, 1.609, 1.831, 1.503 | **1.676** | 1.503–1.831 | 453–513 |
| 1e12 | 4 | 1.578, 1.565, 1.609, 1.536 | **1.572** | 1.536–1.609 | 507–524 |

**Yes, modestly — about 6% on the mean — and the mechanism is a tail, not a
shift.** The floors are equal (1e11 holds the single best run of all, 1.503);
1e12's advantage is entirely that 1e11 occasionally produces 1.80–1.83 and
1e12 never does. The interleaved head-to-head splits 2–1 for 1e12 with one
rep reversing, so on its own it would be weak; pooled n=7 vs n=4 with the
mechanism below, it is real.

### Correction to §6

§6 argued the budget "essentially never binds at either setting" because
`units/ship` was the same. That is true of the mean and false of the tail:

- 1e11 dips to **453 and 477** units/ship on individual runs
- 1e12 holds **507–524** on every run

So **1e11 sits right at the knee** and clips grants some of the time, which
shows up as run-to-run variance rather than a mean shift. 1e12 is clear of it.

### And a correction to §6's proposed formula

§6 proposed `k x mean_unit_m2 x count_cap` with k ~ 1, using the **pool** mean
(2.67e6 x 1792 = 4.8e9). That is wrong by the same factor this campaign keeps
rediscovering: shipped units are **~5.9x costlier than pool-average**
(m2 per shipped unit 1.56e7 at an unbinding budget), because the coordinator
orders the costliest partition — the same effect as this morning's 7.1x
finding. Using the **shipped** mean, `1.56e7 x 1792 ~ 2.8e10`, and the budget
should sit a factor above that. That lands at ~1e11 — i.e. exactly the knee,
which is precisely why 1e11 is marginal and 1e12 is comfortable.

### Revised recommendation

| value | verdict |
|---|---|
| 1e10 (shipped) | **wrong** — 37% straggler regression, a third of movable work unmoved |
| 1e11 | minimum correct value; clears the mean but sits on the knee, tail to 1.83 |
| **3e11** | **suggested default** — clears the knee with margin, still ~35x a typical grant so the all-giants guard is real. NOT MEASURED. |
| 1e12 | best measured (1.572), but ~120x a typical grant — the guard is effectively disabled |

The guard matters: the m2 budget exists to stop the v1 all-giants scoop, so
the goal is the smallest value comfortably past the knee, not the largest
value that runs fast. 3e11 is the interpolation; it is a two-minute job to
measure if wanted.

---

## 10. Job 5256055 (`3e11`) — and §9's `1e12` conclusion does NOT survive it

7 runs, all exact. 3x `3e11` interleaved with `1e11` and `1e12` anchors **in
the same allocation**, so this is a within-job comparison.

| arm | phaseB_s max | units/ship | out_m2/tot_m2 |
|---|---|---|---|
| 3e11 rep1 | 1.642 | 474.0 | 40.2% |
| 1e11 anchor rep1 | 1.618 | 519.6 | 42.5% |
| 1e12 anchor rep1 | 1.721 | 533.8 | 41.9% |
| 3e11 rep2 | 1.737 | 500.9 | 39.3% |
| 1e11 anchor rep2 | 1.743 | 508.1 | 41.3% |
| 1e12 anchor rep2 | 1.716 | 485.3 | 43.6% |
| 3e11 rep3 | 1.665 | 462.1 | 43.6% |

Within-job means: **3e11 1.681, 1e11 1.681, 1e12 1.719.** All three are the
same, and here **1e12 is nominally the worst.**

### §9 was overfitted to one job — retracted

Within-job verdicts across all three jobs that contain both settings:

| job | 1e11 mean | 1e12 mean | favours |
|---|---|---|---|
| 5255613 (ladder) | 1.759 (n=2) | 1.578 (n=1) | 1e12 by 0.18 |
| 5255724 (knee) | 1.648 (n=3) | 1.570 (n=3) | 1e12 by 0.08 |
| **5256055 (this)** | **1.681 (n=2)** | **1.719 (n=2)** | **1e11 by 0.04** |

Pooled: 1e11 **1.677** (n=9, range 1.503–1.831); 1e12 **1.621** (n=6, range
1.536–1.721); 3e11 **1.681** (n=3). The 1e12 advantage shrank from 6% to 3.3%
as soon as two more 1e12 runs landed at 1.72, and one job reverses the sign.
**These three settings are not distinguishable.**

The proposed *mechanism* also fails. §9 argued 1e11 "sits on the knee" and
clips grants, citing units/ship dips to 453/477. In this job 1e11 gives
519.6/508.1 — no dip — while **3e11** gives 462.1 and 1e12 gives 485.3. The
low units/ship values are run-to-run variation in how partitions get consumed,
**not** the budget binding. §9's reading of them was wrong.

### Final recommendation on `FOF_S3_GRANT_M2`

**Raise the default from `1e10` to `1e11`. Nothing beyond that is justified.**

| value | verdict | evidence |
|---|---|---|
| 1e10 (shipped) | **wrong** — 37% straggler regression | n=2 at this cell, plus the ladder |
| **1e11** | **correct — adopt** | n=9, mean 1.677 |
| 3e11 | no better | n=3, mean 1.681 |
| 1e12 | no better; nominally worse in the one job that reps it twice | n=6, mean 1.621 |

1e11 is also the smallest of the three, which is the right tie-break: the m2
budget exists to stop the v1 all-giants scoop, so the least-slack value that
clears the regression preserves the most guard. My §9 suggestion of 3e11 is
withdrawn — it buys nothing and costs guard.

**Standing caution, third time this campaign:** every one of these numbers is
a bare constant tuned to *this* pool at *this* cell. The relative formulation
(§30 addendum's open TODO) remains the actual fix.

---

## 11. Job 5256164 — full projections traces captured (analysis pending a reader)

Kale's point: a regular Projections trace, confined to a PE subset, *can*
capture what §5b said sum-detail could not. Correct — event records carry
per-call timestamps **and message length**, so `s3Shipment` duration can be
regressed against bytes (pup/deserialize, shrunk by the offset wire format)
versus units/nodes (rebuild, attacked by the arena). No code change: it is a
`-tracemode projections` rebuild.

Binaries `FoF3.2b.{wire,prewire}.proj` (`TraceProjections=475`,
`TraceSummary=0`), built via `~/software/build-transport-proj.sh`; app tree
restored to production charm afterwards and re-verified.

Subset justified by stability: **proc 55 is the top straggler in all five
sum-detail traces** taken today (both @1e10, both @1e11, plus §31's 5250364)
across three configs, with proc 87 second in all of them. Traced their blocks,
`+traceprocessors 672-783,1120-1231` (procs 48–55 and 80–87) = 224 of 1792
PEs. Config `GRANT_M2=1e11`, matching the @1e11 sum-detail pair.

Both runs **exact**, no mid-run flush:

| run | phaseB | phaseB_s max | Pre-trav | Iter0 | units/ship | trace |
|---|---|---|---|---|---|---|
| proj-prewire | 1.834 | 1.656 | 4.386 | 6.888 | 551.3 | 184 `.log.gz`, 59 MB |
| proj-wire | 1.788 | 1.664 | 4.482 | 6.993 | 563.2 | 174 `.log.gz`, 56 MB |

Flat again, consistent with §7a. Packaged:
`~/software/proj2b-transport-frontier.tar.gz` (366 MB).

### Two operational findings

- **The skill's untested `+traceprocessors` RSS caveat: no OOM at 2B.**
  Recording 12.5% of PEs ran clean, wall 23 s / 17 s against 17–19 s untraced,
  ~1.8 MB per recorded PE. Whether that is because the flag avoids the RSS
  cost or merely because 224 PEs is small enough is not separated here, but
  the practical answer for a subset trace at 2B is: it works.
- **Do not `du` a traceroot immediately after `srun` returns.** My in-job
  measurement read 531 KB for the prewire traceroot; the real size is 59 MB.
  Trace writes happen at exit and are still landing on Lustre when the step
  ends. The tarball (366 MB) was the tell — larger than the two directories I
  had just "measured". Sizes in the table above were re-read afterwards.
- Also worth noting: 174–184 files against 224 requested PEs, and the range
  includes PE 0. Some in-range PEs evidently recorded nothing; PE 0 records
  regardless. Not investigated — it does not affect the hot blocks, which are
  all present.

### Status: capture done, analysis blocked on a reader

`sumd_tool.py` explicitly does not read `.log.gz`. Kale has a reader on the
laptop or about to land in git, so per his instruction I did **not** write
one. The data is captured and packaged; whichever reader arrives can be
pointed at `proj2b_prewire` / `proj2b_wire`.

The question these traces are meant to answer: **of `s3Shipment`'s ~99 ms/call
at ~550 units/grant, how much is bytes and how much is nodes?** That decides
which of three laptop-side changes is worth writing — and in particular
whether option (A) (lazy per-tree rebuild) is aimed at the right cost, since
the arena result already shows its premise is only ~15% right.

---

## 12. Projections analysis — `s3Shipment` is BYTES-BOUND (the 99 ms is explained)

Kale supplied the missing capability (full trace, PE subset) and pointed at
`github.com/charmplusplus/projections` for the format. §11's traces are now
analysed. **This answers §5b's open question and overturns §31's addendum
diagnosis outright.**

Reader written: `~/software/scripts/projlog_tool.py` (stdlib only;
`totals` / `calls` / `regress` / `entries`). Format verified two ways —
empirically from the traces, then against `ProjDefs.java` in the visualiser
repo, which confirms `2`/`3` = BEGIN/END_PROCESSING and `18`/`19` =
BEGIN/END_UNPACK. Layout:

```
BEGIN_PROCESSING:  2 mtype entry time event pe msglen recvTime [idx...]
END_PROCESSING:    3 mtype entry time event pe msglen time2
```

`$7` on the BEGIN record is **message length** — the lever sum-detail lacks.

### 12a. Duration is almost perfectly linear in message bytes

| run | calls | mean msglen | mean dur | **slope** | pearson **r** | intercept |
|---|---|---|---|---|---|---|
| prewire | 39 | 47,193 KB | 164.7 ms | **0.0034 µs/byte** | **0.999** | 0.33 ms |
| wire | 41 | 39,608 KB | 126.9 ms | **0.0031 µs/byte** | **0.997** | −0.60 ms |

Binned means hold the same slope across the whole range (1.4 MB → 226 MB), so
it is genuinely linear, not a fitted average over curvature.

**r = 0.999 with a ~zero intercept means message size explains essentially all
of `s3Shipment`.** There is no fixed per-shipment cost worth amortising and no
separable allocation-count term. It is a flat streaming cost of
~0.003 µs/byte ≈ **300 MB/s**.

### 12b. What the two changes actually bought

| effect | prewire → wire |
|---|---|
| bytes per shipment (the **wire format**) | 47,193 → 39,608 KB, **−16%** |
| µs per byte (the **arena rebuild**) | 0.0034 → 0.0031, **−9%** |
| combined mean duration | 164.7 → 126.9 ms, −23% |

So the offset wire format did the larger share, and the arena — the part §31's
addendum expected to dominate — bought 9%. Consistent with §7b's −13.4% per
shipped unit, measured independently by sum-detail.

### 12c. The messages are enormous, and that is the real finding

| | prewire | wire |
|---|---|---|
| median msglen | 36.1 MB | 27.9 MB |
| p90 | 90.3 MB | 70.1 MB |
| **max** | **225.9 MB** | **170.9 MB** |
| max duration | **777 ms** | **549 ms** |

A single grant ships up to **226 MB** and occupies one helper PE for **777 ms**
of uninterruptible entry time. At ~550 units per grant that is ~73 KB per
stolen unit — against `ret_edges` of only ~3,300 for the entire run.

### 12d. Charm's unpack is NOT the cost — it is all inside the entry

From the raw record window around one call (PE 1139):

```
18 10611543 1139                              BEGIN_UNPACK
19 10611544 1139                              END_UNPACK        <- 1 us
2 19 450 10611544 11864 1218 41019920 ...     s3Shipment BEGIN, msglen 41 MB
1  5 414 10743888 14336 1139 64 2             first drainForeign CREATION
```

**Charm unpacks a 41 MB message in 1 µs** — it arrives contiguous, nothing to
do. Then **132 ms elapse inside `s3Shipment` before the first `drainForeign`
is even created**, i.e. the helper's 14 PEs sit idle for that whole window.
This is Kale's "no helper PE starts until the entry ends", now timed.

### 12e. What this means for the levers — it flips the option (A) advice

§8 conclusion 2 said to instrument before building option (A) (lazy per-tree
rebuild), because the arena result showed the "allocator-bound" premise was
only ~15% right. That instrumentation is now done, and the answer **supports
(A)** — but for a different reason than §31 gave:

- the cost is **linear in bytes with no fixed term**, so it is perfectly
  divisible: spreading it across the helper's 14 PEs should give close to
  linear speedup of the serial head (132 ms → ~10 ms);
- it is **not** in charm's unpack, so it is inside code we control and can
  defer.

**One design caveat that the trace cannot settle.** `s3Shipment` takes a
marshalled parameter (`const StealShipment<Data>&`), so charm's generated
wrapper deserialises the whole 41 MB *before* user code runs — and that part
is inside the timed entry but *not* deferrable by (A) as described. Splitting
"marshalled deserialise" from "tree rebuild" within the entry is the one thing
that does need code (user bracket events, or switching to a raw message type).
If the deserialise is most of the 0.003 µs/byte, option (A) must also change
the entry signature to a custom message, not just defer the rebuild.

- Cheaper still, and orthogonal: **ship fewer bytes**. 73 KB per stolen unit
  to return ~3,300 edges is the number that stands out. Whether the shipped
  subtree representation can be thinned is a design question for the laptop,
  and it would multiply directly through a cost that is 100% byte-proportional.

### 12f. Operational notes on the reader

- ~28 s per 2B subset trace (174 files, 56 MB) per invocation, single-threaded.
- The tool lives outside the repo at `~/software/scripts/projlog_tool.py`; if
  it is useful it belongs next to `sumd_tool.py` in the skill.

---

## 13. Intra-node transport: LCI, not shared memory (Kale's question, 14:45)

**Answer: everything goes over LCI/libfabric/CXI. POSIX shared memory is
compiled out of this build.**

The generated config is decisive:

```
$ grep CMK_USE_SHMEM .../reconverse-build/src/converse_config.h
/* #undef CMK_USE_SHMEM */
```

reconverse *has* the machinery — `src/cmishm.cpp` (`shm_open`,
`CmiIpcManager`, per-physical-rank shared segments) and an XPMEM variant
behind `CMK_HAS_XPMEM`, selected in `cmishmem.cpp` — but `CMakeLists.txt:55`
declares `option(CMK_USE_SHMEM ... OFF)` and the production build never turned
it on. The `#ifdef CMK_USE_SHMEM` blocks in `convcore.cpp` and `scheduler.cpp`
are therefore dead.

### 13a. What that costs, measured

New `transit` subcommand: pair CREATION (sender) to BEGIN_PROCESSING
(receiver) by event id, giving true in-flight time.

| | pairs | in-flight p50 | p90 | max | implied p50 BW | same physical node |
|---|---|---|---|---|---|---|
| prewire | 35 | 24.4 ms | 60.8 ms | 100 ms | 1782 MB/s | **35 / 35** |
| wire | 36 | 27.1 ms | 77.5 ms | 581 ms | 1927 MB/s | **36 / 36** |

**Every single shipment is intra-node** — as it must be, since a coordinator
block is `FOF_PROCS_PER_PNODE=8` processes on one physical node — and every
one of them takes a fabric round trip at ~1.9 GB/s. That is **~27 ms of
transit on top of the ~91–127 ms of entry time**, and about **12 GB of
intra-node traffic per run** (304 shipments x 40.6 MB).

So the per-grant latency chain is roughly: ~27 ms transit → ~91 ms entry →
helper PEs finally wake. Neither part is compute on the stolen work.

### 13b. Test in flight

Building a third charm tree `~/software/charm-shmem` — same commit
(`3d1fdd89f`), same compiler (`/usr/bin/c++`, verified identical to
production's CMakeCache), `--with-cmake-args="-DCMK_USE_SHMEM=ON"` as the
**only** difference. Production is untouched, so its provenance survives.

Per Kale's instruction the first test is a small program, not the 2B app: the
converse `pingpong` benchmark, which sends PE0↔PE1 — with
`-N 1 --ntasks-per-node=2 +ppn 1` those are two processes on one physical
node, exactly the S3 case. Sweep 8 B → 64 MB so the 40 MB shipment size is
bracketed; the large end is the column that matters, small sizes are
latency-bound.

---

## 14. New instrument: bytes and work-per-byte per shipment

Kale, 14:58: *"do accumulate stats for bytes and m2 (hopefully you can
accumulate stats without needing to store every value)"*. Rationale from §12:
**selection sorts by m2 but the cost is 100% byte-proportional**, so grants
should eventually be picked by work *per byte*. Nothing could measure that.

Added to `fof/FoFPhase1.h` (not pushed — this is a local change for review):

- `s3_out_bytes`, `s3_out_nodes`, `s3_out_parts` — running atomics;
- `s3_wpb_min` / `s3_wpb_max` and a 28-bucket `log2(m2/byte)` histogram
  `s3_wpb_hist`, alongside the existing `s3_ship_hist`.

**No per-shipment values are stored** — all running accumulators, as asked.

`shipmentBytes()` computes the serialized size **without a `PUP::sizer`
pass**: `vector::size()` is O(1), so it is O(#trees), not O(#nodes). That
matters because it runs inside `s3ShipOrder`, which §31 identifies as
donor-side *critical-path* time — an instrument there must not cost anything.
It sums the pup'd members (no struct padding), which is validated against the
`msglen` field of the projections traces (ground truth: 74.2 KB per shipped
unit on the wire run).

New output line:

```
FOF3STAT s3_bytes: node N out_bytes B bytes_per_ship B/s nodes N parts P
                   bytes_per_unit B/u m2_per_byte_mean M
                   m2_per_byte_min m m2_per_byte_max M
FOF3STAT s3_wpb_hist: node N log2m2perbyte b:c ...
```

One compile-time trap hit and fixed: the histogram cannot be declared where
the other new members are, because an in-class array bound cannot
forward-reference `kUnitHistBuckets` (declared ~100 lines later). It sits with
the other histograms instead.

---

## 15. Job 5256808 — the byte instrument validated, and work-per-byte spans 8.8e7x

**Exactness holds with the code change**: 10k = 3549, both 2B runs
424,897,832. The Frontier tree is deliberately left uncommitted, so the
divergence guard flags `paratreet2 MODIFIED` in this job's own output.

### 15a. `shipmentBytes()` is accurate to −5.4%

| | bytes per shipped unit |
|---|---|
| `shipmentBytes()` estimate, aggregated | 70,155 B (**68.5 KB**) |
| projections `msglen` ground truth (wire) | 74,180 B (**72.4 KB**) |
| **error** | **−5.4%** |

The gap is charm's message envelope and pup framing, which the analytic sum
omits by construction. Good as a relative measure; ~5% low in absolute terms.
Total shipped: **11.07 GB per run**, confirming the ~12 GB estimated in §13a.

### 15b. Work-per-byte varies by EIGHT ORDERS OF MAGNITUDE

Across the 57 processes that shipped anything (mean m2 per byte):

| min | p10 | p50 | p90 | max | spread |
|---|---|---|---|---|---|
| 1.0e-05 | 0.087 | 15.1 | 117.4 | 880.8 | **8.8e7x** |

Even p10→p90 is **1350x**. Aggregate mean is 211.7 m2/byte.

**This is the quantitative case for Kale's proposal.** Selection currently
sorts by m2 while §12 shows the cost is 100% byte-proportional. Grants
therefore differ by orders of magnitude in what they buy per unit of cost,
and nothing in the ordering sees it. A work-per-byte criterion has more
headroom than any transport change measured today.

Caveat on the statistic: these are per-process means over that process's
shipments, not per-shipment values, so the true per-grant spread is wider
still. The `min` of 1.0e-05 is a process that shipped almost pure overhead.

### 15c. Histogram resolution — the flagged concern was real, and is fixed

The first version bucketed `log2(m2/byte)` unshifted, and **bucket 0 held 90
of 314 shipments (29%)** — everything below 1.0 collapsed into it, which given
a floor of 1e-5 is five orders of magnitude in one bin. Fixed by shifting:
`kWpbShift = 17`, so bucket `b` means `m2/byte ~ 2^(b-17)`, which places the
measured 2^-16.6 .. 2^9.8 range inside [0, 27] with headroom both ways. Still
no stored values.

---

## 16. SHMEM A/B (jobs 5256960 / 5257264 / 5257317) — and a 4x bandwidth cliff

`~/software/charm-shmem` built from the same commit `3d1fdd89f` with the same
compiler; `--with-cmake-args="-DCMK_USE_SHMEM=ON"` the only difference.
Verified: `#define CMK_USE_SHMEM` there, `/* #undef */` in production.

### 16a. Two bugs in the first attempt, both instructive

Job **5256960 aborted both arms** (rc=134) at
`backend_ofi.cpp:224`, `fi_domain()` returning ENOSYS.

1. **`--network=job_vni` on a single-node job.** `notes/lci-cxi-error.md`
   documents this exactly: `job_vni` provisions a VNI only for multi-node
   jobs, so on one node the CXI provider finds no rgroup/VNI. Single-node
   needs **`--network=single_node_vni`**. The predecessor's note saved the
   diagnosis.
2. **`LD_LIBRARY_PATH` silently invalidated the A/B.** The login profile sets
   it to `$HOME/software/charm/lib`, and the binaries link
   `libreconverse.so`/`liblci.so` **dynamically** — so `pingpong.shmem` was
   resolving **production's** libreconverse (confirmed with `ldd`). The SHMEM
   build would have been bypassed and the result would have read as "SHMEM
   changes nothing". The rerun sets `LD_LIBRARY_PATH` per arm **and refuses to
   report a number unless `ldd` proves the right library resolved.**

### 16b. SHMEM is a LOSS at the sizes that matter

Job 5257264 (8 B → 64 MB) and 5257317 (1 → 128 MB, x2):

| bytes | prod GB/s | shmem GB/s | speedup |
|---|---|---|---|
| 8 B – 2 KB | latency-bound | latency-bound | ~1.0x |
| 1 MB | 12.7 | 7.4 | 0.58x |
| 4 MB | 15.3 | 8.0 | 0.52x |
| 8 MB | 16.2 | 7.9 | **0.49x** |
| 16 MB | 17.3 | 18.2 | 1.05x |
| 32 MB | 4.2 | 3.1 | 0.73x |
| 64 MB | 4.2 | 4.4 | 1.06x |
| 128 MB | 4.2 | 4.5 | 1.07x |

**Shared memory is up to 2x SLOWER** from 128 KB to 8 MB, and only ~5–7%
faster at the 32–128 MB sizes shipments occupy. **Recommendation: do not
enable `CMK_USE_SHMEM`.** The hypothesis that intra-node traffic was paying a
needless fabric cost is not supported — LCI's intra-node path is already
faster than this shared-memory implementation over most of the range.

### 16c. THE REAL TRANSPORT FINDING: a 4x cliff between 16 MB and 32 MB

Present in **both** builds, so it is not a SHMEM artifact:

```
prod:   4 MB 15.3   8 MB 16.2   16 MB 17.3  |  32 MB 4.2   64 MB 4.2   128 MB 4.2
shmem:  4 MB  8.0   8 MB  7.9   16 MB 18.2  |  32 MB 3.1   64 MB 4.4   128 MB 4.5
```

Peak ~17 GB/s at 16 MB, collapsing to a flat ~4.2 GB/s at 32 MB and above —
**a 4.1x drop**. This explains the ~1.9 GB/s measured in flight for real
shipments (§13a): at a median 28–36 MB and a max of 171–226 MB, **every S3
shipment lives past the cliff.**

**Actionable, and it needs no new charm build:** cap shipment size at ~16 MB
and send a large grant as several messages. A 40 MB grant split 3 ways should
move at ~17 GB/s instead of ~4.2, i.e. transit ~27 ms → ~7 ms, and the helper
could begin on the first chunk rather than waiting for the whole grant — which
also attacks the 132 ms serial head of §12d from a second direction.

Caveats before building on it: the cliff was measured with a 2-process
pingpong at `+ppn 1`, not 8 processes x 14 PEs under contention; the cause is
unidentified (a rendezvous/registration threshold is the obvious suspect); and
chunking interacts with the marshalled-parameter deserialise noted in §12e.
Confirming it in-app is one cheap experiment — the byte cap is already a knob
in spirit, since `FOF_S3_GRANT_M2` and the count cap both bound grant size.

---

## 17. Grant packing: three levers, three negatives, and one real finding

Kale, 16:39: *"how about some runs with a new sorting metric? I think just
doing it by m2/bytes will be shortsighted. We don't want to ship tiny jobs
again."* Three knobs built (patch `0002`), all default-off, all measured.
**Every arm exact.** Jobs 5257740, 5257769, 5257964.

### 17a. The design, and why plain m2/byte would indeed be wrong

Kale's instinct was right, for a reason deeper than tiny jobs: **a unit's byte
cost is not a property of the unit.** `place()` dedups subtrees, so the first
unit to pull in a subtree pays for it and later units sharing it cost ~nothing.
Standalone per-unit density is not even well defined — it penalises the first
unit of a cluster and over-rewards its followers. The filter therefore prices
**marginal** bytes, and is evaluated *before* the ownership CAS so a rejected
unit stays unclaimed for the local drain.

### 17b. Results

| arm | phaseB_s max | units/ship | KB/unit | work moved |
|---|---|---|---|---|
| **baseline (all off)** | **1.52 – 1.71** | 478–562 | 65–71 | 0.41–0.46 |
| `GRANT_BYTES=16M` | 2.94 / 2.94 | 209 | 72 | 0.39 |
| `GRANT_BYTES=8M` | 3.04 | 110 | 79 | 0.33 |
| `DENSITY=0.25` | 1.88 | 426 | — | 0.59 |
| `DENSITY=0.5` | 2.07 / 2.17 | 252–258 | — | 0.61–0.64 |
| `DENSITY=1.0` | 2.36 | 140 | — | 0.57 |
| `SPAN_PARTS=2` | 1.72 / 1.71 | 807–869 | 80–87 | 0.36–0.40 |
| `SPAN_PARTS=4` | 1.85 | **1106** | 95 | 0.36 |
| `SPAN=4 DENSITY=0.25` | 1.79 | 967 | **33.3** | **0.55** |

**Baseline wins.** None of the three levers improves the straggler.

- **Byte cap: clear loss.** It halves units/grant and doubles grant *count*;
  ~125 ms/grant of fixed cost (§31) swamps the transport saving. 8M is worse
  than 16M, so this is *not* about the 16–32 MB cliff — it is grant count.
- **Density: works on its own terms, loses overall.** m2/byte roughly doubles
  (217 → 421 vs its matched control) and work moved rises to 0.55–0.64 — but
  as implemented it shrinks the grant.
- **Spanning: works mechanically, doesn't help.** 542 → 1106 units/grant,
  through the partition ceiling toward the 1792 count cap, straggler flat.

### 17c. Two predictions tested — one right, one wrong

A partition is a **KD-split spatial region**, key-ordered internally, so
dedup hits hard within one and less across two. Predicted: spanning buys
units at worse bytes/unit. **Right** — 64.9 → 95.0 KB/unit.

Predicted: density and spanning would *fight*, since the filter rejects units
bringing new subtrees, which is what a freshly spanned partition offers.
**Wrong, and interestingly so** — they compose. Filtering drains a partition
sooner, which triggers *more* spanning (1143 spans, the most of any arm), and
the filter then picks the dense units out of each new region: KB/unit **halves
to 33.3** at the best work-moved of any arm.

### 17d. RETRACTION: grant size is not the control variable

§17 earlier in this session reported `phaseB_s max` tracking
`log(units/shipment)` at **r = −0.775** across six size mechanisms, and called
grant size the control variable. **The spanning arms falsify it**: 1106
units/grant did not help. The correlation was confounded — every earlier
mechanism that shrank grants also changed work moved and grant count
together. Do not build on it.

### 17e. THE REAL FINDING: moving more work makes the straggler WORSE

Across the 14 arms without a byte cap:

```
work moved  vs  phaseB_s max      r = +0.693
```

| work moved | phaseB_s max |
|---|---|
| 0.355 | 1.708 |
| 0.426 | 1.705 |
| 0.454 | **1.520** |
| 0.549 | 1.794 |
| 0.607 | 2.165 |
| 0.645 | 2.071 |

Not merely a fitted correlation — §31 measured the mechanism independently:
**proc 55 spent 4.87 s inside `s3ShipOrder`, 20% of its own execution time**,
collecting grants mid-drain. Collection runs *on the donor*, which is the
hottest process, so every extra grant is critical-path time on exactly the
process S3 exists to relieve. Past a point, what the straggler pays to ship
exceeds what it saves.

**S3 is currently self-limiting: stealing harder is counterproductive until
donor-side collection comes off the critical path (§31 lever (c)).**

### 17f. What this implies for option (A)

Kale, 17:11: option (A) — move tree building out of the single-PE recipient —
*"will win orthogonally, so we have to be careful comparing results."* Agreed,
with one refinement from §17e: **(A) fixes the helper's ~100 ms rebuild, but
the term that correlates with the straggler is the donor's collection.** (A)
remains worth building — it kills the serial head and the
one-outstanding-shipment-per-helper limit — but on this evidence it should not
be expected to move `phaseB_s max` much on its own. §31 lever (c) attacks the
term that does.

**Comparison discipline this demands:** every packing result above is valid
only at *today's* per-grant cost. (A) changes that cost, so the packing arms
must be re-run on top of (A) with a no-(A) control in the same allocation.
Nothing in §17b should be carried forward as settled once (A) lands.

---

## 18. What "collection" actually costs — lever (c) is aimed at the wrong half

Kale, 18:21, asked what "collection" means and what §31 lever (c) is. The
answer required a measurement that changes the recommendation.

**"Collection" is `s3ShipOrder`**, which the entry roster defines as: *"scan
the partition range, CAS-claim unclaimed units, FLATTEN the deduplicated
subtrees (the serialization), send one s3Shipment."* So it is selecting AND
packing — and the roster already noted the odd ratio: **4,569 order calls
served only 473 ships.**

**§31 lever (c), verbatim:** *"cut donor-side collection cost off the critical
path — candidates: grant collection from a precomputed partition manifest
instead of a per-unit scan+CAS at order time, or servicing orders on the
donor's least-loaded PE."*

### 18a. `s3ShipOrder` is sharply bimodal — the scan is free, the flatten is not

From the projections traces (`projlog_tool.py calls`):

| | calls | p50 | p90 | p99 | max | total |
|---|---|---|---|---|---|---|
| prewire | 244 | **0.013 ms** | 101.2 ms | 540.5 ms | 857.6 ms | 6.93 s |
| wire | 232 | **0.015 ms** | 102.6 ms | 353.2 ms | 666.6 ms | 6.67 s |

The median call is **13 microseconds**. ~84% of calls scan, find nothing
claimable, and return for free. The entire 6.9 s sits in the ~16% that
actually build a shipment.

### 18b. The donor's flatten costs MORE than the helper's rebuild

Normalised per shipment (39 / 41 shipments in the traced subset):

| per grant | cost | runs on |
|---|---|---|
| **donor flatten (`s3ShipOrder`)** | **~178 ms** | **the straggler — critical path** |
| helper rebuild (`s3Shipment`) | ~165 ms | an idle helper PE |
| transit | ~27 ms | fabric |

**The donor-side flatten is the single largest critical-path cost per grant,
and it is slightly larger than the helper-side rebuild that option (A)
targets.** This is the mechanism behind §17e's r = +0.693: every additional
grant adds ~178 ms to the hottest process.

### 18c. Consequence: lever (c)'s stated candidates do not fit the measurement

- *"precomputed partition manifest instead of a per-unit scan+CAS"* — aimed
  at the scan, which costs **13 µs**. Would save essentially nothing.
- *"servicing orders on the donor's least-loaded PE"* — moves the flatten,
  but the straggler's 14 PEs are all busy draining, so it relocates the cost
  within a saturated process rather than removing it.

**The version the data supports is to precompute the FLATTEN, not the
manifest.** Partitions are known before any order arrives, so their
deduplicated subtrees can be flattened once — during idle time, or spread
across PEs — reducing `s3ShipOrder` to little more than handing over a
prepared blob. That is option (A)'s trick applied to the donor side, and on
this evidence it is the change most likely to move `phaseB_s max`.

Caveat: 39–41 shipments within a 224-PE subset is small n. The 13 µs vs
100+ ms separation is not subtle, but the per-shipment averages carry the
usual small-sample uncertainty.

**Revised lever ranking for the next build:**
1. **Precompute/parallelise the DONOR-side flatten** (revision of §31 (c)) —
   attacks ~178 ms on the critical path, the term that correlates with the
   straggler.
2. **Option (A), lazy/parallel helper rebuild** — attacks ~165 ms, but paid
   by idle PEs; still worth having for the serial head and the
   one-outstanding-shipment-per-helper limit.
3. Everything in §17 (packing knobs) — re-test only after 1 and 2, since
   both change the per-grant cost that made those levers lose.

---

## 19. CORRECTION to §17e and §18 — Kale's CPU-second objection is right

Kale, 18:29: *"isn't the collection cost borne by 1 PE, but the shipment
worked on by 14 PEs on the recipient? So in CPU seconds it is a net gain for
the donor... or at least the loss is 1/14th."* Correct on both counts, and it
falsifies the explanation I gave in §17e.

### 19a. The per-grant trade is favourable

From the projections traces (@1e11, 39 shipments in the traced subset):

| per shipment | PE-seconds | on |
|---|---|---|
| donor flatten (`s3ShipOrder`) | **0.178** | 1 PE |
| helper rebuild (`s3Shipment`) | 0.165 | 1 PE |
| stolen work (`drainForeign`) | **0.964** | ~13.7 PEs |

**The donor pays 0.178 PE-s to offload 0.964 PE-s — a 1 : 5.4 net gain.**
And its wall-time cost is 177.7 ms *of one PE*, i.e. **12.7 ms of
process-equivalent wall**, not the 178 ms of critical path §18 implied.

(§18 also used the wrong `drainForeign` figure — 69.70 s from the sum-detail
run rather than 37.59 s from the projections trace — which inflated the ratio
to 1:10.1. Corrected above.)

One refinement worth keeping: **PE-seconds are not fungible.** The donor
spends *scarce* capacity — its 14 PEs are the busy ones, which is what makes
it the straggler — to offload onto *free* capacity, since helpers are idle by
construction. Trading scarce for free at 1:5.4 is unambiguously good.

### 19b. The real predictor is grant COUNT, not work moved

Re-testing §17e's correlation against grant count, over the same 14 arms:

```
  grant COUNT vs phaseB_s max : r = +0.919
  work MOVED  vs phaseB_s max : r = +0.693
  grant COUNT vs work MOVED   : r = +0.829   <- the confound
```

§17e's claim was picking up grant count through that confound: the density
arms moved more work *by making 3–5x more grants* (1082–1503 against ~315 at
baseline). **Work moved is not the harmful variable. Shipment count is.**

§17e is therefore **retracted as stated**. Its data stands; its explanation
does not.

### 19c. Revised mechanism: declining marginal value at fixed marginal cost

If each grant is individually a 1:5.4 win, why does making more of them hurt?
Because the *value* of the marginal grant falls while its *cost* does not.
Early grants take work off the critical path. Later ones increasingly ship
work that was not binding — from non-straggler donors, or work the straggler
would have finished anyway — while still costing their donor a fixed
~178 PE-ms. Past some count the marginal grant is net negative. The density
arms ran deep into that region: most work moved, most grants, worst
straggler.

### 19d. What this does to the lever ranking

It sharpens rather than overturns §18. Reducing per-grant donor cost does not
merely save time — it **raises the break-even grant count**, the point at
which an extra shipment stops paying for itself. So:

1. **Precompute/parallelise the donor-side flatten** — still first, but the
   justification is now "raise the break-even count", not "the donor is
   paying too much in total".
2. **Option (A)** — helper-side rebuild, 0.165 PE-s on idle PEs. Cheap
   already in CPU-seconds; its value is the serial head and the
   one-outstanding-shipment-per-helper limit, not CPU.
3. **Fewer, larger grants remain right at today's costs** — which is why
   baseline beat every packing lever, and why the byte cap (which multiplied
   grant count) lost worst of all.

**Two of my explanations have now been retracted in one afternoon** (the
`log(units/ship)` correlation in §17d, and work-moved in §19b). Both were
correlations across heterogeneous arms where the mechanisms co-vary. The
per-grant PE-second accounting above is the first account here grounded in
measured costs rather than a fitted trend, and it is the one to build on.

---

## 20. The timeline picture (Kale, 19:20) — collection is CONCURRENT, which
## partly undoes §19's 1/14th argument

Kale supplied `Timeline-S3-08-13.png`, a Projections timeline of
`proj2b_wire`, PEs ~737–783 (processes N52–N55), 8.6–12.2 s. He identified
the orange entry methods on process 55 as shipment preparation.

Reading it against the numbers:

- **Bar width matches the measurement.** The orange `s3ShipOrder` bars on
  proc 55 span roughly 600–700 ms at this scale, consistent with the measured
  max of 858 ms and p99 of 540 ms.
- **They are NOT confined to one PE.** Orange appears on approximately
  **six of proc 55's fourteen PEs** — P771/772, P774, P776, P778, P780, P782 —
  and they **overlap in time**, clustered around 10.0–10.7 s.

### 20a. Correction to §19

§19 accepted that a grant costs the donor "177.7 ms of one PE = 12.7 ms of
process-equivalent wall", on the assumption that one PE collects while the
other thirteen keep draining. **The timeline shows that is not what happens.**
With ~6 of 14 PEs in `s3ShipOrder` simultaneously for ~600 ms, the donor loses
on the order of **40% of its capacity** for that window, not 1/14.

That is a better explanation for §17e's grant-count correlation than the
"declining marginal value" story §19c proposed: collection is concurrent, so
grant count translates into simultaneous capacity loss on precisely the
process that is already the bottleneck.

The CPU-second accounting in §19a still stands — 0.178 PE-s spent to offload
0.964 PE-s is a real 1:5.4 gain. What was wrong was the *wall-clock* inference
drawn from it. Both can be true: each grant is a good trade in CPU-seconds
while a burst of concurrent grants is a bad trade in critical-path time.

**This is the third correction of the afternoon, and the pattern is
consistent: aggregate counters gave the right totals and the wrong picture of
concurrency. The timeline was the only thing that showed it.**

### 20b. Kale's treebuild question

*"I wonder if we do the treebuild (before Phase1 begins — not the shipments)
in such a way that each treepiece is contiguous (size it, allocate it and then
fill it). Doable?"*

**Yes, and the machinery is largely present.** `FullNodePool::getBuf()`
(`src/TreeCache.h`) already bump-allocates contiguously within `new[]` chunks,
and `alloc()` uses placement-new, so pointers stay valid — no rewriting to
offsets is needed. The gap is that the pool is **per-lane (per-PE)**:
`makeNode(int lane, …)` indexes `pools[lane]`, so every TreePiece on a PE
interleaves its nodes with the others'. Per-TreePiece pools, or a reserved
contiguous span per piece, would deliver the contiguity.

**The catch:** that pool also serves the **CacheManager**, whose remote nodes
arrive asynchronously during traversal and scatter through the chunks. phaseB
units are cross-TreePiece pairs, so what `s3ShipOrder` actually flattens is
plausibly dominated by cache-fetched subtrees, which per-TreePiece contiguity
would not touch.

**Measure first:** of the nodes a shipment flattens, what fraction are
locally-built versus cache-fetched? Local-dominated means the change lands
directly on the 178 ms; cache-dominated means the contiguity idea has to be
applied to the cache arrival path, which is harder because arrival order is
not spatial.

---

## 21. THE DONOR IS BLOCKED ON THE SEND — 73% of collection (job 5259335)

Both runs exact. Donor-side split of `s3ShipOrder`, from the new
`FOF3STAT s3_time` counters (untraced production binary, GRANT_M2=1e11):

| component | per grant | share |
|---|---|---|
| flatten (tree walk + build) | 28.8 ms | 26.3% |
| **marshal + send** | **80.1 ms** | **73.3%** |
| scan / CAS / everything else | 0.4 ms | 0.4% |
| **total `s3ShipOrder`** | **109.3 ms** | over 315 shipments |

A field-by-field PUP-style pass over a 40 MB grant benchmarks at ~4 ms (~8 ms
for the sizer+pack pair), so **~72 ms of the 80 ms is the donor blocked in the
send path** — waiting on a 40 MB message whose completion it does not need.

### 21a. This vindicates Kale's zero-copy post API proposal

Kale, 18:45: *"Charm++ supports zero-copy messages, where you send a control
message first, the recipient gives a buffer, and LCI put or get moves the
data."* That is precisely the fix for a blocking send: the donor issues a
small control message and returns; the payload moves by RDMA without the
donor waiting.

**On this measurement it is the single highest-value change available**, and
it targets the term that sits on the straggler's critical path.

### 21b. Two of my own conclusions are retired

- **Lever (c)'s "precomputed partition manifest" is dead.** Scan+CAS is
  **0.4%** — 0.4 ms of a 109 ms grant. §18 inferred this from the 13 µs median
  call; this measures it directly. There is nothing there to reclaim.
- **My "cache misses / object construction" diagnosis (§12e, §20b) was
  wrong.** I argued from a pup microbenchmark that the cost could not be data
  movement because packing runs at ~10 GB/s. Correct about packing, wrong
  about the conclusion: the cost is the *network wait*, which the benchmark
  never modelled. The donor is not chasing pointers; it is blocked.

### 21c. It also explains the timeline

§20 read six of proc 55's fourteen PEs sitting in overlapping ~600 ms
`s3ShipOrder` bars. On this split those are largely **six PEs blocked on
sends**, not six PEs computing — which makes §20's concurrency finding and
this one the same phenomenon, and makes it much cheaper to fix than
restructuring anything.

### 21d. Revised ranking

1. **Zero-copy post API for `s3Shipment`** — targets 73% of donor collection,
   on the critical path. Kale's proposal.
2. **Per-TreePiece contiguous tree build** (§20b) — targets the 26% flatten.
   Real, and the `FullNodePool` machinery is largely present.
3. **Option (A), lazy/parallel helper rebuild** — helper side, paid by idle
   PEs.
4. **Lever (c) manifest** — retired, 0.4%.

### 21e. Caveats

- The helper-side split did not take: only **23 of ~302** shipments were
  counted, because helper counters print at the helper's own merge, which can
  precede its helping. §20 of the campaign doc already records this as a known
  cosmetic issue; it now blocks a measurement, and the fix (print from
  phase3Stats instead) is worth doing before anyone trusts helper-side timing.
- 109.3 ms/grant here vs 178 ms from the projections trace: expected, since
  the traced binary carries tracing overhead. Do not mix them in one table.
- "marshal + send" is one bracket. Splitting the pup from the send would need
  a timer inside charm's generated code; the ~4–8 ms bound comes from the
  standalone benchmark, not from this run.

---

## 22. Marshal vs send (job 5260928) — Kale's contiguity objection was load-bearing

Kale, 23:30: *"Even with post API, you will need a contiguous message at the
source (what is marshal? Didn't the flatten do it?)"*

**Flatten did half of it.** `b797e73` produced the wire FORMAT — offset-based
`WireNode`s, no pointers to fix up, which is the hard prerequisite for any
zero-copy scheme. It did **not** produce contiguity: a shipment is
`vector<StealTree>`, each holding `vector<WireNode>` + `vector<Particle>`,
plus `unit_pairs`. **Marshal** is charm's `PUP::sizer` + `PUP::toMem` pass
that gathers those into one contiguous message buffer.

### 22a. Measured split (both runs exact)

A timed `PUP::sizer` walks exactly what `toMem` walks, without copying, so it
bounds the gather's traversal:

```
s3ShipOrder            134.9 ms/grant   (17.1 ms of it is the sizer scaffolding)
  flatten (tree walk)   30.3 ms          22.5%
  marshal + send        87.1 ms          64.5%
    sizer walk          17.1 ms
  trees per shipment   189.1  =>  379 separate vectors to gather
```

Removing the measurement overhead, the real per-grant donor cost is about:

| component | ms/grant | share | removed by |
|---|---|---|---|
| flatten (tree walk) | ~30 | 26% | arena-building flatten (partly) |
| **marshal gather** | **~34** | **29%** | **arena-building flatten (fully)** |
| **send (donor blocked)** | **~53** | **45%** | **zero-copy post API** |
| total | ~117 | | |

**189 trees per shipment = 379 separate allocations to chase.** That is why a
pass which copies nothing still costs 17 ms.

### 22b. Build order — arena-flatten first

It is not send-or-gather, it is both. But **arena-flatten should be built
first**:

1. it removes the ~34 ms gather outright;
2. it needs no new charm API;
3. it is the **prerequisite** for the post API, which requires a registered
   contiguous source buffer — which nothing currently produces until marshal
   does.

The post API then takes the ~53 ms send block on top. Together they address
~74% of donor collection cost, on the straggler's critical path.

### 22c. Honesty on the send figure

`send ≈ 53 ms` assumes `pack ≈ 2 x sizer`. If the copy is cheap,
pack ≈ 17 ms and send ≈ 70 ms; if pack ≈ 3 x sizer, send ≈ 36 ms. **The send
is somewhere in 36–70 ms** and cannot be narrowed without a timer inside
charm's generated marshalling code. The gather number (17 ms of pure
traversal over 379 vectors) is measured, not estimated.

Also: the extra sizer pass inflates `s3ShipOrder` in this run only
(134.9 vs ~117 ms). It is measurement scaffolding, flagged in the code, and
should be removed before the patch is merged.

---

## 23. THE SEND IS ~1 ms. THE MARSHAL PACK IS THE COST. (Kale, 23:39)

Kale: *"The entry method record (between begin_execute and end_execute)
records any sends coming out of that block. And you can see the sends
themselves are close to the end of the block. So they are not a major part of
the cpu overhead... zero-copy is a good thing to do overall, but I won't
expect significant gain."*

**Verified directly from the projections traces.** For every `s3ShipOrder`
block, the position of the enclosed `s3Shipment` CREATION record, as a
fraction of the block (0 = start, 1 = end):

```
min 0.970   p10 0.981   p50 0.992   p90 0.995   max 0.995     n = 39 blocks
```

**The send sits at 99.2% through the block** — it occupies roughly the last
1 ms of a ~135 ms block. The donor never blocks on the fabric.

### 23a. Corrected donor cost breakdown

| component | ms/grant | share | removed by |
|---|---|---|---|
| flatten (tree walk) | ~30 | 26% | arena-flatten (partly) |
| charm `sizer` pass | ~17 | 15% | **arena-flatten** |
| charm `pack` — gather 379 vectors + copy 40 MB | **~69** | **59%** | **arena-flatten** |
| send | **~1** | **~1%** | zero-copy post API |
| total | ~117 | | |

**Arena-flatten is worth ~86 ms of a ~117 ms grant — about 73% of donor
collection.** If `flattenStealTree` writes directly into one contiguous
arena, there is nothing to size and nothing to gather; the buffer is already
the message.

**The zero-copy post API addresses ~1 ms.** Kale's own assessment — *"good to
do overall, but I won't expect significant gain"* — is correct. It remains
worth having for latency and for freeing the RDMA engine, but it is not a
lever on the straggler.

### 23b. My error, twice, on the same 80 ms

§21 concluded "the donor is blocked in the send path". §22 refined it to
"send is 36–70 ms". **Both were wrong, and from the same cause:** my timer
bracketed *around the send call*, so charm's sizer, pack and send all landed
in one number, and I attributed it to the send because that was the part I
could name.

The disproof was already in the traces I had. CREATION records appear inside
the enclosing entry block, so "where in the block does the send happen" was
answerable all afternoon, and I never asked it.

**Pattern for the record:** the aggregate counters have repeatedly produced
correct totals with wrong attributions — §17d (grant size), §19b (work
moved), §20a (concurrency), §21/§22 (send). Every correction came from
reasoning about what the records *mean*, not from more counters. The
projections timeline and the event-record semantics carried information that
no amount of summing could.

### 23c. Revised build ranking (final for the session)

1. **Arena-building flatten** — ~86 ms/grant, 73% of donor collection. No new
   charm API. Also the prerequisite for anything zero-copy.
2. **Per-TreePiece contiguous tree build** (§20b) — attacks the residual ~30 ms
   flatten by improving locality of the walk itself. Composes with 1.
3. **Option (A), lazy/parallel helper rebuild** — helper side, idle PEs.
   Note the helper-side timers still do not work (§21e).
4. **Zero-copy post API** — ~1 ms of donor time. Worth doing for latency and
   RDMA-engine offload, not as a straggler lever.
5. **Lever (c) manifest** — retired (0.4%).

---

## 24. STEP 1 — POD WIRE + BULK PUP: the campaign's best result (job 5262081)

Kale, 23:50: *"that vtable issue feels like an unnecessary price to pay. All
the actual nodes belong to the same subclass.. Can we leverage that somehow?"*

Implemented: `WireNode` no longer embeds `SpatialNode<Data>` (which carries
`virtual ~SpatialNode()`), but a POD `WireSpatial` holding exactly the five
fields `SpatialNode::pup` shipped — `data`, `n_particles`, `depth`,
`particle_min_index`, `particle_max_index`. `WireNode` and `Particle` are then
trivially copyable, so `StealTree::pup` moves each array in ONE raw-bytes call
instead of ~15 field-pups per element.

**Gates: 10k exact (3549) AND `FOF_S3_LOOPBACK=1` exact** — the loopback
replays every locally claimed unit through flatten → pup → rebuild → walk
against the direct walk, which is the strongest correctness evidence available
for a wire-format change. Both 2B runs exact.

### 24a. Donor collection

| | order ms/grant | flatten | marshal+send | sizer |
|---|---|---|---|---|
| before (5260928) | 117.8 | 30.3 | **87.1** | 17.1 |
| **POD wire** | **49.4** | 30.6 | **18.5** | **0.0** |

**−58% on donor collection; −78% on marshal+send.** The sizer walk went to
**zero**: with a bulk pup, `PUP::sizer` adds `nn * sizeof(WireNode)` in one
call rather than walking 285k elements. That is the direct confirmation that
the unexplained 30x of §23 was per-element pup machinery.

### 24b. End to end — best of the campaign on all three headline rows

| | phaseB_s max | Pre-traversal | Iteration 0 |
|---|---|---|---|
| best before tonight | 1.520 | 4.322 | 6.98 |
| **POD wire rep1/rep2** | **1.322 / 1.229** | **3.744 / 3.596** | **6.416 / 6.278** |

The straggler is now **4.9x the 0.25 s granularity floor**, against 6.1x at the
previous best and 12.6x for the campaign as originally specified (§17).

### 24c. STEP 2 — the shipment-level arena is currently a REGRESSION

| | order | flatten | marshal+send | phaseB_s max |
|---|---|---|---|---|
| step 1 alone | **49.4** | **30.6** | 18.5 | **1.23–1.32** |
| step 2 arena | 56.7 | **39.5** | 16.9 | 1.48–1.49 |

Concatenating every tree into one shipment-level vector saves ~2 ms of marshal
and costs **~9 ms of flatten**: appending into a single ~30 MB vector triggers
repeated reallocation, each copy moving the whole arena, where per-tree vectors
stayed small and grew independently.

**Fixable** — the self-calibrating `s3BytesPerBelow()` predictor already exists
and can `reserve()` the arena up front. Retested in §25. The arena remains
required for zero-copy (a single registered contiguous buffer), so this is a
performance bug in the implementation, not a reason to drop the design.

### 24d. What this does to the lever ranking

With marshal collapsed, the donor breakdown is now **flatten 30.6 ms (62%)**,
marshal+send 18.5 ms (37%), scan 0.4 ms. So:

1. **The flatten is now the dominant donor cost** — which promotes Kale's
   other idea, the per-TreePiece contiguous tree build (§20b), and the arena
   once its reallocation is fixed.
2. **Zero-copy's ceiling has shrunk** from 87 ms to 18.5 ms of donor time. Its
   value is now mostly the latency and RDMA-engine offload Kale noted, not CPU.

---

## 25. Steps 2 and 3: the arena is neutral, zero-copy is blocked upstream

### 25a. Arena with `reserve()` — the regression is fixed, the idea is not needed

| | flatten | marshal+send | MB/ship | **order per MB** |
|---|---|---|---|---|
| step 1 (POD wire) | 30.6 | 18.5 | 30.5 | **1.62** |
| arena, no reserve | 39.5 | 16.9 | 27.1 | 2.09 |
| arena + reserve | **29.4** | 22.6 | 33.8 | **1.64** |

Reserving the arena from the running bytes-per-unit fixed the +9 ms flatten
regression exactly as predicted (repeated reallocation of a ~30 MB vector).
But normalised for grant size the arena is **neutral** — 1.62 vs 1.64 ms/MB —
and end to end it stays behind step 1 (max 1.337/1.376 vs 1.229/1.322).

**The arena was built to fix a cost step 1 had already removed.** Its premise
was that marshal must gather 379 scattered vectors; with bulk pup, gathering
N vectors is N memcpys, so the vector count stopped mattering. Not merged.
Preserved only because zero-copy needs one contiguous registered buffer.

### 25b. Zero-copy: correct at 10k, aborts at 2B — an upstream limitation

Implemented on top of the arena: `nocopypost` entry method, on-demand buffer
posting in a post EM, heap-owned shipment freed by a refcounted completion
callback. Job 5262530 arm C (2B, `FOF_S3_ZC=1`) aborts on every process:

```
CMK_NOCOPY_DIRECT_BYTES is too small        (reconverse/src/conv-rdma.cpp:528)
```

```c
// include/conv-rdma.h
// 8-byte for mr, 16-byte for rmr
// TODO: better to use dynamic allocation and PUP
#define CMK_NOCOPY_DIRECT_BYTES 32
```

**CXI's remote memory-region key does not fit in charm's 32-byte layer-info
buffer.** The design is already flagged TODO upstream. Not a paratreet2 bug,
and not fixable without rebuilding charm — which was NOT done, because
`~/software/charm` is pinned at `3d1fdd89f` for campaign provenance.

Work preserved with a full write-up at `~/software/patches/wip/`.

### 25c. TWO VACUOUS VALIDATIONS — the lesson of the night

I twice reported evidence that could not have detected this failure:

1. **"All four 10k gates passed, including zc-loopback."** At 10k, S3 ships
   **nothing** — the pool drains before any helper is matched (§21 records the
   same at 80M). The zero-copy send never executed. `zc-loopback` exercises
   flatten → pup → rebuild *locally*, which is the wire format, not the
   transport.
2. **"The standalone probe moved 8 MB with contents verified."** Two processes
   on one node take the `CmiUseCopyBasedRDMA` path, which never registers
   memory — the exact step that fails.

Both tests ran at a scale where the code under test does not execute. The
correct gate for the zero-copy path is 2B, or 80M in forced mode
(`FOF_S3_TEST=1`) where shipments are guaranteed.

This is the same failure mode as §17d and §19b, one level down: there I drew
conclusions from correlations across arms whose mechanisms co-varied; here I
drew confidence from tests whose scale excluded the mechanism. **In both
cases the number was real and the inference was not.**

### 25d. Also observed: a transient fabric fault

Job 5262486's marshalled control run died with
`post_send_impl: Input/output error` — with `FOF_S3_ZC` unset, so none of the
new code ran. The triage job re-ran the identical binary and settings and it
passed (arm B, exact). **Transient.** Recorded because it would otherwise look
like a regression in the ZC binary; the machine has been unsettled tonight
(an 8-hour queue estimate at 17:25, then instant backfills at 20:24 and 03:39).
