# For tomorrow: helper-thread lifetime and multiplicity

Kale's question, 2026-08-21. A note for whoever picks this up — the affinity
fix is already in and measured (`RECOMMENDATION-affinity-fix.md`,
`reports/relay40.txt`–`relay45.txt`); this is about the *mechanism*, which
matters for the helper-thread taxonomy in `paper-idea-helper-threads.md` and
for other codes that hit the same pattern.

## The question, as asked

> The GPU helper issue arose from a kernel being fired with its helping threads
> bound to the same cpuset (a single cpu/core) as the one making the api call.
> But why did the helper thread stay alive well past the completion of the
> kernel? (they impacted the post-gpu-use phase, right?) And what happens if
> there are multiple kernel invocations then?

## One precision first

The helpers did not inherit a *cpuset* in the cgroup sense — the job's cgroup
was wide (112 CPUs: `1-7,9-15,…,57-63,65-71,…,121-127`). They inherited the
creator's `sched_setaffinity` **mask**, which under `+pemap` is a single
logical CPU. That distinction is why widening the mask at the call site fixed
it while the cgroup was never the lever.

## Yes, they impacted the post-GPU phases — that is why the cost was large

Phase 1 is about 0.58 s of a 3.5–5 s iteration, but the quiescence-detection
episodes we measured sit at 20.2–23.7 s in the trace — union-find, the dual
walk, teardown. The victim PE was absent roughly half of every second
*throughout the run*, not during GPU work. So the damage was almost entirely
post-GPU.

## Why they outlive the kernel — expected answer, worth confirming not assuming

ROCm's helpers are almost certainly **runtime/context-scoped, not
kernel-scoped**: the HSA event/signal handler (KFD waiter), the async-copy
management thread and the completion-callback thread are created lazily on
first use and live until context teardown, because they serve *all* subsequent
GPU activity. Tearing them down per kernel would be absurd. So "kernel
completion" was never their lifecycle boundary.

The measured behaviour fits that. One intruder took 3093 ms of CPU across a
26 s run — about 12% duty — with 2623 ms of runqueue wait, and the *same* 128
victim PEs appeared in episodes 3.2 s apart (20.25 s and 23.49 s). Long-lived,
stable placement.

The second half of the mechanism matters as much as the first: a 12%-duty
thread costs roughly half a core when its co-resident **never sleeps**. Charm
PEs spin. So *still alive* + *occasionally runnable* + *peer spins* = the 16 ms
alternation.

## Multiple kernel invocations

Expected: helpers are created **once** and reused, so more kernels do not
create more threads — they raise the existing helpers' duty cycle, and with it
the fraction of the victim core taken. Two things are genuinely unknown:

- We saw **3 pinned-creator creations per process** (256 `libamdhip64` + 128
  `libhsa-runtime64` over 128 ranks) yet only **one doubled core** per process.
  Either one PE did all the creating — plausible, since `deviceBind` runs on
  `CkNodeFirst(CkMyNode())` — or the other two helpers re-set their own
  affinity afterwards, which is consistent with the three unpinned, near-idle
  threads seen on CPUs 73/74/81 in the thread map. Unresolved.
- If several PEs each make a *first* HIP call, do you get several victims per
  process? Streams may matter too: `GPU_MAX_HW_QUEUES` governs hardware queues
  and could add queue-monitor threads, each inheriting whoever created it. (I
  have not checked that variable's default — verify before relying on it.)

## Cheap experiments, in order

1. **Timestamp the interposer** and correlate creations with phase boundaries.
   Confirms creation is once-only and that thread count is flat afterwards.
2. **Per-phase `schedstat` deltas** rather than whole-run. Does the helper's
   CPU concentrate in GPU phases, or is it spread evenly? Spread means periodic
   work unrelated to kernel activity — exactly what would damage post-GPU
   phases.
3. **Force several PEs to make a first HIP call** and count victims per
   process.
4. **Vary kernel and stream count, and `GPU_MAX_HW_QUEUES`**; watch helper
   count and victim runqueue wait.
5. **`HSA_ENABLE_INTERRUPT`** — if the helper blocks on an interrupt instead of
   polling, its duty should fall and the alternation may vanish *without* the
   affinity fix. That separates "pinned" from "hungry", which is the
   distinction the paper needs.

The affinity fix makes placement moot either way; these questions are about
mechanism and generality.

## Tools

All in `~/software/stall-campaign-residue/tools/`. `interpose-pthread.c` and
`monitor-threads.py` cover items 1–4 as they stand; item 1 needs a timestamp
added to the shim's log line, which is a one-line change.
