# Frontier report: the GRANT_M2 strangle + reservation verdict at 2B (received 2026-08-13)

Provenance: replacement Frontier session (predecessor killed overnight),
paratreet2 0f30988, 2B/16 nodes. Jobs 5253255 (10k gate), 5253291
(spec campaign, 7 runs), 5253386 (GRANT_M2 ladder, 6 runs), 5253475
(best-cell head-to-head, 4 runs) — 21/21 2B runs exact (424897832).
Also folds in never-relayed pre-crash results: 5250425 (GRANT x PARTS
grid, b210b6f) and 5250906 (80M split-size). Relayed by Kale
(frontier-inbox/s3-reserve-2b.md, plus temp2.txt excerpt earlier the
same morning); stored verbatim below. Acted on same morning: defaults
changed at 1996ebd (GRANT_M2 1e10, GRANT_UNITS_PER_PE 128, RESERVE
off), design doc section 30.

---

# S3 section-27 donor-side reservation at 2B / 16 nodes — Frontier

Executing `design/frontier-s3-reserve-2b.md`. Written by the replacement
Frontier session on 2026-08-13 after the overnight session was killed
(connection reset).

**STATUS: COMPLETE. 21 of 21 2B runs exact (424,897,832), plus a 4-arm 10k
gate.** Jobs 5253255, 5253291, 5253386, 5253475.

**Bottom line up front — two findings, and the second is the bigger one:**

1. **Reservation (§27) is a net loss and should default OFF.** It is correct,
   and it does engage at 2B (65–70 windows per run, 66–96% of shipped units
   sourced from them — the dynamic self-excluded-mean trigger works as
   designed and needs no recalibration). But it makes grant composition
   *worse*, not better, and costs **35% on the straggler and +85% on
   Pre-traversal** at the campaign's best configuration. §27's premise —
   "donor eats giants, helpers get dust" — does not survive the `tot_m2`
   denominator instrument that msg2 itself asked for: grants were already
   carrying **7.1x the pool mean** m2 per unit without it.
2. **`FOF_S3_GRANT_M2 = 5e7`, new in `0963ded`, silently erased S3 v2's
   entire win.** It binds ~25x tighter than the 448-unit count cap, collapsing
   grants from 206–224 units to 5.6. Lifting it reproduces v2 exactly on five
   metrics. This has nothing to do with reservation and would have gone
   unnoticed, because every arm of the campaign as specified sits at it.

**Best configuration found:** `GRANT_UNITS_PER_PE=128`, `PB_PARTS=16`,
`GRANT_M2` lifted, **reservation off** → `phaseB_s max` **1.572 s** (6.3x the
0.25 s floor, vs 12.6x for the campaign as specified) and the best Iteration 0
of the whole campaign, 7.138 s.

If you read this file before ~11:00 and it ended at §6 or §8, reload — I was
writing into it while you had it open.

---

## 0. State recovered from disk and Slurm

The predecessor's context was gone; everything below was reconstructed from
files and `sacct`. **Read this section first — two of the task spec's
premises turned out to be wrong.**

### Where the notes actually were

`~/software/reports/` **did not exist**. The predecessor wrote to top-level
files in `~/software/`:

| file | mtime | what |
|---|---|---|
| `BUILDS.md` | Aug 12 19:47 | build-state note (two charm trees, one app tree, staged binaries) |
| `BLOCKER-input-access.md` | Aug 13 03:04 | the overnight permission failure |
| `s3-ab-results.md` | Aug 12 21:41 | 28 KB running report, generations through job 5250364 |
| `frontier-corrections.md` | Aug 13 01:05 | corrections to `frontier.md` (build recipe, module quirks) |
| `lci-cxi-error.md` | Aug 12 15:47 | LCI/CXI transport notes |

I created `~/software/reports/` and put this report there as instructed. The
five files above are still at the top level — **`scp` them too**; they are not
duplicated here.

### Jobs found, and their fates

`squeue` was **empty** on arrival — nothing to cancel. Full `sacct` since
Aug 12:

| job | name | fate | notes |
|---|---|---|---|
| 5248429 | s3-ab-2b-16n | COMPLETED 16:58 | S3 v1 A/B, 9/9 exact |
| 5248439 | s3-shakedown-4n | COMPLETED 16:54 | 80M shakedown |
| 5248741 / 5248892 | smt-ab-2b-16n | COMPLETED | SMT split verdict |
| 5248902 / 5249400 | sumd2b-16n | COMPLETED | trace-shutdown fix |
| 5248978 | slice-ab-2b-16n | COMPLETED 18:19 | `FOF_PHASEB_SLICE_MS` |
| 5249722 | sumd2b-slice-16n | COMPLETED 19:28 | |
| 5250048 | slice-v2-2b-16n | COMPLETED 20:25 | v1 capped |
| 5250364 | slice-v3-2b-16n | COMPLETED 21:40 | **S3 v2, first net win** |
| 5250425 | opt-trials-2b-16n | **COMPLETED 22:47** | GRANT x PARTS grid, 6 cells, all exact — **results were never reported; folded in at §5 below** |
| **5250574** | opt-combo-2b-16n | **CANCELLED while PENDING** | see below |
| 5250804 | splitsize-80m-4n | FAILED 03:00 | input unreadable; aborted at its own exactness gate |
| 5250906 | splitsize-80m-4n | COMPLETED 04:19 | repointed to the `/ccs/proj` 80M copy — **results folded in at §6** |

