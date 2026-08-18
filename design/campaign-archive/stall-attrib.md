# Stall attribution: which remedy is keeping the LCI idle-stall away?

Job 5302769, 16 nodes, 2B, 2026-08-18. Binary `bin/FoF3.2b.stagedump`
(`0957364`, production charm, full clean rebuild).
Companion: `reports/relay13.txt`.

## 0. The question, and what was known before this job

The LCI idle-stall (`charm_best_practices.md`, 2026-08-03/04; workaround
landed 2026-08-04) has two measured preconditions:

1. **about a second of quiet** on the wire, and
2. **resumption as a repeated two-way exchange cycled over many distinct
   peers from one thread.** Fan-in alone is clean, fan-out alone is clean,
   and 15 threads with one peer each are clean; only one thread cycling
   two-way over many peers stalls.

Any background traffic anywhere in the job suppresses it completely --
including traffic from ranks not involved in the exchange. That is what
the keep-alive ring exploits: one raw-Converse message per process every
100 ms (`fof/FoF.C:106`).

Two remedies have been live at the same time ever since, and neither has
been removed on the current stack:

* the keep-alive ring (`FOF_KEEPALIVE`, on by default), and
* `+backend_poll_thread 2` in the recommended run line.

Kale's question: the old post-phase-1 stalls fired after 0.5-1 s without
communication, 16-node phaseA is still about 2 s quiet, so the trigger
still exists in this configuration -- yet the E16 projections trace shows
only 8 all-idle gaps >= 5 ms in the whole run and none with the
lone-message signature. Which remedy deserves the credit?

## 1. Before the job: both preconditions really are met

This part needs no new run. Job 5296573's full-machine E16 projections
trace is on Frontier
(`/lustre/orion/csc710/scratch/lvkale/s3ab/5296573/proj-E16`, 1792 PE
logs). Projections `BEGIN_PROCESSING` records the **source PE** of the
message being executed, so an arrival is inter-process when
`src//14 != dst//14`. The keep-alive ring is invisible in these traces --
raw Converse handlers are not traced -- which is exactly right for this
question: what is measured is the **application's own** ambient traffic,
the thing that decides whether the stall's precondition survives when the
ring is switched off.

`scripts/proj-quiet-scan.py`, four processes:

| process | inter-process arrivals | rate | 100 ms buckets with ZERO | longest silence |
|---|---|---|---|---|
| 0   | 166,050 (85.5%) | 13,868/s | 60 of 120 | 1291 ms |
| 5   | 196,493 (80.7%) | 16,404/s | 81 of 120 | **2387 ms** |
| 40  | 288,137 (89.6%) | 24,059/s | 66 of 120 | 1464 ms |
| 100 | 240,832 (84.3%) | 20,112/s | 60 of 120 | 1317 ms |

The traffic is intense in aggregate and extremely bursty in time. Half the
run's 100 ms buckets carry **no inter-process message at all** for a whole
process.

The silences line up exactly with the phase structure. The traced run
prints Decomposition 6.654 s, Tree build 1.006 s, phaseA 2.009 s,
Pre-traversal 2.459 s ending at "Starting tree traversal at time 10.815",
Tree traversal 1.592 s. Against process 5's gap list:

| silence | span (s) | what is running |
|---|---|---|
| 2387 ms | 8.359 -> 10.747 | **Pre-traversal: phaseA (2.009 s) + upwardPass + cache load** |
| 1447 ms | 6.759 -> 8.206 | tree build |
| 2763 ms | 0.695 -> 3.458 | decomposition (particle read/flush) |
| 627 ms  | 11.154 -> 11.781 | inside the phase-3 walk |
| 592 ms  | 11.816 -> 12.408 | inside the phase-3 walk |

Per PE -- the level the bug actually lives at, since it is thread-local --
the longest silence is 2.4-2.8 s on essentially every PE of every process
sampled.

**Precondition 1 is met, several times per run, by a wide margin.**

