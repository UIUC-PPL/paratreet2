# The stall campaign: what it found, what it cost, and what not to repeat

Distilled from 87 files (relay19–relay43, 2026-08-19 to 2026-08-21) at Kale's
request, after fable's note: *raw relays go, distilled prose stays.* The raw
material remains on Frontier at `~/software/stallStudies/` if any of this ever
needs re-deriving.

## The question and the answer

**Question.** At 896 PEs the FoF application spent long stretches doing
nothing — 53–81 ms at every phase boundary, six times a run, with no entry
method executing anywhere on the machine. It looked like a communication
problem.

**Answer.** It was not communication, and it was not the runtime. ROCm creates
its helper threads lazily on the first HIP call, from whichever thread happens
to be calling, and `pthread_create` gives a new thread its creator's CPU
affinity mask. Under Charm's `+pemap` that caller is a worker PE pinned to
exactly one core, so the helper is born welded to that core. Because a Charm PE
with nothing to do spins rather than sleeps, the two then alternate at the
scheduler timeslice — the victim PE accumulated 2.7 seconds of runqueue wait in
a 26-second run while every PE with its core to itself accumulated 0.0 ms.

Every collective had to reach that PE, so the cost was
(collectives per run) × (one timeslice). Fixing it is worth **−22%** on the
iteration, on both the old base and the current merged code.

The fix is `gpu-helper-affinity-fix.diff` in the parent folder — one file.

## The four traps to keep

These are the mistakes a future investigation would otherwise repeat.

### 1. Event-sort inversion (Projections)

`BEGIN_IDLE`(14) and `END_IDLE`(15) very often carry the **same microsecond**,
because reconverse's scheduler leaves the idle condition to poll and re-enters
it. Records must be read in **file order**. Sorting by (time, kind) inverts
those pairs and turns the genuine idle period that follows into a phantom
overhead gap. Two false results were published internally before this was
written down.

The same file: `BEGIN/END_PACK`(16/17) and `BEGIN/END_UNPACK`(18/19) **nest
inside** an entry method. They are sub-intervals, not state transitions;
returning to OVERHEAD when a pack ends reclassifies ordinary execution as
overhead. `tools/relay18_state.py` is the one correct state machine — use it
rather than writing another.

### 2. The start-of-trace artifact

The interval from a PE's first record to its first `BEGIN_IDLE` is trace
startup, not a stall. It contributes **exactly one interval per PE** at
t ≈ 0–1 s — 62–85% of the raw 10–60 ms band population at 896 PEs. `open_entry
== -1` identifies it and every real number must exclude it.

What makes it genuinely dangerous rather than merely large: it is **absent from
some arms**, because when the input read was cold that interval ran past 60 ms
and fell outside the band. So an uncorrected band count silently changes
population between arms, and the A/B compares different things.
`tools/relay18-band.py` excludes it; the earlier `relay18-analyse.py` did not,
and its raw numbers are inflated.

### 3. RUNPATH loses to LD_LIBRARY_PATH

`$HOME/software/charm/lib` is on the login shell's `LD_LIBRARY_PATH` and holds
**both** a `liblci.so` and a `libreconverse.so` from the production stack. A
binary's RUNPATH does not win against that. An arm that thinks it is testing
one library can be running another.

Strip every stale path per arm, and **gate on library CONTENT, not on the
path**: `strings <resolved liblci.so> | grep -c FI_THREAD_DOMAIN` gives 2 for
the fork and 0 for upstream. Every job script in the campaign ends with that
gate for this reason.

### 4. The unlocked reproducer against the locked application

The standalone LCI reproducer "died 8 of 8" while the application ran "clean 23
of 23" on the same library in the same shape. The whole difference was one
setting the reproducer never made: reconverse sets

    g_attr.ofi_lock_mode = TRYLOCK_SEND | TRYLOCK_RECV | TRYLOCK_POLL

before runtime init, and LCI's own default is `0` — no lock. The reproducer ran
unlocked; the application did not.

The general lesson: a reproducer inherits none of the embedding runtime's
configuration. Before believing a small reproducer contradicts the application,
diff the runtime attributes each one actually sets.

## Blind alleys, so nobody re-walks them

Every one of these was measured and eliminated. None is the cause.

| hypothesis | how it died |
|---|---|
| LCI fork vs upstream | fork and upstream indistinguishable in the app's own shape; five arms, all exact |
| LCI backlog drain | 0.002 entries per progress call |
| MR cache monitor (`FI_MR_CACHE_MONITOR`) | A/B flat |
| libfabric domain sharing (`+lci_ndevices`) | flat, and backwards from the prediction |
| CXI match mode | flat |
| GPU poller assignment (`+backend_poll_thread`) | the measured order was the reverse of the prediction |
| QD's dependence on idle | real (86% of handoffs wait) but worth 1.54 ms mean; removing it costs 8× more rounds for no wall time |
| QD protocol cost at scale | isolated benchmark settles in 0.45 ms at the same 896 PEs |
| the keep-alive ring | an InfiniBand behaviour; 0 of 160,000 samples over 1 ms on Frontier's Slingshot |

Two lessons that turned out to matter more than any of the above, and that are
already recorded in `charm-notes/`: scope a fabric hypothesis to the fabric it
was observed on, and `+backend_poll_thread` is a divisor rather than a switch.

## The one methodological lesson worth carrying

**Low utilisation is imbalance or a critical path before it is communication.**
Sum busy time per PE first. In this campaign the split was 88% straggler tail,
12% dependence chain, 0% communication — and thirteen rounds were spent on
communication before anyone summed busy time per PE.

Its corollary, learned the hard way in relay35/36: **if you cut a window to a
phase, say so in the output and check what lies just outside it.** Anchoring
the scan at the first walk entry method hid nine quiet stretches — 366 ms —
that had been counted as zero since relay28.

And the trap that closed the case: **when a stretch of trace contains no entry
methods, the entry methods are not the whole record.** `BEGIN_IDLE`/`END_IDLE`
still fire, and Converse-level protocols — quiescence detection, and anything
built on `CcdRaiseCondition` — appear as waves of idle-exits. A window is only
silent at the layer you are reading.

## The patches

Of the eight cumulative patches the campaign produced (0013–0020), **two are
worth keeping** and both are in this bundle or its parent:

- `../gpu-helper-affinity-fix.diff` — the fix. One file.
- `charm-instrumentation.diff` — QD made visible in Projections, plus the
  off-core detector. Two files in charm, independent of the fix, useful in any
  Charm/reconverse investigation.

0013–0017 were diagnostic probes for hypotheses that are now dead (LCI phase
counters, poll-loop instrumentation, `CmiHandleMessage` counters, QD switches).
They are superseded and tied to hypotheses nobody should revisit. 0019 is
superseded by 0020, and 0020's app-side content is the standalone fix above.