**Correction to the task spec — job 5250574 produced nothing.** The spec says
the 22-run SLICE x GRANT x PARTS sweep "CRASHED overnight" and asks me to
"harvest whatever runs completed before the crash." It never ran a single
cell:

```
5250574  opt-combo-2b-16n  CANCELLED by +  Start: None  End: 2026-08-13T03:03:13  Elapsed: 00:00:00
```

`Start: None` with `Elapsed 00:00:00` — it was still **PENDING** in the queue
when the predecessor cancelled it, deliberately and correctly
(`BLOCKER-input-access.md` §"Actions taken": cancelled because it reads the 2B
file and "would have failed identically, burning a 16-node allocation and
dropping core dumps"). There is no output directory
(`/lustre/orion/csc710/scratch/lvkale/s3ab/5250574` does not exist) and no
`.out` file. **Nothing to harvest.** The last 2B job that produced data is
5250425.

This matters for the campaign config — see §3.

**Second correction:** the spec says "anything after it CRASHED." Not so. The
predecessor kept working after the blocker: it diagnosed the failure, found a
second copy of the *80M* input at `/ccs/proj/csc710/rrao/lambb.00500`,
repointed the split-size sweep at it, and job **5250906 completed cleanly at
04:19** with all arms exact. Those results are in §6 and were never relayed.

### Build state recovered

`BUILDS.md` was accurate. The shared app tree was linked against
**tracedcharm** (`examples/fof3/FoF3` carried 524 `TraceSummary` symbols), so
the full clean relink against production charm was genuinely required, not
precautionary.

---

## 1. Dataset — resolved

Both checks in the task spec came back, and the good outcome happened while I
was rebuilding:

| path | state |
|---|---|
| `/lustre/orion/csc710/scratch/rrao/cosmo25cmb.768g2_dm.001024` | **still `Permission denied`** (dir still `drwx--S--- rrao rrao`) |
| `/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024` | **READABLE — appeared 10:21 today** |

Ritvik copied the dataset into project space rather than re-opening personal
scratch — the more robust of the two fixes `BLOCKER-input-access.md` asked
for. `lambb.00500` (80M) landed alongside it at 10:23.

```
-rw-r--r-- 1 rrao csc710 76780929056 Aug 13 10:21 cosmo25cmb.768g2_dm.001024
-rw-r--r-- 1 rrao csc710  2902376480 Aug 13 10:23 lambb.00500
```
(directory `drwxrws--- root csc710`)

**I verified it is the same file and the copy is complete** before submitting
anything — a half-copied 76 GB input would have produced a plausible-looking
but wrong component count:

- size `76780929056`, **stable over 20 s**, and byte-identical to the size
  recorded pre-crash in `BLOCKER-input-access.md`;
- first and last 32 bytes both readable (so the tail is not a hole);
- XDR header parses and is self-consistent:
  `nbodies 1,981,808,640` (matches the pre-crash header exactly),
  `nsph 452,984,832`, `ndark 1,528,823,808` — the two sum to `nbodies`, and
  `32 + nsph*48 + ndark*36 = 76,780,929,056` = the exact file size.

**The campaign uses the proj-shared copy**, per the spec's preference and the
blocker note's recommendation. The sbatch prefers it and falls back to rrao
scratch only if it disappears.

---

## 2. Code

```
cd ~/software/paratreet2 && git fetch && git checkout phaseab-campaign && git pull
```

Landed on **`0f30988a24eb16375d0b954044117946e36e6eae`** — "Merge branch 'main'
into phaseab-campaign". This is exactly the "must land on 0f30988 or later"
tip; the working tree was 18 commits behind and fast-forwarded.

The design doc `frontier-s3-reserve-2b.md` says "must land on 4630ef2"; the
task spec correctly flags that hash as stale. `0f30988` supersedes it.

All four required commits verified present as ancestors of HEAD:

| commit | subject |
|---|---|
| `309673c` | S3 section 27: dynamic donor-side reservation |
| `be6374d` | S3 instruments round 2 (Frontier msg2): out_m2 denominator + shipped-composition histogram |
| `0963ded` | S3 instruments + hybrid grant (Frontier Opus analysis, 2026-08-12) |
| `aba7833` | windowed flush to fix memory error |

Working tree clean of tracked modifications (only `utility/`, the untracked
submodule dir, shows up in `git status`).

Design docs read: `phaseab-balancing.md` §§19–28 and
`frontier-s3-reserve-2b.md`.

Knob and stat names verified directly against `fof/FoFPhase1.h` rather than
assumed:

- `FOF_S3_RESERVE` (default **on** when S3 armed: `return !e || atoi(e) != 0`)
- `FOF_S3_RESERVE_FACTOR` (default `2.0`), `FOF_S3_RESERVE_FRAC` (default `0.5`)
- `FOF_S3_GRANT_UNITS_PER_PE` (default `32`, multiplied by node size)
- prints: `FOF3STAT s3_reserve: node %d lo %u hi %u m2 %.3g`,
  `FOF3STAT s3: ... out_m2 ... rem_m2 %ld tot_m2 %.4g resv_shipped %ld`,
  `FOF3STAT s3_grant_m2_hist: ... log2m2`

---

## 3. Build and gates

Clean relink of the whole stack against **production** charm
(`CHARM_HOME=$HOME/software/charm/reconverse-linux-x86_64`, pinned at
`3d1fdd89f`), `make clean` at every stage per BUILDS.md — no header
dependency tracking. Script: `~/software/rebuild-prod.sh`.

Non-interactive shells have no `module`, so the script sources
`/opt/cray/pe/lmod/lmod/init/bash` first (`frontier-corrections.md` §1e) and
loads `PrgEnv-gnu cmake hwloc python`.

Stages: `htram` → `unionfind/prefixLib` → `unionfind` → `paratreet2/src` →
`paratreet2/fof` → `paratreet2/examples/fof3`. All succeeded.

**Trace verification** (`nm -C <bin> | grep -ci TraceSummary`):

| binary | before | after |
|---|---|---|
| `paratreet2/examples/fof3/FoF3` | 524 (traced) | **0** |
| `~/software/FoF3.2b.resv` (staged) | — | **0** |

Staged under the distinct name **`FoF3.2b.resv`** per BUILDS.md's
"binary swapped under a queued job" incident.

### 10k smoke test — PASSED (job 5253255, 2 nodes x 1 task x 4 PEs)

Run before any 2B submission. Four arms, all exact:

| arm | components | gold |
|---|---|---|
| base | 3549 | 3549 |
| s3-reserve (defaults) | 3549 | 3549 |
| s3-noreserve (`FOF_S3_RESERVE=0`) | 3549 | 3549 |
| s3-reserve-all (`FOF_S3_RESERVE_FACTOR=0`) | 3549 | 3549 |

The `FACTOR=0` arm is the correctness-gate config from the design doc — every
member reserves. Its windows came up **empty** (`lo 1450 hi 1450 m2 0`,
`lo 1514 hi 1514 m2 0`), exactly as the doc predicts for small pools: "the
windows come up empty there — small pools drain before RESERVE lands." So the
10k gate proves correctness, not engagement. 2B is the first place the
mechanism can actually engage.

---

## 4. The 2B campaign — job 5253291 (16 nodes, 7 runs, 2 min 21 s)

**All 7 runs exact at 424,897,832.** Divergence guard clean on all three app
repos.

### Exact srun line (as executed, every 2B run)

```
srun -N 16 --ntasks-per-node=8 -t 10:00 \
     --mpi=cray_shasta --network=job_vni --unbuffered \
     --cpu-bind=none --distribution=block:block \
     $HOME/software/FoF3.2b.resv \
     -f /lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024 \
     -d oct -u serial +traceoff +ppn 14 \
     +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
     +lci_ndevices 7 +backend_poll_thread 2
```

Fixed across every arm (the 5250364 configuration, per the design doc's
fallback rule — see §0 for why 5250574 supplied nothing):

```
FOF_PHASEB_SLICE_MS=2  FOF_PB_PARTS=32  FOF_S3_GRANT_UNITS_PER_PE=default(32)
FOF_STEALA=1  FOF_STEALA_GEO=1  FOF_PB_M2KEY=1  FOF_PROCS_PER_PNODE=8
LCI_ATTR_BACKEND=ofi  FI_CXI_RX_MATCH_MODE=hybrid  PMI_MAX_KVS_ENTRIES=4194304
```

Arms differ **only** in `FOF_S3` / `FOF_S3_RESERVE`. Order was interleaved:
base, reserve, noreserve, reserve, base.

### Primary block

| arm | phaseA | phaseB | phaseB_s max | Pre-trav | Iter0 | resv windows | out_ships | out_units | units/ship | out_m2/tot_m2 | resv_shipped | declines |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| base-serial-1 | 1.937 | 3.205 | 3.176 | 5.302 | 8.049 | — | 0 | 0 | — | — | — | 0 |
| s3-reserve-1 | 1.964 | 3.177 | **3.144** | 5.272 | 7.961 | **69** | 3021 | 31428 | 10.4 | 3.42% | 28573 (90.9%) | 1069 |
| s3-noreserve-1 | 1.946 | 3.199 | 3.165 | 5.306 | 8.018 | 0 | 3289 | 18269 | 5.6 | **5.56%** | 0 | 1297 |
| s3-reserve-2 | 1.944 | 3.187 | 3.147 | 5.343 | 8.115 | **68** | 3092 | 28733 | 9.3 | 3.70% | 27682 (96.3%) | 1010 |
| base-serial-2 | 1.944 | 3.218 | 3.191 | 5.335 | 8.049 | — | 0 | 0 | — | — | — | 0 |

Seconds; `phaseB_s max` is the per-PE max over 1792 PEs. `phaseA_skew` was
flat across all arms (within 1.22–1.27, cross 1.42–1.43, global 1.72–1.75,
`max_piece_n` 138233 identically) — phaseA is untouched, as designed.

### Supplementary block (GRANT=128 / PARTS=16, the 5250425 best cell)

| arm | phaseA | phaseB | phaseB_s max | Pre-trav | Iter0 | resv windows | out_ships | out_units | units/ship | out_m2/tot_m2 | resv_shipped | declines |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| supp-reserve | 1.940 | 3.209 | 3.173 | 5.312 | 8.045 | 66 | 2620 | 28185 | 10.8 | 2.77% | 26307 (93.3%) | 525 |
| supp-noreserve | 1.951 | 3.176 | 3.136 | 5.296 | 8.051 | 0 | 2824 | 18793 | 6.7 | 5.11% | 0 | 770 |

Same shape as the primary block. The better grant sizing did not rescue it.

### The FACTOR probe was not needed

The design doc's contingency ("if the natural s3-reserve runs show NO
`s3_reserve:` lines, add a run with `FOF_S3_RESERVE_FACTOR=1.2`") did not
trigger. **The reservation trigger fires readily at 2B: 69 and 68 windows**
against 128 processes, versus zero on the laptop and empty windows at 10k.
The self-excluded-mean fix is doing its job. `FACTOR` needs no recalibration.

---

## 5. THE HEADLINE — and it is not the one the doc expected

### 5a. The whole S3 v2 win has disappeared at this commit

Compare against the generations in `s3-ab-results.md`:

| metric | unsliced base 5249400 | **v2 `b210b6f` 5250364** | THIS build, base | THIS build, s3-reserve |
|---|---|---|---|---|
| Pre-traversal | 5.32 | **4.47–4.53** | 5.30–5.34 | 5.27–5.34 |
| Iteration 0 | 8.01 | **6.98–7.24** | 8.05 | 7.96–8.12 |
| phaseB (timer) | 3.30 | **2.08–2.18** | 3.21–3.22 | 3.18–3.19 |
| phaseB_s per-PE max | 3.148–3.221 | **1.983–2.113** | 3.176–3.191 | 3.144–3.147 |
| out_units | 1320 | **95,992–105,899** | — | 28,733–31,428 |
| units per ship | 132 | **206–224** | — | 9.3–10.4 |

**At `0f30988`, S3 is statistically indistinguishable from no-S3, and the
whole stack is back at the pre-v2 baseline.** phaseB_s max is 3.14–3.19 s
against the 0.25 s granularity floor — a **12.6x** gap. v2 had cut it to
7.9–8.4x. This is a regression of the campaign's headline metric, not a
reservation result.

### 5b. Cause: the hybrid grant's m2 budget strangles every grant

This is arithmetic, not a guess. From `fof/FoFPhase1.h`:

- `FOF_S3_GRANT_M2` defaults to **`5e7`** (line 508–511)
- the count cap is `FOF_S3_GRANT_UNITS_PER_PE(32) x CkNodeSize(14)` =
  **448 units** (line 472–479)
- both are tested per unit in the collection loop (lines 811, 829); whichever
  binds first ends the grant

Measured pool scale this run: `tot_m2 = 6.201e12` over
`phaseB_units total = 2,321,034` → **mean 2.67e6 m2/unit**. So the 5e7 budget
exhausts after **~18 average units**, roughly **25x tighter than the 448-unit
count cap**. The count cap is now dead code at 2B.

Confirmation: `out_m2 / out_ships` = **7.0e7** (reserve) and **1.05e8**
(noreserve) — one budget's worth per shipment, i.e. essentially every grant
terminates on the m2 budget. (Both overshoot 5e7 because the budget is
tested *before* the unit is added, and the first unit always ships.)

**The s3-noreserve arm is what isolates this.** With reservation off, that
arm is functionally "v2 + hybrid grant + reader fix". It ships 5.6 units per
grant versus v2's 206–224. So the collapse is *not* caused by reservation —
it was already there, introduced by `0963ded`. §24 proposed
`FOF_S3_MAX_GRANT_M2 ~ 1e8`; the shipped default is half that, and both are
far below what this pool needs.

The reader fix `aba7833` is exonerated: this build's **base** arm
(5.30–5.34 / 8.05 / 3.21) matches the pre-v2 unsliced base 5249400
(5.32 / 8.01 / 3.30). Only the S3 benefit vanished, not baseline throughput.

### 5c. The composition verdict — reservation INVERTED it

This is the doc's designated headline readout (`s3_grant_m2_hist` vs the
noreserve control). I aggregated the full per-node histograms from the run
logs, not the 4-node sample in the `.out`:

| arm | shipped units | mean log2(m2) | arithmetic m2 per shipped unit | vs pool mean |
|---|---|---|---|---|
| **pool as a whole** | 2,321,034 | ~21.3 | **2.67e6** | 1.0x |
| s3-reserve-1 | 31,428 | **17.55** | 6.75e6 | 2.5x |
| s3-reserve-2 | 28,733 | 18.22 | 7.76e6 | 2.9x |
| **s3-noreserve-1** | 18,269 | **20.53** | **1.89e7** | **7.1x** |
| supp-reserve | 28,185 | 19.10 | 6.12e6 | 2.3x |
| supp-noreserve | 18,793 | 19.81 | 1.68e7 | 6.3x |

§27 predicted "mass should shift to the high log2m2 buckets if grants now
carry giants." **It shifted the other way, by both measures** — the log-mean
falls 20.53 → 17.55 and the arithmetic m2/unit falls 1.89e7 → 6.75e6. The
low buckets tell it plainly: s3-reserve ships **1560 units of bucket 0**
(m2 ≤ 1, pure dust) against noreserve's **14**.

### 5d. Why — and it questions the premise reservation was built on

The reserved windows are not high-m2 at all. Summed over the 69 windows of
s3-reserve-1: `total_m2 = 3.296e11` across `total_width = 116,638` units =
**2.83e6 m2/unit — 1.06x the pool mean.** The window fences *average* work.