Precondition 2 is met too, and by a lot. `scripts/proj-resume-fanin.py`
characterises what each PE does in the first 200 ms after its longest
silence ends (process 5, all 14 PEs):

| PE | silence (ms) | resumes at (s) | distinct peer PEs | distinct peer processes | messages |
|---|---|---|---|---|---|
| 70 | 2620 | 10.881 | 302 | 105 | 4127 |
| 71 | 2656 | 10.952 | 154 | 55 | 2485 |
| 73 | 2644 | 10.913 | 219 | 88 | 4890 |
| 80 | 2623 | 10.915 | 265 | 108 | 3776 |
| 82 | 2628 | 10.918 | 269 | 101 | 3518 |
| ... | 2447-2764 | 10.75-10.97 | 139-302 | 48-108 | 656-4890 |

The resumption point is the start of the phase-3 walk, whose remote
node/particle fetches are request/response -- two-way -- against 48-108
distinct peer processes per thread. The LCI microbenchmark that produced
the bug stalled at K=31 peers with two-way traffic. This is K=48-108,
immediately after 2.6 s of silence, on every thread at once.

So the configuration is not merely still capable of triggering the stall.
It is a stronger trigger than the one the bug was characterised with, and
it fires three times per run. Whatever is suppressing the stall is doing
real work, and this job is about which thing it is.

## 2. The job

Job 5302846, 16 nodes, 2B, `bin/FoF3.2b.stagedump` (`0957364`, production
charm, full clean rebuild, md5 `1b5e13a67bee1defe92eb4687d5d805f`; vfmadd 0,
TraceSummary 0, `pe_sets` 1, `phaseA_stages` 1, `stage_pe` 1).
Config identical on every arm and equal to the standing recommendation:
`-u dist`, `FOF_PE_SETS=14 FOF_PE_SETS_MODE=1`, `FOF_S3=0`, the shipped `-E`
default (no `-E` on the command line), `+ppn 14`, `+lci_ndevices 7`.
Four arms, two interleaved reps, `srun -t 8:00` per arm. One warmup run
absorbs the cold read and is excluded.

Both 10k gates exact (3549), `stage_pe` printed only with the dump on,
keep-alive banner present. stderr empty. **All six completed 2B arms
exact (424,897,832).**

## 3. Result: the ring is not what is holding the stall off

| arm | ring | poll | rc | Iteration 0 | Pre-traversal | Tree traversal | phaseA |
|---|---|---|---|---|---|---|---|
| asis-r1  | on  | 2 | 0 | 5183.9 ms | 2408.6 | 1497.7 | 1.966 s |
| asis-r2  | on  | 2 | 0 | 5151.5 ms | 2414.4 | 1476.0 | 1.971 s |
| **ka0-r1**   | **off** | 2 | 0 | **5192.4 ms** | 2422.9 | 1501.5 | 1.979 s |
| **ka0-r2**   | **off** | 2 | 0 | **5118.6 ms** | 2415.7 | 1440.7 | 1.972 s |
| pt0-r1   | on  | 0 -> 1 | 0 | 5242.0 ms | 2434.7 | 1536.0 | 1.987 s |
| pt0-r2   | on  | 0 -> 1 | 0 | 5260.3 ms | 2454.0 | 1516.8 | 2.007 s |
| pt14-r1  | on  | 14 | **143** | **HANG** at cache-manager init, killed at 497 s |
| pt14-r2  | on  | 14 | **143** | **HANG** at cache-manager init, killed at 501 s |

**Switching the keep-alive ring off changes nothing.** Means: as-is 5167.7 ms,
ring off 5155.5 ms -- ring off is 12 ms FASTER, against a 32 ms within-arm
spread. Both ka0 arms print `keep-alive banner = 0`, so the off-switch took.
There is no stall, no inflation, and nothing at the phase boundary.

The regions where the old stall lived are unchanged to the millisecond:

| arm | uf2_setup | phase3_walk | edge_gather | uf2 | relabel | comp_hist |
|---|---|---|---|---|---|---|
| asis-r1 | 24 | 801 | 5 | 69 | 128 | 413 |
| asis-r2 | 25 | 778 | 5 | 69 | 127 | 413 |
| ka0-r1  | 25 | 799 | 5 | 71 | 128 | 416 |
| ka0-r2  | 25 | 738 | 21 | 72 | 128 | 399 |
| pt0-r1  | 34 | 828 | 6 | 75 | 128 | 408 |
| pt0-r2  | 35 | 805 | 5 | 75 | 128 | 411 |

And the stall detector that needs no trace -- wall minus the instrumented
parts inside it -- finds no dark time on any arm:

| arm | Pre-traversal residual | Tree traversal residual |
|---|---|---|
| asis-r1 | 231.6 ms | 0.7 ms |
| asis-r2 | 231.4 ms | 2.0 ms |
| ka0-r1  | 231.9 ms | -0.5 ms |
| ka0-r2  | 230.7 ms | 0.7 ms |
| pt0-r1  | 234.7 ms | 0.0 ms |
| pt0-r2  | 234.0 ms | 0.8 ms |

The Tree traversal wall is accounted for by its own instruments to within
2 ms on every arm, with or without the ring. The Pre-traversal residual is
a constant 231-235 ms of uninstrumented tree-canopy and cache-manager
setup, identical across arms -- not a stall, and not moved by the ring.

**No projections capture was made, because the condition for it did not
occur.** Neither arm 2 nor arm 3 reproduced a stall, and the arm that did
hang did not hang on a stall (next section). The lci-handover bug report
does not get a live reproduction from this stack.

## 4. What `+backend_poll_thread` actually does, settled from the source

The flag is not an on/off switch for a progress thread, and reconverse has
no separate progress thread on this path. Three facts from the source
decide every arm above:

* `scheduler.cpp:182` -- `progress()` is called INLINE at the bottom of the
  scheduler loop by ranks with `rank % backend_poll_thread == 0`, including
  while the rank is idle.
* `convcore.cpp:351` -- `if (backend_poll_thread < 1) backend_poll_thread = 1`.
  **`0` becomes `1`.**
* `comm_backend_lci2.cpp:225` (`initThread`) --
  `nthreads_per_device = ceil(nthreads / ndevices)`, `device_id = thread_id /
  nthreads_per_device`; and `progress()` (line 310) advances **only the
  caller's own device**, never the others.

With `+ppn 14 +lci_ndevices 7` that is 2 ranks per device, `device_id =
rank/2`:

| setting | pollers | device coverage | measured |
|---|---|---|---|
| `+backend_poll_thread 2` (recommended) | ranks 0,2,4,6,8,10,12 | **all 7 devices, exactly one poller each** | baseline |
| `+backend_poll_thread 0` -> 1 | all 14 ranks | every device polled by 2 threads | +1.3% Iter0, uf2_setup 24 -> 34 ms (+42%) |
| `+backend_poll_thread 14` | rank 0 only | **device 0 only; devices 1-6 never progress** | hangs at cache-manager init, twice |

So the recommended `2` is not a tuning preference: with `ndevices 7` and
`ppn 14` it is the unique value that covers every device exactly once.
The rule is `backend_poll_thread x lci_ndevices = ppn`. That also explains
the older `+lci_ndevices must track +ppn` rule recorded in BUILDS.md and
relay3 -- same invariant, seen from the other side.

The pt14 hang is therefore a **device-coverage deadlock at startup, not a
resurrected idle-stall**: it dies in `* Initializing cache managers`,
before the input file is read, before phaseA, before any quiet window
exists. It is real and reproducible (497 s and 501 s, both killed by the
8:00 guard) but it says nothing about the stall.

**Consequence for the question: the progress-starvation direction cannot be
tested with this flag.** Any setting that reduces polling below one thread
per device breaks startup. The only reachable states are "every device
covered once" and "every device covered twice".

## 5. So which remedy deserves the credit? Neither of the two on the table

What the job establishes:

* The keep-alive ring is **not** load-bearing at this scale and in this
  configuration. Removed, twice, the run is byte-identical in answer and
  indistinguishable in every timing region.
* The polling configuration cannot be credited either, because it cannot be
  taken away: the flag has no "less" direction that keeps the job alive.

What section 1 establishes is that this is not because the trigger is gone.
The trigger is present and strong: 2.4 s of application silence per
process, 2.6-2.8 s per thread, three times per run, each ending in a
two-way exchange against 48-108 distinct peer processes per thread.

Two candidates survive for what is actually suppressing it, neither
measured here, both testable:

1. **Warm connections.** The bug's rate decays about 35x over a run
   (`charm_best_practices`, 2026-08-03) and "touch 31 peers once" leaves no
   residue. FoF pushes 13,000-24,000 inter-process messages per second per
   process through decomposition before the first quiet window ever opens,
   so by the time phaseA goes silent every peer pair in the job is hot. The
   microbenchmark's stalls were densest in its first 100 iterations.
2. ~~**Device fan-out.**~~ **WITHDRAWN 2026-08-18 18:00 -- this was already
   measured and is dead.** Job 19624352 ran the pure-LCI reproducer at
   BOTH 1 device/rank and 15 devices/rank: K=1 immune in both, K=8 and
   K=31 stalling in both (0.070%/0.210% and 0.620%/0.430% over 10 ms). The
   note says so explicitly -- "per-thread devices are not a workaround". I
   should have read that table before offering the candidate.

   **What replaces it is larger than either candidate.** Every measurement
   of this bug -- jobs 19608513 through 19661625 -- was made on **Anvil,
   over Mellanox InfiniBand HDR**. Frontier is Slingshot/CXI, through
   libfabric's cxi provider, and the FoF.C comment that describes the
   workaround says "on reconverse/LCI over InfiniBand" in its first line.
   The bug was localised to "below the progress API
   (libfabric/IBV provider)". So the first question is not which remedy is
   holding it off on Frontier; it is whether the bug is present on this
   fabric at all. That is measurable, and section 9 measures it.

Distinguishing them is one job, and it is cheap: run the pure-LCI
reproducer from 2026-08-04 (jobs 19644929 / 19661625) on the CURRENT stack,
with and without a warm-up phase, and with 1 versus 7 devices. If it no
longer stalls at all, the app-side question is closed and the ring can be
retired outright. Say the word and I will run it -- it is a microbenchmark,
not a 2B job.

**Recommendation for now: keep the ring.** It costs nothing measurable
(-12 ms, inside noise, and it is 160 messages/s job-wide), and the argument
for removing it would rest on the two unmeasured candidates above. But it
should no longer be described as the thing that fixed the stalls, and the
`WORKAROUND-lci-idle-stall.md` claim that FoF's uf2-bracket stalls are
suppressed by it is not supported by this stack: they are absent without it.

## 6. The per-PE stage dump (free here) -- and it retires relay12 item 26

`FOF_STAGE_DUMP=1` rode on asis-r2: 1792 lines, `pe pieces self cross`.
Relay12 item 26 proposed a claim priority that prices the MARGINAL CROSS
COST of taking one more piece (a PE holding p pieces owes p(p-1)/2
within-PE pairs, so piece p+1 costs one self walk plus p-1 new pairs), on
the argument that cross work is convex in pieces-per-PE. The dump was
built to settle that. **It does not survive contact with the data.**

Totals, 1792 PEs: self 1768.5 s, cross 102.4 s, cross share 5.47%
(relay12 measured 5.4% from process sums -- agrees). Pieces per PE: min 7,
median 35, max 80, mean 35.7, stdev 10.8.

The correlations that decide it:

| pair | correlation |
|---|---|
| pieces vs cross seconds | **-0.084** |
| pieces vs self seconds | -0.246 |
| pieces vs total phaseA seconds | **-0.237** |
| self vs cross seconds | +0.318 |

