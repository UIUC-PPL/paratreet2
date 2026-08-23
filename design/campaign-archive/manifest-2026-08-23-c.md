# uploads -- 2026-08-23, relay98: the collect gate at 64 nodes, and a retraction

relay96/97 archived on your side, so they are removed here. This folder is
relay98 and nothing else: the report, the script that produced it, and this
file. The runs described under "Running now" carry their own scripts when
they report -- they are named here only so you know what is in flight.

## The report

**relay98.txt** -- the collect gate at 64 nodes, job 5333391, COMPLETED,
all 13 runs EXACT.

**Short answer to your question: no.** The gate is worth **42 ms at -s 128,
not ~130**, and **nothing at -s 2048** (+7.1 ms, wrong sign, n=2). It does not
turn 64 nodes into a capping win on its own, because in that allocation
capping already won without it. So the crossover does not move much here, and
**the 24B advice should not lean on this number** -- see below for what it
should lean on instead.

**Read §3b first.** It was added after the rest of the report: a third
measurement (relay99's untraced pair, 128 nodes) gives the gate **-14.8 ms**,
against relay97's -134.8 and relay98's -42.0. The mechanism reproduces exactly
in all four jobs; the wall does not. **I am retracting relay97's -8.3% as a
number** -- it should not go into the README. The merge is still right (fewer
messages, 97x smaller sort, less memory, never slower), but its wall value is
somewhere between ~15 and ~135 ms and we have not measured it better.

Two things in it matter more than the 42 ms:

- **§2. My whole-levels arithmetic was wrong and my own run gate caught it.**
  I predicted 9,362 collected at -s 2048; the true figure is 6,754. The canopy
  is **not a complete tree** -- only 3,377 of a possible 4,681 elements exist
  below depth 5 at 64 nodes, so depth 4 is 68% populated. The formula in your
  commit message is an **upper bound**, exact only while every kept level is
  full. At -s 128 it is exact at both 64 and 128 nodes because depths 0..3 are
  full. Worth a line in the README if the number is quoted there.

- **§3. relay96 and relay98 disagree on the SIGN of cap-128-vs-uncapped at 64
  nodes**, 1824.2-vs-1764.8 against 1750.2-vs-1810.4, both with tight
  separated within-job ranges. It is **not** the build -- 9798a06 gates
  `read_velocity_and_soft` in the NChilada loader only, and our 2B input is a
  single Tipsy file, so v88 and v89main are identical on it. The likely cause
  is allocation variance: neither job used the core-thief fix, and the
  campaign's own note puts unfixed-arm spread at 2-4%, which is 36-71 ms --
  the size of both disagreements.
  **This weakens relay96 §4's 64-node row, which you have already used.** The
  refutation in relay96 §3 stands (it is all within-allocation). But no
  64-node number should place the crossover precisely, and the README rule is
  right in shape while not trustworthy to finer than ~4%. If you want it
  settled, one allocation with 5+ interleaved reps **with** the core-thief fix
  (<1% spread instead of 2-4%) would do it -- say the word.

## Running now (both started 18:23)

- ~~5333830 relay99~~ COMPLETED. Traces are on disk, but its own untraced
  pair (see §3b) puts the effect in that allocation at ~19 ms, not 125, so the
  traces can no longer discriminate the three candidates the way they were
  designed to. I would rather re-aim item 2 at the GPU path once relay101 says
  how big the effect is there. Superseded text follows for the record: traced pair,
  both arms, -s 128, traces kept. Built to separate your three candidates:
  A receive-side runtime cost shows as PE-0 busy-but-not-in-any-EP falling;
  B poll-thread contention shows as ranks 1-6 of process 0 recovering too;
  C queueing shows as the window shortening with PE-0 busy time unchanged.
  Untraced reps run alongside so tracing overhead is checked, not assumed.
  The 66 ms cross-process clock skew is handled by keeping every comparison
  within process 0 or between durations.

- ~~5333837 relay100~~ **FAILED, all arms, before the analysis** --
  `_pmi2_add_kvs: KVS data segment of 4194304 entries is not large enough` at
  2048 processes. A launch limit the campaign has carried since its
  128-process days, nothing to do with 24B; nothing was learned about the
  dataset or the loader. Raised to 16777216 and redone as relay102 below.
  Original description: item 3. U vs -s 128 vs
  -s 2048 on **cosmo25PLK.2304g_dm.004096 at 256 nodes**, matching Ritvik's
  geometry read from scontrol (2048 tasks, 8/node, cpus-per-task 14).
  Memory is reported per arm alongside time, since you flagged it may bind
  first. I could not read his sbatch (permission denied on his scratch), so
  his leaf size and decomposition are unknown to me and this run uses the
  campaign's CPU settings -- **the transferable result is the ratio between
  caps, not the absolute times**. No gold exists for 24B, so arms are gated on
  bitwise self-consistency, the same call as 80M.
  First NChilada directory input, first 24B, first 256 nodes for this
  campaign; if the uncapped arm fails, that is the finding and it is reported
  as one.

## Note on why 24B advice should not come from relay98

The gate's value fell from 134.8 ms at 128 nodes to 42.0 ms at 64 -- the
messages halved but the gain fell 3.2x. The serial fan-in cost grows faster
than the message count, which is the same anti-scaling relay92 measured. That
means the gate is worth **more** at 256 and 512 nodes than either measurement,
and the trend, not the 64-node point, is what Ritvik's advice rests on.


## Added since: the GPU pivot (your 18:18 instruction)

Pulled main to **b0f04fc**, which carries 8e4da9e *and* the width audit
(`FoFPhase1.h`, `Decomposition.{C,h}`, `Splitter.h`) -- real code, so v88/v89
walls do not transfer to it. Built `FoF3.cpu-prod-v91` and `FoF3.gpu-prod-v91`
from that one ref so GPU and CPU can be compared without a second variable.

- **5333878 relay101-gpu-ssweep-128n** (RUNNING) -- the `-s` sweep on GPU, 2B,
  five arms, 3 reps, **with the core-thief fix and a printed spread column**.
  Prediction on the record: capping wins by *more* on GPU, since loadCache is
  host-side and unchanged while the GPU arm roughly halves the iteration.
- **5333879 relay102-gpu-24b-256n** (RUNNING) -- item 3 redone on GPU with the
  PMI fix, `-l 128`, and memory reported per arm.
  Flagging one gap: `-l 128` is the GPU leaf optimum measured on **2B**, used
  here for 24B without evidence. Same gap as `-l 32` on 80M. Ratios hold;
  absolute times are not Ritvik's.
