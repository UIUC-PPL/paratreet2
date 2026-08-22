# uploads/ — 2026-08-22 13:20  (UPDATED, nothing removed)

**Your scp is still running, so I have added files and deleted nothing.** The
five trace tars below are untouched.

## NEW: `relay84.txt` — the message-gap analysis, no job needed

**Answer: the receiving PEs are keeping up, comfortably. By your rule that is
the second branch — chain concurrency joins chain length as a closed line, and
the next lever is protocol round-trip count.**

You were right to flag the clocks, and it changed the result. My first pass
reported find-gap medians of 12.9 ms (walk) and 24.3 ms (drain) and I was about
to read them as queuing. Two things say otherwise: the label group produced
*negative* gaps, and the receiver-side numbers cannot support waits that size.

**How big the skew is** — bidirectional test over 593 home-PE pairs. For floor
L and offset d, min(A→B)=L+d and min(B→A)=L−d:

    (minAB+minBA)/2 = true one-way floor    p50 ~104 us
    |minAB-minBA|/2 = clock offset          p50 90 us, p90 10.6 ms, max 66 ms

All 593 sums are ≥ 0, so the matching is sound — but a quantity with a 66 ms
error bar cannot measure a delay that matters at tens of microseconds. Per-pair
minimum subtraction removes only a *constant* offset and barely moved the
distribution (16,682 → 16,531 us), so the residue is drift or real variation
and these timestamps cannot separate them.

**The clock-free answer** (one PE's own clock, no cross-process comparison):

    cpu-szON WALK   home-PE busy 53.4%   executions p50 2us / p90 14us / p99 74us
                    continuous busy runs p50 7us / p90 44us / p99 152us
    cpu-szON DRAIN  home-PE busy  3.7%
    cpu-szOFF WALK  home-PE busy 52.7%   runs p50 7us / p90 46us / p99 163us

A message cannot wait in a queue longer than its receiver is continuously busy —
an idle PE has an empty queue. **99% of busy runs end within 152 µs.** There is
no backlog to relieve.

**So the nodegroup is not justified by anything measured.** Tail concurrency you
had already excluded (3.7% busy in the drain, confirmed). Queuing behind walk
executions is not supported. And real delivery latency (~104 µs floor) is not
something a nodegroup removes — vertex ownership stays with the *process*, so
cross-process messages stay cross-process; sharding only converts within-chare
hops into intra-process messages, which relay83 counted at ~151k per run as
*added* cost.

Round-trip count is the live lever: ~455k distributed edges each paying a
two-phase find, and 16.5% of chains are flips that throw a completed two-phase
find away because the two root ids arrived in the wrong order — ~90k wasted
round-trip pairs per run, at a ~104 µs floor.

Two things I did not establish, in the report: the true *median* latency (only
the floor is measurable), and whether the 104 µs floor is transport or scheduler
pickup — it is a floor either way and neither is addressed by a nodegroup.

Also new: `relay84-msggap.py` (v1, raw) and `relay84-msggap2.py` (v2, with the
per-pair correction and the skew diagnostics). v1 is included because its header
carries the retraction.


The relay81 traces, **split into five tars** because the single 3.1 GB one hit
your file-size limit. Previous batch cleared (all four files verified against
their durable copies first; the manifest is archived at
`reports/uploads-manifest-2026-08-22-sharding.md`).

## Take the small one first — it may be all you need

    relay81-sumd-all4.tar     109 MB    all four arms' .sumd + .sts

**Every attribution result in relay80/81 was produced from these files alone.**
`relay77-waveattrib.py`, `relay78-groups.py` and `relay81-findonly.py` read only
`.sumd` and `.sts`. The drain lengths, the per-group PE-ms tables, the
utilization and active-PE figures — all of it comes out of this 109 MB.

## The full Projections logs, one tar per arm

    relay81-logs-cpu-szON.tar    785 MB
    relay81-logs-cpu-szOFF.tar   771 MB
    relay81-logs-gpu-szON.tar    703 MB
    relay81-logs-gpu-szOFF.tar   687 MB

These are the per-event `.log.gz` files, 896 per arm. You need them only for
per-event work — `relay78-drainanatomy.py` (message volumes per window) reads
them, and so does the Projections GUI. They do **not** contain the `.sts`, so
to open an arm in Projections untar its log tar *and* the sumd tar into the same
directory tree.

**If 700–785 MB is still over the limit, say what the limit is and I will split
further.** The mechanical way, no new tar needed:

    split -b 400M relay81-logs-cpu-szON.tar part-cpu-szON-
    # on the far side:
    cat part-cpu-szON-* > relay81-logs-cpu-szON.tar

## What they are

Job 5326926, the `FOF_UF_SIZES` traced pairs on both devices, from clean trees
at `paratreet2 7263ff1` / `unionfind db73766`. All four arms EXACT. Binaries
`FoF3.cpu-prodtr-v79` `22777303bf63b966fb99ecf4e9d8a429` and
`FoF3.gpu-prodtr-v79` `4062b58854e1ee240d41fd3111f2202b`, both against
`charm-prodtr` (Release + tracing). Linked tracing costs CPU +2.7–3.2%, so these
walls are not production numbers.

Untar to `<arm>/…` — the paths are arm-relative, so all five unpack into one
directory and rebuild the original layout.

Counts verified inside each tar, not on disk: 896 `.log.gz` per log tar, and
896 `.sumd` + 2 `.sts` per arm in the sumd tar (3,592 members).

The originals stay on scratch at
`/lustre/orion/csc710/scratch/lvkale/s3ab/5326926/traces/`.