**Piece count does not predict cross cost, and predicts total phaseA cost
with the wrong sign.** PEs holding MORE pieces finish SOONER. The claim
pool is already doing the right thing by construction: a PE that draws
cheap pieces comes back for more, so piece count is an outcome of speed,
not a cause of load.

Per-process, the PE that sets the phaseA wall holds slightly FEWER pieces
than its peers (33.9 against 35.8, ratio 0.946) while taking 16.7% longer.
Piece counts are much more skewed than time -- within-process max/mean is
1.490 for pieces against 1.145 for time -- and the two skews barely
correlate (+0.189). Equalising piece counts would therefore make the
imbalance WORSE, not better, even though the convexity arithmetic it was
based on is itself correct (equalising counts would cut
`sum p(p-1)/2` by 7.2%, about 5 s of nominal pair work job-wide).

What actually varies is the cost of a piece: per-piece phaseA cost runs
11.7 / 28.6 / 138.9 ms (min / median / max), a **4.9x spread from the
median to the worst**. That, not count, is what a claim priority would
have to charge.

Where the critical PE's excess sits, summed over 128 processes: cross
13.28 s (60.2%), self 8.79 s (39.8%) -- though the per-process majority
runs the other way (self is the larger term in 62 of 128). So the
within-process critical PE is set apart mainly by CROSS time, which is the
part no piece-count policy can see.

Ceiling, recomputed from per-PE data instead of process sum/max: sum over
processes of max(self+cross) is 154.12 s against 133.63 s for a perfect
within-process split, **-13.3%** of the phaseA critical path. That is the
same quantity relay12 item 23 put at -368 ms of Iteration 0 at 16 nodes,
and it is unchanged. What changes is the mechanism that could reach it:
not a count-based claim priority.

**Verdict on item 26: do not build it as specified.** The next measurement,
if this line is worth pursuing, is whether the existing per-piece cost
proxy (the `m2_*` terms already computed for the load model) predicts the
4.9x realized per-piece spread. A claim priority can only work if it
charges estimated COST at claim time; the instrument to check that is a
per-piece (proxy, realized) dump, not another per-PE one.

## 7. Caveats

* The quiet-window measurement in section 1 counts **Charm-level**
  arrivals. Raw Converse traffic is invisible to Projections -- including
  the keep-alive ring itself, and any raw-Converse runtime chatter. So
  "quiet" there means quiet at the application level, which is the level
  the trigger model is stated at, but it is not proof of zero packets.
* Section 1's trace is job 5296573 (traced binary, E16, same config, same
  tree modulo the stage dump), not one of this job's arms. Traced runs are
  slower; the phase structure and the silences are the same shape.
* Two reps per arm. A stall of the size at issue (0.05-0.7 s) would be
  obvious against a 32 ms within-arm spread, but a rare intermittent stall
  would not be caught by four ring-off minutes of runtime.
* pt0/pt14 vary polling only. Nothing here measures LCI's own internal
  progress threads, if any are configured by `LCI_ATTR_BACKEND=ofi`.
* The `phaseA_skew: within` figure reads 1.35-1.37 on every arm of this
  job against 1.23 on both arms of job 5301010, same config and
  code-identical binary. Consistent within each job, different between
  them -- so it tracks something about the allocation or the claim race,
  not the code. Worth knowing before quoting that number across jobs.

## 8. Files

```
reports/stall-attrib.md              this report
reports/relay13.txt                  the relay
sbatch/stall-attrib-2b-16n.sbatch    the job
scripts/stall-attrib-analyze.py      arm table + the wall-minus-parts stall detector
scripts/stage-pe-analyze.py          the per-PE stage dump (section 6)
scripts/proj-quiet-scan.py           inter-process silences from a projections trace
scripts/proj-resume-fanin.py         peer count at the resumption after a silence
```

Raw logs: `/lustre/orion/csc710/scratch/lvkale/s3ab/5302846`.
Trace used in section 1: `/lustre/orion/csc710/scratch/lvkale/s3ab/5296573/proj-E16`.
