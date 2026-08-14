# S3 parallel helper-side rebuild — Frontier, 2026-08-14

Executes `design/frontier-s3-parallel-rebuild.md`.

- tree: `a8d94ab` (pulled and clean-rebuilt at the start of this session) plus
  one uncommitted patch, `~/software/patches/0006-s3-parallel-helper-rebuild.patch`
  (one file, `fof/FoFPhase1.h`, +122/-23).
- charm: `3d1fdd89f`, production (untraced) throughout.
- machine: Frontier, 16 nodes, 128 processes, `+ppn 14`, `+lci_ndevices 7`,
  `+backend_poll_thread 2`, `--network=job_vni`.
- srun line, identical in every 2B run of every arm:

```
srun -N 16 --ntasks-per-node=8 -t 15:00 --mpi=cray_shasta --network=job_vni \
     --unbuffered --cpu-bind=none --distribution=block:block \
     <bin> -f /lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024 \
     -d oct -u serial +traceoff +ppn 14 \
     +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
     +lci_ndevices 7 +backend_poll_thread 2
```

- environment common to all arms: `FOF_STEALA=1 FOF_STEALA_GEO=1 FOF_PB_M2KEY=1
  FOF_PROCS_PER_PNODE=8 FOF_PHASEB_SLICE_MS=2 FOF_PB_PARTS=16 FOF_S3=1`
  (`GRANT_M2` is 1e11 by default in code as of `c65f735`).

Jobs: 5265850 (gates + 2 reps), 5265958 (4 reps, parbuild4), 5265988 (gates +
4 reps + forced pair, parbuild5), 5266088 (packing knobs). Two earlier jobs,
5265437 and 5265850's predecessor, failed and are documented in section 6
because the failures are the useful part.

---

## 1. What changed

**Prerequisite, done first as the design required: trustworthy helper timers.**
The helper-side S3 counters printed at `finishPhaseB`, which fires at the
helper's *own* merge — and a helper's own merge can precede the helping it
does for others. That print was catching roughly 23 of some 300 shipments.
The print moved to `phase3Stats`, which runs after all phaseB work including
help given to other processes. Every helper-side number in this report comes
from the new print. Without this the experiment could not be read at all, and
none of the numbers below would have meant what they appear to mean.

**The change itself.** `s3Shipment` previously did all of this inside the
entry method, on one PE, while the other thirteen in the process idled:
copy each tree's particle vector, build each tree by placement-new into an
arena, then build the unit list. Now:

- `s3Shipment` takes ownership of the deserialized wire data (`const_cast`
  and move off the marshalled parameter — safe because the generated wrapper
  destroys it after return), sets up empty per-tree slots, and sends
  `drainForeign` to every PE in the process. It builds nothing.
- `drainForeign` gained a build pre-pass. Each arriving PE claims trees off
  an atomic cursor and runs the arena pass itself, then falls through into
  the existing unit drain. Units address trees by index, so no unit may be
  walked until every tree is ready.
- **No barrier and no polling.** A PE with nothing left to claim returns;
  whichever PE completes the last tree sends `drainForeign` to the whole
  process. That is the same last-one-out idiom the record already used to
  fire `s3FinishForeign`. Cost of this coordination, measured: **127 ms out
  of a 21819 ms pre-pass, under 1%** — so the design's named fallback (lazy
  per-unit build) is not needed. The design predicted the entry would fall to
  10-15 ms; measured it is 27.3 ms, because unmarshalling stays in the entry
  (section 3).
- Kept unchanged: per-unit CAS ownership, the owned-units and returns
  termination ledger, arena lifetime on the last drainer, forced mode,
  loopback. The one-outstanding-shipment-per-helper protocol remains out of
  scope, as the design said.

**One further change, found by measurement after the first working version**
(section 4): the per-tree particle vector copy became a move. The old code
copied because `ship` was `const`. The wire data is now owned, and
`buildStealTree` reads only `t.nodes` — it uses the particle vector purely as
backing storage to take pointers into — so handing it the wire's own buffer
is equivalent.

## 2. Gates

Stated per the vacuous-validation lesson: what each scale actually executes.
Three validations earlier this week were vacuous because the code under test
did not run at the scale tested, so each gate below says whether the new path
ran.

