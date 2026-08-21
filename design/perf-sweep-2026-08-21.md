# FoF3 on Frontier, 2026-08-21: parameter sweep at 16 nodes, and 2B scaling 8-128

Two questions, one afternoon, one build: **what are the best launch
parameters for the device phase-1 path**, and **how does 2B scale from 8
to 128 nodes on the current stack**. Cosmo25 (`cosmo25cmb.768g2_dm.001024`,
N = 1,981,808,640), OctDecomp, `-u dist`, `-c stats`, 8 processes/node,
one MI250X GCD per process. No Projections tracing anywhere — the binary
is not trace-linked, so none of the numbers below carry trace hooks.

**Every arm is gated on the gold pair — 424,897,832 components,
max_size 185,317,566.** 69 of 70 timed arms produced it exactly; the one
that did not is the `poll2` hang in section 4, which never reached an
answer at all.

## The stack

| repo | ref |
|---|---|
| paratreet2 | `423f242` (merged main: device engine + helper-thread affinity fix + ppn-7 record) |
| charm | `90ef159b9`, branch `reconverse-specific-build` |
| reconverse | `6f34e68` **+ 262 uncommitted local insertions** across 6 files |
| binary | `examples/fof3/FoF3`, md5 `75d087b31373d0262d2f3428e47af661` |

The reconverse working tree is dirty (`comm_backend_lci2.cpp`,
`mempool.C`, `converse.h` and others carry staged-but-uncommitted work).
This is the "non-production Charm++" caveat, recorded precisely so a
later re-run can tell whether it is comparing like with like. It did
**not** produce a regression here — see section 5.

Jobs: sweep `5322236` (16 nodes, 19:21); scaling `5321655`/`5321654`/
`5321653`/`5321652`/`5321651` (8/16/32/64/128 nodes); smoke `5321280`;
diagnostic `5321594`. Scripts and logs in
`/lustre/orion/csc710/scratch/rrao/sweep0821/`.

## 1. The trap that cost five jobs, first, because it is the transferable part

`sbatch --export` splits its value on commas. So

    sbatch --export=ALL,PPN=7,PEMAP=1-7,9-15,17-23,...,57-63 scale.sbatch

delivers `PEMAP=1-7` into the job and silently discards the rest as
malformed assignments. All 8 processes on a node then pinned their 7 PEs
to cores 1-7 — 56 threads on 7 cores — and the whole run came out **8-9x
slow**: 2B/16 went from 4.29 s/iteration to 38.07 s, tree build from 0.80
to 9.38 s. Nothing in a 3 MB log said so except one short line:

    Charm++> cpuaffinity PE-core map (OS indices): 1-7

The fix is structural, not a correction: **nothing containing a comma
crosses `--export`.** `scale.sbatch` now derives `PEMAP` and
`FOF_HELPER_CPUS` from `PPN` and `HELPER_MODE` on the job side, aborts if
the map does not have 8 blocks (ppn 7) or 16 (ppn 14), and echoes the map
it actually used in every arm header.

The lesson generalises past this bug: an oversubscription this severe is
invisible in every application-level number — the run is just *slow*,
uniformly, with no error — so the launch shape has to be **asserted**,
not assumed.

## 2. Parameter sweep, 2B on 16 nodes

One factor at a time from the README baseline (ppn 7, `-l 128`,
`+lci_ndevices 7`, `+backend_poll_thread 1`, affinity fix on its own),
three reps of every arm run as three full interleaved passes. Iteration 1
(steady state), mean of 3, milliseconds.

| arm | iter1 | spread | vs base | phase1_dev | walk | tbuild |
|---|---:|---:|---:|---:|---:|---:|
| `leaf96` | 2587.1 | 57.8 | −0.5% | 0.481 | 0.548 | 610.3 |
| `leaf64` | 2596.1 | 60.4 | −0.1% | 0.510 | 0.477 | 641.5 |
| **`base` (l128)** | **2599.5** | 85.2 | — | 0.472 | 0.606 | 589.3 |
| `ndev4` | 2616.4 | 57.2 | +0.6% | 0.473 | 0.606 | 605.4 |
| `ndev1` | 2616.9 | 62.4 | +0.7% | 0.472 | 0.611 | 603.5 |
| `leaf192` | 2738.1 | 44.3 | +5.3% | 0.482 | 0.762 | 578.6 |
| `leaf32` | 2858.0 | 16.8 | +9.9% | 0.623 | 0.484 | 718.6 |
| `leaf256` | 3124.6 | 120.4 | +20.2% | 0.498 | 1.143 | 569.9 |
| `leaf12` | 3373.8 | 56.2 | +29.8% | 0.638 | 0.555 | 965.0 |
| `ppn14` | 3948.8 | 345.6 | +51.9% | 0.533 | 0.876 | 723.9 |
| `ppn14_ccd` | 3950.3 | 151.5 | +52.0% | 0.534 | 0.823 | 722.4 |
| `helper_ccd` | 4055.2 | 156.9 | +56.0% | 0.524 | 0.774 | 827.2 |
| `nofix` | 4061.9 | 257.4 | +56.3% | 0.530 | 0.756 | 813.4 |
| `cpu_l12` | 5216.2 | 34.4 | +100.7% | — | 0.599 | 780.3 |
| `poll2` | **HANG** | — | — | — | — | — |