The mechanism is visible in the collection loop (line 806–826 vs 827+): a
grant now drains the donor's **cursor window** `[phaseb_next, +≤1792)` first,
and only then falls through to `range` — the **coordinator-ordered costliest
partition**. Reservation therefore *displaces* collection away from the
costliest partition toward whatever happens to sit at the donor's cursor.

That matters because of what the new `tot_m2` denominator reveals — the very
instrument msg2 asked for in order to settle this question:

> **Grants were never carrying dust.** Without reservation they carry
> **7.1x the pool mean** m2 per unit, because the coordinator was already
> ordering the costliest partition.

msg2's composition finding rested on m2/shipped-unit *falling as grants grow*
(19.1 → 16.4M across GRANT 32→128) — a relative trend, with no absolute
reference. With the denominator now in, that 19.1M is **7x the 2.67M pool
mean**, not dust. So the premise behind §27 — "donor eats giants, helpers get
dust" — does not survive its own instrument. Reservation replaced a 7.1x
source with a 1.06x source and made composition worse.

**Honest caveat, and the reason for the follow-up job.** The 5e7 budget
distorts this comparison: a noreserve grant that starts on a giant ships that
one giant and stops, which flatters its m2/unit by construction. Both effects
— the vanished v2 win and the composition inversion — are measured *through*
a broken grant sizer. Job 5253386 (§7) re-runs the reserve/noreserve pair on
a `FOF_S3_GRANT_M2` ladder to separate them.