| gate | nodes | result vs gold | did the new path execute? |
|---|---|---|---|
| 80M forced, CONTROL binary | 4 | 23707197 exact | n/a — baseline, proves the harness |
| 80M forced | 4 | 23707197 exact | **yes** — 378 shipments through the parallel build |
| 10k forced | 2 | 3549 exact | yes — 15 shipments, small trees; checks cursor correctness, not performance |
| 10k loopback | 2 | 3549 exact | **no** — loopback round-trips within one process and covers the **wire only**. It cannot exercise the parallel build or the cross-PE handoff. Run to confirm no wire regression, nothing more. |
| 80M natural | 4 | 23707197 exact | barely — 85 shipments; this is an idle-path check that the no-steal case still works |
| 2B natural and forced | 16 | 424897832 exact in all 21 runs | **yes** — 463 to 1478 shipments per run |

The control arm is in the gate list deliberately. In job 5265437 the control
failed too, which proved the failure was my harness (missing `+lci_ndevices`,
`+pemap`, `+backend_poll_thread`) and not the code under test. Without a
control arm I would have spent that round debugging the wrong thing — as in
fact I started to.

## 3. Result: the entry method — and a correction

This was the design's target metric. **Two different numbers measure it, and
only one of them is the honest answer.**

| measurement | value | what it covers |
|---|---|---|
| `FOF3STAT` `ship_per_grant_ms` | 0.03 ms/grant | only the function BODY — my timer starts at the first line inside `s3Shipment` |
| **Projections entry duration** | **27.3 ms/call** | the whole entry, INCLUDING unmarshalling the message |

I reported 0.03 ms first. That was an overclaim: the Projections bracket also
covers deserialization of the marshalled `StealShipment`, which runs on that
same PE before any of my code does. At 49 MB per shipment, a 27 ms unmarshal
is about 1.8 GB/s — the right order for a bulk copy, so the number is
consistent with the message size.

Against the POD-wire baseline on the same traced PE subset (job 5266210 vs
the 08-13 capture, 183 vs 180 trace files):

| entry | POD wire | parallel rebuild | change |
|---|---|---|---|
| `s3Shipment` | 50.6 ms/call, 42.5 MB/call | **27.3 ms/call**, 49.1 MB/call | −46% per call, −53% per byte |
| `drainForeign` | 48.00 s busy, 9410 calls | 50.41 s busy, 11304 calls | **+2.4 s** |
| `phaseBChained` | 50.93 s | 50.92 s | unchanged |

**So the serial entry is not eliminated; it is halved, and what remains is
deserialization this change cannot touch.** Removing it needs a different
transport (zero-copy, or a message type that avoids the copy), not a
different build placement.

**The work moved rather than vanished, which is what the design intended.**
`s3Shipment` lost 23.3 ms per call; `drainForeign` gained about the same in
aggregate, now spread across 14 PEs instead of held by one. Total busy time is
essentially unchanged.

**This also explains the size of the end-to-end win.** 477 shipments over 128
processes is 3.7 per process; at 23.3 ms of serial time removed per call that
is about 87 ms per process, against a 3.8 s Pre-traversal — 2.3% predicted
against 3.2% measured (the remainder plausibly from better overlap). The 3%
of section 5 is therefore explained arithmetically, not merely observed.

Caveat on the trace: 42 of the 224 requested PEs wrote no file, all of them in
proc 87's block (1120-1231). Proc 55's block (672-783), the straggler block
this campaign has been following, is complete. Totals above are over 183 and
180 files respectively, so the two are comparable, but per-PE claims about
proc 87 would not be safe from this capture.

## 4. Result: where the pre-pass time actually goes

The first working version showed a 21819 ms pre-pass against 8686 ms of
build. I initially attributed the difference to barrier waiting. That was
wrong, and splitting the number showed it:

| version | prepass | = copy | + build | + coordination |
|---|---|---|---|---|
| parbuild4, 2B natural | 21818.7 | **13005.5** | 8686.2 | 127.0 |
| parbuild4, 2B forced | 344417.4 | **190770.6** | 152997.5 | 649.3 |
| parbuild5 (move), 2B natural | 7568.2 | ~0 | 7476.1 | 92.1 |
| parbuild5 (move), 2B forced | 140457.3 | ~0 | 139916.3 | 541.0 |