**Leaf size: a broad basin at 64-128, and it is flat inside.** 2587,
2596, 2599 across `-l 96`, `-l 64`, `-l 128` — a 12 ms range against
58-85 ms of within-arm spread, so those three are indistinguishable and
`-l 128` needs no change. Outside the basin it degrades fast in both
directions: +10% at 32, +30% at 12, +20% at 256. This settles what
design/phase1-gpu.md 16.2/16.3 explicitly left open — that sweep measured
the *phase-1 kernel only*, at 80M, and could not speak to tree build or
phase 3. Measured whole-iteration at 2B, the same answer holds, and the
two ends fail for opposite reasons the table shows directly: at `-l 12`
the cost is tree build (965 ms vs 589) and the device pass (0.638 vs
0.472 s); at `-l 256` it is the phase-3 walk (1.143 s vs 0.606).

**ppn 7 wins by 34%, bigger than the record had it.** The campaign
recorded ppn 7 beating ppn 14 by ~27% on the GPU arm; here it is
2599.5 vs 3948.8, i.e. ppn 14 is +51.9%. The ppn-14 rescue config
(`--core-spec=0` + `FOF_HELPER_CPUS` on the CCD-first cores) does not
rescue it: 3950.3, statistically identical to plain ppn 14. Note also
`ppn14`'s 345.6 ms spread against ppn 7's 85.2 — corroborating
STACK-STATE.md's calibration that ppn-14 single-rep spread is ~400 ms,
not the ~3% once assumed.

**The affinity fix is worth 36% on this stack** (2599.5 vs `nofix`
4061.9), up from the −22.4% measured on merged main at job 5319480. It
is the largest single knob in the table.

**`ndev` and `poll` do not matter — except when `poll` hangs.** ndev 7,
4 and 1 land within 18 ms of each other, inside spread. See section 4 for
`poll2`.

**Device vs CPU chain at 16 nodes: 2.007x** (2599.5 vs 5216.2), with
`phaseA` alone accounting for 2.551 s of the CPU arm.

## 3. 2B scaling, 8 to 128 nodes

Three arms per node count — device at the swept-best leaf, device at
leaf 12 (apples-to-apples: same tree, same labels, same phase-3 work as
the CPU arm), and the CPU chain at its own best leaf — three reps each,
45 arms, all exact. Iteration 1, seconds.

| nodes | device `-l 128` | device `-l 12` | CPU `-l 12` | GPU/CPU | vs 8n | ideal |
|---:|---:|---:|---:|---:|---:|---:|
| 8 | **4.837** | 6.273 | 9.459 | 1.96x | 1.00x | 1x |
| 16 | **2.601** | 3.388 | 5.203 | 2.00x | 1.86x | 2x |
| 32 | **1.750** | 2.114 | 2.991 | 1.71x | 2.76x | 4x |
| 64 | **1.267** | 1.430 | 1.951 | 1.54x | 3.82x | 8x |
| 128 | **1.535** | 1.627 | 1.881 | 1.23x | 3.15x | 16x |

Phase columns (device `-l 128`, seconds):

| nodes | phase1_device | upwardPass | loadCache | phase3 walk | uf2 | tbuild (ms) |
|---:|---:|---:|---:|---:|---:|---:|
| 8 | 0.857 | 0.443 | 0.004 | 0.907 | 0.048 | 1387.9 |
| 16 | 0.473 | 0.224 | 0.009 | 0.597 | 0.037 | 601.3 |
| 32 | 0.280 | 0.124 | 0.025 | 0.629 | 0.035 | 254.4 |
| 64 | 0.153 | 0.082 | 0.095 | 0.516 | 0.032 | 109.2 |
| 128 | 0.085 | 0.081 | **0.378** | 0.605 | 0.042 | 79.5 |

**Phase 1 on the device scales essentially perfectly**: 0.857 → 0.085 s
over 16x the nodes, 10.1x. It is not the bottleneck at any scale in this
sweep, and `-l 128` beats `-l 12` at *every* node count — reversing the
2026-08-14 sweep, where leaf 12 was the better whole-iteration
configuration at 64 nodes.

**The 64 → 128 turnover is entirely `loadCache`.** It anti-scales
0.095 → 0.378 s, +283 ms, while the whole iteration regresses
1.267 → 1.535 s, +269 ms. The two match within 5%, and `loadCache`
anti-scales identically on the CPU arm (0.089 → 0.370), so this is the
pre-existing starter-pack broadcast — not something the device path
introduced, and the single thing to fix if 128 nodes is to be worth more
than 64.

