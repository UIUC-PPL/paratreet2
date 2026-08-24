# uploads -- 2026-08-24, the GPU pivot

relay98 was uploaded and processed earlier and is not here. **Nothing is
running**; the queue is empty.

Four reports. Two are results, two are corrections that change standing
advice. Read them in this order.

---

## 1. relay106.txt -- the `-s` sweep on GPU, complete. THE RESULT.

Job 5335375, 11 runs, all EXACT, 128 nodes, 2B.

    cap    Iter0 ms   [range]           walk s  loadCache   pool_MB
    U       1528.3   [1461.8..1594.7]   0.477    455 ms     157,673
    2048    1008.5   [1002.1..1014.9]   0.485     20 ms      80,484
    512      940.8   [ 906.3.. 975.3]   0.439      8 ms      76,766
    128      896.7   [ 894.8.. 898.5]   0.399      6 ms      76,021   best
    32       923.9   [ 912.1.. 935.7]   0.431      3 ms      75,560

**Capping is worth -41.3% and halves memory.** A genuine interior optimum:
32 is worse than 128 despite removing more loadCache.

**§2 is the load-bearing part.** `canopy_fills` climbs 0 -> 356,265 and
`phase3_walk` does not rise -- it falls slightly. On CPU the same caps cost
the walk 30-140 ms, which is exactly what made the CPU crossover marginal at
16 and 64 nodes. On GPU the walk side of the trade is free, so capping is
close to pure gain. I cannot say why and have not invented a mechanism.

Not safe at n=2: the ordering among 2048/512/128/32. Read "low hundreds".

## 2. relay108.txt -- helper placement. YOUR ppn 6 IDEA, MEASURED.

Job 5336620, 13 runs, all EXACT. Every placement read back from the binary's
own `affinity fix active (...)` line and gated, not assumed.

    arm  ppn  placement          walk s   Iter0 ms   [range]
    N     7   off                0.725     2471.3   [2214.3..2644.4]
    S     7   SMT siblings       0.426      914.1   [ 840.3..1000.3]
    D6    6   FOF_HELPER_CPUS    0.374      860.2   [ 847.1.. 879.9]
    B6    6   SMT siblings       0.365      857.8   [ 829.5.. 899.4]

- **The fix is worth -63% at 128 nodes**, ranges fully separated (N's best is
  2.2x S's worst). The campaign's -22.8% is a 16-node number.
- **Your ppn 6 works, but the dedicated core is not why.** D6 vs B6 holds the
  PE loss constant and varies only the explicit pinning: 2.4 ms apart,
  overlapping. **The dedicated core buys nothing** over the SMT siblings the
  fix already derives.
- **Flagged, not claimed:** ppn 6 is 6.2% faster than ppn 7 *with 14% fewer
  PEs*, and far tighter (walk range 0.023 s vs 0.156 s). Ranges overlap, so
  the speed is unproven; the tightness is real. My hypothesis is that this is
  the **pollers**, not the helpers -- 7 poll threads per process, and at ppn 7
  the PEs occupy all seven cores. A ppn 6/7 pair on CPU would settle it. Not
  run; do not repeat as fact.
- **A correction to what I said mid-run:** the walk worsens 70% without the
  fix, but Iteration 0 worsens by 1557 ms, so **the treewalk is only about a
  fifth of the damage**. Where the rest lands is unmeasured.

## 3. relay105.txt -- `--core-spec=0`. A STANDING RECOMMENDATION IS WRONG.

Separated out because you can act on it without reading the `-s` work.

    header                              wall     Iteration 0
    --cpus-per-task=14 (no core-spec)     49 s      1514.9 ms
    --core-spec=0 --cpus-per-task=16    1122 s      1516.9 ms
    ... same header, srun cpt=14        1184 s      1539.9 ms

**~1100 s of startup per run, and Iteration 0 unchanged.** Both arms slow, so
it is `--core-spec=0` itself, not the cpuset width.

Your correction is in §2: taking the OS core was never the design. It entered
as the ppn 13/14 escape hatch and got written into the note as if it were the
fix. The code agrees with you -- `FoFDevice.cpp:1050-1098` only widens the
helper's cpuset, and its decline warning names *"leave one CPU per process
unpinned"* first.

**§4 lists what to change.** One item is yours to make: paratreet2 README
line 524 still says to add `--core-spec=0 --cpus-per-task=16` at ppn 13/14.
Still correct *for ppn 13/14*, but it should carry the startup cost. I have
not edited it -- laptop-side, one line.

## 4. relay101.txt -- the first attempt, kept for the mistakes.

Job 5333878, timed out after 4 of 16 arms. Its §1 result is superseded by
relay106. Kept because §3 and §4 record: my sizing error, the 95 minutes I
left the queue empty unwatched, and a mislabel of my own arms -- relay103's
"helpers off" cells were actually SMT-sibling pinning, so what had been
compared until then was two *good* placements against each other.

---

## Scripts

`relay106-gpu-ssweep-128n.sbatch`, `relay108-helper-placement-128n.sbatch`,
`relay105-corespec-startup-128n.sbatch`, `relay104-bisect-startup-128n.sbatch`,
`build-v91.sh` (CPU+GPU from one ref, b0f04fc).

## Open, and stated so it is not mistaken for settled

- **The noise floor.** Unresolved. relay106 spreads were 0.4-8.7% at n=2. The
  collect-gate retraction in relay98 §3b stands: mechanism certain, wall not.
- **Why the lazy canopy refill is free on GPU.** A result without a mechanism.
- **Where the other four fifths of the helper damage lands.** One per-phase
  comparison of an N rep against an S rep would answer it from data already
  printed.
- **ppn 6 vs ppn 7, and the poller hypothesis.** Untested.
- **24B/56B.** relay107 cancelled while pending, per your instruction to
  understand 2B first. One `sbatch` away; it needs `PMI_MAX_KVS_ENTRIES=16777216`
  at 2048 processes (relay100 proved 4194304 too small) and no `--core-spec`.

## Cost of the night, plainly

Two jobs lost to timeout (relay101, relay102 -- the latter produced nothing),
one lost to my dropped `LD_LIBRARY_PATH` line, three diagnostic jobs to find
`--core-spec=0`, and 95 minutes of empty queue before a monitor was armed.
An `ldd` preflight now aborts on the batch node, and a persistent job monitor
runs on every submission.