The copy was 60% of the pre-pass and it was pure memmove. Replacing it with a
move removed 13 s of aggregate work at 2B natural and 191 s at 2B forced.

**But it does not show end to end** (section 5): parbuild4 and parbuild5 are
indistinguishable in wall-clock terms. 191 s of aggregate copy spread over
128 processes and 14 PEs is about 108 ms per PE against a 13.7 s
Pre-traversal — under 1%, and off the critical path. The change is kept
because it is one line, removes real memory traffic, and carries no risk, not
because it was measured to help.

## 5. Result: end to end at 2B

A/B against the unmodified vetted tip, interleaved in the same allocation,
paired by rep. Two independent experiments, one per variant.

**2B natural**, 4 paired reps each:

| | Pre-traversal | Iteration 0 | phaseB_s max |
|---|---|---|---|
| parbuild4 vs control | +3.20% (t=1.64, 4/4 faster) | +1.86% (t=1.43, 3/4) | +4.27% (t=1.06, 2/4) |
| parbuild5 vs control | +3.20% (t=3.11, 4/4 faster) | +2.20% (t=3.51, 4/4) | +3.56% (t=1.70, 3/4) |
| **pooled, 8 paired reps** | **+3.20%, 122.0 ms, t=3.15, 8/8** | **+2.03%, 132.6 ms, t=3.03, 7/8** | not resolved |

With 7 degrees of freedom, t=2.36 is p=0.05. So Pre-traversal and Iteration 0
are established at p<0.05; **phaseB_s max is not resolved by this data** — its
run-to-run spread is larger than the effect.

**2B forced**, one pair per variant (forced ships whole pools, so it is the
maximal exercise of the helper path):

| | Pre-traversal | Iteration 0 | phaseB_s max |
|---|---|---|---|
| parbuild4 | 16377 → 13660 ms, **−16.6%** | 19016 → 16475 ms, −13.4% | 1.355 → 1.351, −0.3% |
| parbuild5 | 16287 → 13796 ms, **−15.3%** | 18994 → 16542 ms, −12.9% | 1.373 → 1.193, −13.1% |

Both variants agree at about −15%. n=1 pair each, but the effect is an order
of magnitude larger than the natural-mode spread, and it reproduces across two
independent allocations.

**Reading.** The change does what it was designed to do, and the size of the
payoff tracks how much work is being shipped. At forced volume it is large.
At natural 2B volume it is a real but modest 3%, because at that volume the
serial helper entry was not the dominant term. This is worth stating plainly:
the timeline that motivated the change (helper P751 holding a 150 ms block
while 13 PEs idle) was a genuine and now-fixed defect, but removing it buys
3% at production settings, not the larger number the timeline suggested.

Exactness: all 21 2B runs returned 424897832.

## 6. Two failures, and what they cost

Both are mine, and both are recorded because the second was preventable by
the first.

**6a. Missing runtime flags in the gate harness (job 5265437).** My `small()`
helper ran the 80M gates without `+pemap`, `+lci_ndevices 7`, or
`+backend_poll_thread 2`. `+lci_ndevices` must track `+ppn`; with `+ppn 14`
and the default device count the run hangs at cache-manager init. I first
read this as a defect in my own new code. The control arm settled it: the
control hung too. Lesson already recorded in the transport report section 23;
the gate script now always passes the full flag set when `ppn >= 8`.

**6b. A deadlock I wrote into the pre-pass, and the reasoning error behind it
(job 5265437, again).** The build loop breaks when the 2 ms slice expires, to
keep entry methods short. In the callback version I made *both* exit paths
`return`:

```cpp
if (slice > 0 && CkWallTimer() - tb0 > slice) break;
...
if (finished_last)                { wake all PEs; }
else if (built_count < n_trees)   { return; }      // <-- wrong for slice exit
```

A PE leaving on the slice has not finished the last tree, so it returned —
but trees could still be **unclaimed**. If every PE left on the slice before
the cursor emptied, nobody built the rest, nobody became the finisher, and no
wake ever fired. Permanent stall. It reproduced at 80M and 2B and not at 10k,
because at 10k the tree count is small enough that the slice never expires.