**The phase-3 walk is the other flat term**: 0.907, 0.597, 0.629, 0.516,
0.605 s. It stops scaling after 16 nodes and is now the largest
component of the iteration at every scale past 32 nodes. Between it and
`loadCache`, phase 1 is no longer where the remaining time is.

## 4. `+backend_poll_thread 2` is a hang, not a slowdown

At ppn 7 with `+lci_ndevices 7` there is exactly one PE per device, so
poll stride 2 silences the pollers for devices 1, 3 and 5 **permanently**
— the pathology named in reconverse `comm_backend_lci2.cpp:419-423`.
Three reps, three hangs: 150 s timeout, exit 124, no answer. A healthy
arm in the same job is 14-20 s.

It is shape-dependent, which is why the older scripts got away with it:
at ppn 14 with ndev 7 there are two PEs per device, the even ranks cover
every device, and stride 2 is harmless. **`poll 1` costs nothing
measurable** (2891 vs 2943 ms in the diagnostic, inside spread), so there
is no reason to run any other stride on this shape.

The sweep now wraps every arm in `timeout --signal=TERM --kill-after=30
150`, so a wedged arm reports 124 and the job continues instead of the
hang eating the allocation.

## 5. The runtime is not the problem, and is measurably better

The dirty reconverse tree invited the question, so: same input, same node
count, same shape, device Replace at `-l 128` on 16 nodes —

    2026-08-20, job 5314471, ndev 4 / poll 2     Iteration 0   4290.5 ms
    2026-08-21, job 5321594, ndev 7 / poll 1     Iteration 0   2891.4 ms

**−32.6%**, and against the 2026-08-14 sweep the gap is much wider:

| nodes | device 08-14 | device 08-21 | | CPU 08-14 | CPU 08-21 |
|---:|---:|---:|---|---:|---:|
| 8 | 6.843 | 4.837 | −29.3% | 13.360 | 9.459 |
| 16 | 4.984 | 2.601 | −47.8% | 8.268 | 5.203 |
| 32 | 4.205 | 1.750 | −58.4% | 6.502 | 2.991 |
| 64 | 4.008 | 1.267 | −68.4% | 5.332 | 1.951 |
| 128 | 5.883 | **1.535** | **−73.9%** | 5.849 | 1.881 |

The 08-14 sweep's headline problem — GPU/CPU speedup decaying
monotonically to 0.99x, parity, at 128 nodes — **is gone**. It now
bottoms at 1.23x. The 08-14 sweep ran ppn 14 without the affinity fix, so
sections 2's two largest knobs account for most of this; the rest is the
merge and the runtime.

## 6. One silent-failure path worth closing

`helper_ccd` (ppn 7, `FOF_HELPER_CPUS=0,8,...,56`, no `--core-spec=0`)
came out at 4055.2 ms — indistinguishable from `nofix` at 4061.9, i.e.
**no fix at all**. The reason is visible by counting announcements:
`FoFDevice.cpp` prints `affinity fix active (...)` once per process, and
exactly 10 arms printed it — every device arm *except* `helper_ccd`.
Without `--core-spec=0` the CCD-first cores are outside the job's cgroup,
`sched_setaffinity` returns EINVAL, `active` stays false, and the scope
returns having printed **nothing**: not the active line, not the DECLINE
warning.

That is the one path in an otherwise carefully-warned design where a
silent decline can masquerade as success — precisely what the DECLINE
warning exists to prevent. A warning on the `sched_setaffinity` failure
branch would close it. Until then: **do not set `FOF_HELPER_CPUS` at
ppn 7** — the derived SMT-sibling default is both automatic and correct,
and naming the CCD cores is at best a no-op and at worst untraceable.

## 7. Recommended configuration, as measured

```sh
PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1 \
  ./FoF3 -f <input> -d oct -u dist -c stats -l 128 \
  +ppn 7 +pemap 1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63 \
  +lci_ndevices 7 +backend_poll_thread 1
```

- `-l 128`, or anything in 64-128; do not go below 32 or above 192.
- ppn 7, one PE per physical core, no SMT. Worth 34%.
- Leave `FOF_HELPER_CPUS` **unset**. Worth 36%, automatically.
- `+backend_poll_thread 1`. Stride 2 hangs at this shape.
- `+lci_ndevices` is free to choose; 7, 4 and 1 all measure the same.

## 8. What to do next, in order of payoff

1. **`loadCache`.** The whole 64 → 128 regression, +283 ms, and the
   largest term in the iteration at 128 nodes. Pre-existing, arm-
   independent, and the reason 128 nodes is slower than 64.
2. **The phase-3 walk.** Flat at 0.5-0.6 s from 16 nodes onward and now
   the biggest component at scale. With phase 1 at 0.085 s, this is where
   the iteration lives.
3. **The `sched_setaffinity` warning** in section 6 — a few lines.
4. Not measured here, and still open: the classic-charm arm, mixed
   CPU/GPU jobs, and whether the reconverse local modifications should be
   committed or reverted before the next campaign.