---

## 6. Folded in: job 5250425 (opt-trials, 2B/16) — never reported

Completed 22:47 on Aug 12, ~15 min before the input went unreadable. Its
results were still on disk and had not been relayed. It tested exactly the
§26 "next lever 1" (grant attrition compensation), which the design doc still
lists as *not yet built*. Binary `FoF3.2b.prod`, paratreet2 `b210b6f`, all six
cells exact (424,897,832):

| cell | GRANT/PE | PARTS | phaseA | phaseB | phaseB_s max | Pre-trav | Iter0 | ships | units | units/ship | declines |
|---|---|---|---|---|---|---|---|---|---|---|---|
| grant32-parts32 | 32 | 32 | 1.957 | 2.180 | 2.077 | 4.564 | 7.266 | 467 | 103,547 | 221.7 | 1415 |
| grant64-parts32 | 64 | 32 | 1.952 | 2.062 | 1.874 | 4.690 | 7.369 | 442 | 123,055 | 278.4 | 1368 |
| grant128-parts32 | 128 | 32 | 1.944 | 2.128 | 2.006 | 5.958 | 8.631 | 355 | 109,802 | 309.3 | 1320 |
| grant32-parts16 | 32 | 16 | 1.924 | 2.290 | 2.221 | 4.537 | 7.252 | 411 | 115,573 | 281.2 | 732 |
| grant64-parts16 | 64 | 16 | 1.930 | 1.893 | 1.838 | 4.322 | 7.021 | 355 | 146,839 | 413.6 | 708 |
| **grant128-parts16** | 128 | 16 | 1.933 | **1.698** | **1.529** | 4.584 | 7.277 | 302 | 160,416 | **531.2** | 759 |

Readings:

- **Lever 1 works.** Raising `GRANT_UNITS_PER_PE` 32→128 monotonically raises
  units/ship (221.7 → 531.2 at PARTS=16) and cuts phaseB_s max 2.221 → 1.529.
  Grant size *was* binding at v2's default, as §26 suspected.
- **PARTS=16 beats PARTS=32** at every grant size, and roughly halves declines
  (1320–1415 → 708–759). Fewer, larger partitions give the coordinator
  targets the local cursor has not already half-eaten — the §26 explanation.
- **Best cell `grant128-parts16` reaches phaseB_s max 1.529 s**, the best
  figure of the whole campaign to date (v2 best: 1.983). Against the 0.25 s
  floor that is 6.1x, down from 7.9x.
- `grant128-parts32` is the one outlier on wall-clock (Pre-trav 5.958,
  Iter0 8.631) while its phaseB rows look normal — single rep, and the
  machine was busy; treat as noise unless it reproduces.