The underlying error: the loop had two reasons to stop and I gave them one
ending. Stopping because the slice expired means *I still have work*.
Stopping because the cursor is empty means *others have work I must wait
for*. Only the second should return and wait for the wake; the first must
re-send to itself. The fix is three lines and is in the patch.

This also sharpens a distinction I had collapsed while removing the earlier
polling version:

- Re-sending to **continue your own work** is the normal Charm idiom, and the
  unit drain below this code already does exactly that on slice expiry.
- Re-sending to **check whether someone else has finished** is polling, which
  is what should be a callback instead.

My first attempt spun on a pthread `yield` (wrong — that never re-enters the
Charm scheduler; `CthYield()` is the Charm yield and works only in
`[threaded]` entry methods). My second polled. My third removed the polling
and the legitimate continuation together. The fourth keeps the callback for
waiting and the re-send for continuing.

**6c. An attribution error in my own reading of the data.** I reported that
the barrier was costing more than the build, from `prepass − build`, without
splitting that difference. Coordination was 127 ms; the rest was the particle
copy (section 4). The subtraction was right and the cause was wrong, and it
pointed toward the design's fallback — a substantial rewrite to lazy per-unit
build — when the actual fix was one `std::move`. Recording it because it is
the same shape as three earlier errors this week: inferring a cause from a
number consistent with several causes, instead of measuring which one.

## 7. Packing knobs

Section 14/15 of relay1 judged `FOF_S3_DENSITY=0.25` and `FOF_S3_SPAN_PARTS=4`
negative, but that was measured at the old per-grant helper cost. The design
asks for a rerun if the rebuild win is real. It is (section 5), so job 5266088
reruns base / density / span / both, 2 reps each, all on the same parbuild5
binary so the knob is isolated from the rebuild. Results in section 8.

## 8. Packing knob results

Job 5266088, 2B natural, 2 reps per arm, cycled base/density/span/both within
one allocation, all on the same parbuild5 binary. All 8 runs exact.

| arm | ships | donor ms/grant | helper build ms | Pre-trav mean | Iter0 mean |
|---|---|---|---|---|---|
| base (no knob) | 483 | 49.7 | 7265 | **3562.2** | **6237.9** |
| `FOF_S3_DENSITY=0.25` | 1248 | 9.9 | 4940 | 3595.0 (+0.9%) | 6253.2 (+0.2%) |
| `FOF_S3_SPAN_PARTS=4` | 243 | 178.7 | 13680 | 3974.8 (**+11.6%**) | 6718.7 (+7.7%) |
| both | 496 | 54.5 | 8339 | 3756.8 (+5.5%) | 6443.6 (+3.3%) |

**Density moved from negative to neutral; span is still negative.** Paired by
rep, density is faster once and slower once, a 33 ms difference against a
140-227 ms within-arm spread — indistinguishable from base. Span is slower in
both reps by margins larger than the spread. Composed is worse than either
knob alone.

Defaults stay. The design's reason for retaking the verdict was sound (the
old judgement was made at the old per-grant helper cost), and the answer only
moved for density, from a loss to a wash.

**The mechanism is the interesting part.** Density cut donor cost per grant
by a factor of five, from 49.7 ms to 9.9 ms, and shipped 2.6x as many grants
— and bought nothing end to end. Taken with section 4, where removing 191 s
of aggregate helper-side copy also bought nothing end to end, the two results
agree: at natural 2B volume neither the donor's per-grant collection nor the
helper's per-grant rebuild is what bounds this phase. That is worth carrying
into the next round of work, because several remaining levers on the agenda
(the one-outstanding-shipment protocol, zero-copy transport) target exactly
those two costs and should be expected to behave the same way.

## 9. Still outstanding

- Projections subset capture on the straggler block and one helper block, to
  show the timeline collapse directly. Needs traced binaries built against
  the traced charm; deferred until no job is running, because relinking
  htram/unionfind rewrites shared objects that a running job has loaded.
- `phaseB_s max` is not resolved at natural volume and would need more reps.
