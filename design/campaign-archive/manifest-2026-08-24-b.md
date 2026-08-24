# uploads -- 2026-08-24, second batch: where the helper damage lands, and why capping is free on GPU

relay101/105/106/108 were processed and archived. Two new reports. **Nothing
is running and nothing is queued.** No new job was run for relay110.

---

## 1. relay109.txt -- where the helper damage actually lands

Job 5337328, 128 nodes, 7 runs, all EXACT, 4m15s of allocation. N and S differ
in one environment variable and nothing else.

**It is a uniform tax, and the walk is the phase that suffers least.**

    phase                  N mean   S mean    delta   ratio   share
    uf2                     0.332    0.037   +0.295    9.1x   24.9%
    phase3_walk             0.706    0.457   +0.248    1.5x   20.9%
    upwardPass              0.173    0.034   +0.139    5.1x   11.7%
    uf2_setup               0.196    0.069   +0.127    2.8x   10.7%
    component_histogram     0.224    0.112   +0.112    2.0x    9.5%
    edge_gather             0.108    0.005   +0.103   21.6x    8.7%
    relabel                 0.078    0.019   +0.059    4.1x    5.0%
    loadCache               0.052    0.006   +0.046    8.7x    3.9%
    tip_encode              0.040    0.017   +0.023    2.3x    1.9%
    tip_sentinel            0.030    0.009   +0.021    3.4x    1.8%
    phase1                  0.121    0.107   +0.013    1.1x    1.1%

Deltas sum to 1.187 s against an Iteration 0 gap of 1515.8 ms, so the phases
cover **78%**; the remaining ~330 ms is outside every phase the binary times
and this run does not name it.

**The relative ordering is inverted from the absolute one.** Short phases
suffer worst as multiples (`edge_gather` 21.6x) while the walk, by far the
longest, is 1.5x. That is a **fixed-size preemption**, not a proportional
slowdown — consistent with the 16.006 ms slice the QD-settle work measured,
and it explains why the fix is worth so much more at 128 nodes than the
16-node −22.8%.

Your expectation and the table are both right, and §3 says so rather than
picking one: in absolute terms the walk *is* the largest single contributor at
20.9%, but only because it is the longest phase. Per unit of time in it, the
walk suffers least. "It bites the treewalk" would be a misleading summary.

Arms: N 2475.8 ms, S 960.0 ms — **roughly 2.6× slower without the fix**, at
that precision per your softening.

**You said afterwards this run seemed unnecessary. It had already completed.**
Recorded because the allocation was spent either way. §4 lists two bugs of
mine it exposed: relay108's log-label collision that made the rerun necessary,
and a summary-block edit that silently did not apply, so the job printed the
wrong legend and I computed the table afterwards from files it wrote correctly.

## 2. relay110.txt -- why capping is free on the GPU walk. NO NEW JOB.

Your sharpened test, run on relay106 and relay97 data already on disk.

    cap    leaf_visits   avg/PE    same_frag   walk s
    U        9,907,448   1382.2     193,151    0.477
    128      9,900,835   1381.3     193,151    0.399

**Same work, 16.4% faster.** `leaf_visits` −0.067%, prunes −0.15%,
`same_frag` byte-identical in every arm, `edges_emitted` 166.0 avg throughout.
The dilution reading made a prediction and it held.

    capping U -> 128     pool_MB   placeholders   requests   walk
    GPU                   -51.8%      -53.9%       -0.8%    -16.4%
    CPU                   -29.6%      -30.0%       -0.7%    +31.1%

**Requests are flat on both paths** — no extra miss traffic on either, which
kills the refill-latency story from the other side too.

Three things I will not overstate. `suppression` moves −5.25%, the only work
counter that is not flat, quoted rather than omitted. Uncapped GPU requests are
6.17M against CPU's 12.11M — a factor of two, which I decline to call "far
below". And **the CPU contrast is unresolved**: the CPU gets the same kind of
pool relief and flat requests yet its walk gets 31% worse, and nothing on disk
decides whether that is the fills it pays for or simply the GPU's larger
dilution relief outweighing them.

§3 separates what is established — the walk does the same work, so every
explanation resting on extra fetches or traversal is dead — from what remains
inference, namely that the residue is cache/TLB locality.

**§4 parks PAPI with the exact blocker**, so resuming costs nothing to
rediscover: charm's `detect-features-c.cmake:244` runs a bare
`check_c_source_compiles` with empty `CMAKE_C_FLAGS`, so `papi.h` is not found
regardless of `CMAKE_PREFIX_PATH`. Fixable with an explicit `-I`. Not verified:
whether charm's PAPI path is wired for reconverse at all.

It also records a finding that outlives PAPI: **`module load X 2>&1 | tail -1`
runs `module` in a subshell and discards every `setenv`.** `build-v88` through
`v91` all use that pattern, so their module blocks have most likely been doing
nothing — harmless, because cmake gets absolute compiler paths, but not what
those scripts appear to do.

---

## Files

`relay109-nvss-perphase-128n.sbatch`, `build-charm-papi.sh` (gated so it cannot
waste a compile). `merged/charm-papi/` is a 463 MB configured-but-unbuilt copy
left in place for cheap resumption; `charm-prodtr` is untouched.

## Open

- Why the CPU walk pays for capping while the GPU walk does not.
- The ~330 ms of helper damage outside every timed phase.
- ppn 6 vs ppn 7 and the poller hypothesis, on CPU.
- 24B/56B.
- PAPI, parked at a known line.