**This supersedes the design doc's premise for §26 lever 1.** It also means
the campaign's best known config is `GRANT=128 / PARTS=16`, not 5250364's
`GRANT=32 / PARTS=32` — which is why I ran the supplementary block there.

## 6b. Folded in: job 5250906 (split-size, 80M/4) — never reported

The predecessor's post-blocker recovery run, completed 04:19 after being
repointed at `/ccs/proj/csc710/rrao/lambb.00500`. All arms exact
(23,707,197). `C` is the split-size knob; `pre` is Pre-traversal ms.

| arm | phaseB | phaseB_s min/avg/max | maxpair max | units | mean_unit_ms | pre |
|---|---|---|---|---|---|---|
| C=0 (warm-up, discarded) | 0.091 | 0.003/0.022/0.052 | 0.018 | 724,117 | 0.014 | 407.3 |
| C=0 | 0.093 | 0.003/0.023/0.053 | 0.019 | 729,165 | 0.014 | 419.0 |
| C=24 | 0.173 | 0.000/0.023/0.056 | 0.007 | 1,837,560 | 0.006 | 490.6 |
| C=12 | 0.262 | 0.000/0.025/0.060 | 0.022 | 2,413,951 | 0.005 | 583.4 |
| C=6 | 0.336 | 0.002/0.026/0.059 | 0.003 | 3,773,926 | 0.003 | 658.2 |
| C=0 (drift control) | 0.093 | 0.003/0.023/0.053 | 0.019 | 721,138 | 0.014 | 417.5 |

**Splitting units is a clean loss at 80M.** phaseB rises monotonically with
finer splits (0.093 → 0.336, 3.6x) while per-PE *max* barely moves
(0.052 → 0.059) — so the extra units buy no balance, they only add overhead,
and Pre-traversal rises with them (407 → 658 ms). The drift control returns
to the C=0 value exactly, so the trend is real and not machine drift. At 80M
the pool is already fine-grained (mean unit 0.014 ms); this knob is aimed at
the 2B straggler and should be re-tested there, not judged on this.

---

## 7. Follow-up job 5253386 — the `FOF_S3_GRANT_M2` ladder (6 runs, 2 min 14 s)

Not in the design doc. I added it because §5b's diagnosis, if right, means the
campaign as specified cannot answer its own question — reservation was being
measured through a broken grant sizer. Everything is identical to 5253291's
primary block except `FOF_S3_GRANT_M2`. **All 6 runs exact.**

### 7a. Reserve arm ladder (the 5e7 row is from 5253291)

| `FOF_S3_GRANT_M2` | units/ship | out_units | out_m2/tot_m2 | phaseB | **phaseB_s max** | Pre-trav | Iter0 | ships |
|---|---|---|---|---|---|---|---|---|
| 5e7 (default) | 10.4 / 9.3 | 31,428 / 28,733 | 3.4 / 3.7% | 3.177 / 3.187 | 3.144 / 3.147 | 5.272 / 5.343 | 7.961 / 8.115 | 3021 / 3092 |
| 2e8 | 24.0 | 48,511 | 7.4% | 3.194 | 3.153 | 5.361 | 8.089 | 2018 |
| 1e9 | 69.9 | 61,777 | 12.5% | 3.227 | 3.188 | 6.215 | 8.988 | 884 |
| 5e9 | 176.1 | 83,662 | 23.6% | 2.839 | 2.748 | 5.082 | 7.813 | 475 |
| 1e11 | 307.2 | 84,182 | 24.7% | 2.268 | **2.225** | 5.093 | 7.859 | 274 |

### 7b. Matched noreserve controls

| `FOF_S3_GRANT_M2` | units/ship | out_units | out_m2/tot_m2 | phaseB | **phaseB_s max** | Pre-trav | Iter0 |
|---|---|---|---|---|---|---|---|
| 5e7 (default) | 5.6 | 18,269 | 5.6% | 3.199 | 3.165 | 5.306 | 8.018 |
| 1e9 | 53.9 | 57,172 | 14.0% | 3.302 | 3.232 | 5.616 | 8.382 |
| 1e11 | 220.1 | 93,542 | 34.7% | **2.067** | **2.029** | **4.581** | **7.303** |

### 7c. The diagnosis is confirmed — v2 is reproduced exactly

`noreserve @ 1e11` (m2 budget effectively off, count cap binding) against job
5250364's v2, same knobs:

| metric | v2 `b210b6f` 5250364 | noreserve @ 1e11, `0f30988` |
|---|---|---|
| units per ship | 206–224 | **220.1** |
| phaseB | 2.08–2.18 | **2.067** |
| phaseB_s max | 1.983–2.113 | **2.029** |
| Pre-traversal | 4.47–4.53 | **4.581** |
| Iteration 0 | 6.98–7.24 | **7.303** |

Five for five. **`FOF_S3_GRANT_M2 = 5e7` is the entire regression** — nothing
else between `b210b6f` and `0f30988` costs anything. `aba7833` is clean, and
so is the reservation code as a *correctness* matter (13 exact 2B runs today).

### 7d. The reservation verdict — a net loss where it matters

Head-to-head at matched grant budget, `phaseB_s max`:

| `FOF_S3_GRANT_M2` | s3-reserve | s3-noreserve | verdict |
|---|---|---|---|
| 5e7 | 3.144 / 3.147 | 3.165 | tie (both strangled) |
| 1e9 | 3.188 | 3.232 | reserve −0.04 s (noise) |
| **1e11** | **2.225** | **2.029** | **noreserve wins by 0.196 s — reservation 9.7% WORSE** |

The apparent small edge for reservation at 5e7 and 1e9 is an artifact of the
strangled budget. Once grants can actually be sized (1e11), reservation is a
clear loss, and it loses on every axis at once: fewer units moved (84,182 vs
93,542), less work moved (24.7% vs 34.7% of pool m2), worse wall-clock
(Pre-trav 5.093 vs 4.581; Iter0 7.859 vs 7.303), and **6.6x the returned-edge
traffic** (20,441 vs 3,106) for the donor to re-dedup.

That last number is the mechanism of the loss, and it lines up with §5d: the
cursor window feeds grants average-cost units in bulk, which generate far more
edges per unit of useful work than the coordinator's costliest-partition picks.

### 7e. Anomalies

- `ladder-reserve-m2_1e9` shows Pre-trav **6.215** / Iter0 **8.988**, ~1 s
  above its neighbours while its phaseB rows are ordinary; its run took 40 s
  of wall against 17–19 s for the rest. Same signature as `grant128-parts32`
  in 5250425 (§6). Treat as machine noise — it does not affect any conclusion
  (the 1e9 comparison is a tie either way).
- **Parser artifact in my sbatch, not in the code**: the `RESERVE TOTALS:
  windows=N` figure printed in the `.out` files of 5253291/5253386 is wrong —
  that awk `END` block uses `NR`, which counts every line of the log rather
  than the matched ones. The correct window count is the `s3_reserve windows:`
  line immediately above it (69, 68, 66, 61, 64, 67, 70). `total_width` and
  `total_m2` accumulate only on matched lines and **are** correct. Every table
  in this report uses the correct counts.

---

## 8. Conclusions and recommendations

1. **Ship a new `FOF_S3_GRANT_M2` default, or drop the m2 budget.** At 5e7 it
   binds ~25x tighter than the 448-unit count cap and silently erased S3 v2's
   entire win. The hybrid grant's stated intent (§24: the m2 budget stops a
   grant that scoops giants, the count cap stops one that is all dust) is
   sound, but 5e7 is far below this pool's scale (`tot_m2` 6.2e12, mean
   2.67e6/unit). On this evidence the knee sits between 1e9 and 5e9;
   **5e9–1e10 restores v2 behaviour with the budget still nominally active.**
   Whatever value is chosen, express it relative to measured pool m2 rather
   than as a bare constant, or it will silently re-break at the next problem
   size.
2. **Default reservation OFF (`FOF_S3_RESERVE=0`) pending redesign.** It is
   correct (all 21 2B runs exact, plus the `FACTOR=0` 10k gate) and it
   *engages* at 2B (65–70 windows, 66–96% of shipped units sourced from them)
   — the dynamic self-excluded-mean trigger works as designed, and needs no
   recalibration. But it makes grant composition worse, and the cost grows
   with grant size: **~10% on the straggler at PARTS=32, and 35% on the
   straggler / +85% on Pre-traversal at the best cell** (§9b).
3. **The premise behind §27 needs revisiting.** The `tot_m2` denominator msg2
   asked for shows grants were already carrying **7.1x the pool mean** m2 per
   unit without reservation, because the coordinator orders the costliest
   partition. "Donor eats giants, helpers get dust" does not survive its own
   instrument. If reservation is retried, the target should be *displacing*
   the costliest-partition path rather than pre-empting it — the current code
   drains the cursor window *before* `range`, which is exactly what trades
   1.89e7 m2/unit for 6.75e6.
4. **Adopt `GRANT_UNITS_PER_PE=128` + `PB_PARTS=16`** (§6, job 5250425): best
   cell of six, `phaseB_s max` 1.529 s — the campaign's best figure, 6.1x the
   0.25 s floor. Note 5250425 ran `b210b6f`, which predates the m2 budget, so
   reproducing it at `0f30988` requires the budget lifted. **Confirmed in §9:**
   with `GRANT_M2=1e11` it reproduces (`phaseB_s max` 1.572/1.637 vs 1.529)
   and is the campaign's best configuration.
5. **Against the doc's headline question** — "how far does reservation close
   the 6x gap to the 0.25 s floor?" — the answer is **it does not close it at
   all; it widens it.** Best straggler figures end to end: 1.529 s (5250425,
   no reservation, no m2 budget) → 2.029 s (noreserve @ 1e11) → 2.225 s
   (reserve @ 1e11) → 3.14 s (reserve @ 5e7 default — i.e. the campaign
   exactly as specified).

---

## 9. Closing job 5253475 — the best cell, budget lifted (4 runs, ~1.5 min)

Combines the campaign's two findings: 5250425's best cell
(`GRANT_UNITS_PER_PE=128`, `PB_PARTS=16`) with the m2 budget lifted
(`GRANT_M2=1e11`). Interleaved noreserve/reserve/noreserve/reserve, everything
else fixed. **All 4 exact.**

| arm | phaseA | phaseB | **phaseB_s max** | **Pre-trav** | **Iter0** | units/ship | out_units | out_m2/tot_m2 | ret_edges | resv windows |
|---|---|---|---|---|---|---|---|---|---|---|
| best-noreserve-1 | 1.912 | **1.681** | **1.572** | **4.422** | **7.138** | 566.4 | 166,533 | **38.4%** | 3,271 | 0 |
| best-reserve-1 | 1.911 | 3.862 | 2.025 | 9.530 | 12.298 | 945.7 | 150,373 | 18.7% | 21,545 | 65 |
| best-noreserve-2 | 1.906 | 1.742 | 1.637 | 4.483 | 7.182 | 550.9 | 163,627 | 37.6% | 2,860 | 0 |
| best-reserve-2 | 1.910 | 2.434 | 2.293 | 6.919 | 9.699 | 860.6 | 162,653 | 25.8% | 19,473 | 68 |

### 9a. §6's best cell reproduces at `0f30988` once the budget is lifted

`best-noreserve` vs 5250425's `grant128-parts16` (which ran `b210b6f`, before
the m2 budget existed):

| metric | 5250425 grant128-parts16 | best-noreserve @ 0f30988 |
|---|---|---|
| phaseB | 1.698 | **1.681 / 1.742** |
| phaseB_s max | 1.529 | **1.572 / 1.637** |
| units/ship | 531.2 | **566.4 / 550.9** |
| Pre-traversal | 4.584 | **4.422 / 4.483** |
| Iteration 0 | 7.277 | **7.138 / 7.182** |

Recommendation 4 of §8 is confirmed, and this is **the campaign's best
configuration to date**: `phaseB_s max` **1.572 s** against the 0.25 s floor
(6.3x, down from v2's 7.9x and this build's default 12.6x), with the best
Iteration 0 of any run in the campaign (**7.138 s**, vs the 8.01 s unsliced
baseline and v2's 6.98–7.24).

### 9b. Reservation at the best cell is not marginal — it is severe

| metric | noreserve (mean) | reserve (mean) | reservation cost |
|---|---|---|---|
| Pre-traversal | 4.453 | 8.225 | **+85%** |
| Iteration 0 | 7.160 | 10.999 | **+54%** |
| phaseB (timer) | 1.712 | 3.148 | +84% |
| phaseB_s max | 1.605 | 2.159 | **+35%** |
| out_m2/tot_m2 | 38.0% | 22.2% | **−42% of work moved** |
| ret_edges | 3,066 | 20,509 | **6.7x** |

At PARTS=32 (§7d) reservation cost 9.7% on the straggler; here it costs 35% on
the straggler and **nearly doubles wall-clock**. The larger the grants, the
more damage it does — consistent with the mechanism, since bigger grants mean
more of each shipment is sourced from the cursor window (79% and 66% of
shipped units here, `resv_shipped` 118,639/150,373 and 106,790/162,653).

Note also that reservation ships **larger** grants (861–946 units vs 551–566)
that carry **less work** (out_m2/tot_m2 18.7–25.8% vs 37.6–38.4%) — grants
inflated with average-cost units. That is §5d's composition inversion, now
visible at the level of wall-clock rather than histograms.

The reserve arm is also unstable: Pre-traversal 9.530 vs 6.919 across two
identical reps, against 4.422/4.483 for noreserve. Two reps is thin, but the
direction is unambiguous and consistent across all four grant configurations
tested today.

### 9c. Final ranking, `phaseB_s max` (0.25 s granularity floor)

| rank | configuration | phaseB_s max | Pre-trav | Iter0 |
|---|---|---|---|---|
| **1** | **GRANT=128 PARTS=16 M2=1e11, no reserve** | **1.572** | **4.422** | **7.138** |
| 2 | GRANT=128 PARTS=16, `b210b6f` (5250425) | 1.529* | 4.584 | 7.277 |
| 3 | GRANT=64 PARTS=16, `b210b6f` (5250425) | 1.838 | 4.322 | 7.021 |
| 4 | GRANT=32 PARTS=32 M2=1e11, no reserve | 2.029 | 4.581 | 7.303 |
| 5 | v2 `b210b6f` (5250364) | 1.983–2.113 | 4.47–4.53 | 6.98–7.24 |
| 6 | GRANT=128 PARTS=16 M2=1e11, **reserve** | 2.159 | 8.225 | 10.999 |
| 7 | unsliced base (5249400) | 3.148–3.221 | 5.32 | 8.01 |
| 8 | **the campaign as specified** (reserve, M2=5e7 default) | **3.144** | 5.272 | 7.961 |

\* single rep at a different commit; ranks 1 and 2 are the same configuration
measured either side of the m2 budget and should be read as one result,
`1.53–1.64 s`.

The bottom two rows are the point of this report: **the configuration the
design doc asked me to run is the second-worst of everything tested**, and it
is worse than doing no stealing at all was a generation ago — entirely because
of a default that changed underneath the campaign.

