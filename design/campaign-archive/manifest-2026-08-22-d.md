# uploads/ — 2026-08-22 12:50

**START HERE: `relay83.txt`.** Two items you asked for, and one of them made me
retract a number from the other.

The previous batch — including the 3.1 GB trace tar — is cleared, on your
"everything from relay80/81/82 is processed and landed". The traces themselves
are still on scratch at `s3ab/5326926/traces/` if they are ever wanted again.

## 1. The sharding measurement (relay83, job 5327046, all arms EXACT)

`climb_local_hops_log2`, within-chare hops per climb episode — each becomes an
intra-process message under sharding. CPU, 2 reps per arm, sizes off in both:

    arm                    b0        b1       b2      b3   total local hops
    A localcomp ON    230,538   131,849    2,077      11   136,122
                      213,676   161,993    2,089       9   166,265
    B localcomp OFF   188,431   142,635   15,146   2,785   190,306
                      203,998   130,736   21,977   2,822   191,944

**About 60% of climb episodes take zero local hops** — the parent is already
remote — and nearly all the rest take exactly one. Disabling compression raises
total hops only 26%, but moves the tail 250-fold (4–7 hops: ~10 → ~2,800). So B
bounds the volume from above and the bound is not alarming: **~151k
intra-process messages per run today, ≤191k unshortened.**

`local_union` against total submissions:

    CPU   local_union 787–795k (63.5%)   distributed 451–455k   total ~1.24M
    GPU   local_union  52–54k (10.9%)    distributed 434–436k   total  ~487k

**The GPU arm is a different problem entirely.** `FOF_GPU_PHASE1` forces
peSets=1, so phase 1 already merged the intra-process pairs and there is almost
no fast path to lose. Putting the two costs side by side:

    cost                        CPU arm          GPU arm
    intra-process hop messages  ~151k (≤191k)    ~123–149k
    local fast path lost        ~790k edges      ~53k edges

**On CPU the dominant cost is losing the fast path, not the hops — about 5:1 —
and that cost is manufactured by the PE-set split rather than being intrinsic.
On GPU both costs are small and comparable.** One caveat on budgeting from this:
sharding converts the quantity it is being measured against, since pairs that
are same-chare today stop being so. The table bounds the conversion; it does not
predict the steady state.

Also: **local path compression is worth nothing on the wall.** A 4579.5
[4565.7..4593.4] against B 4562.3 [4528.1..4596.4], overlapping. That is the
third independent chain-shortening mechanism to buy no time, after the wave
and the backward short-circuit.

## 2. The GPU headline — retracted, and I am glad you offered the reps

You offered to fold in another GPU pair if I wanted it harder than n=2. I did,
and it changed the answer:

    relay80  szON 2630.9                szOFF 2562.2 [2547.9..2576.5]  −2.61% SEPARATED
    relay83  szON 2625.4 [2592.7..2658.1]  szOFF 2598.2 [2590.7..2605.7] −1.04% OVERLAPS
    POOLED n=4                                                          −1.83% OVERLAPS

relay80's separation rested on a sizes-on pair whose two reps landed 0.012 ms
apart — a warning sign, not a precision claim. **The GPU effect is not
established and 2562.2 / −2.61% must not be quoted.** The CPU effect does stand:
relay79 −1.92% and relay82 −2.41%, both within-job pairings with separated
ranges. Set the knob on both arms regardless — free, EXACT everywhere, direction
never reversed.

**And I overstated the CPU arm's stability.** Sizes-off across four jobs: 4541.0,
4464.8, 4467.2, 4579.5 — a 115 ms (2.6%) between-job spread, as large as the
effect. I called that arm "strikingly stable" on three points in relay80; the
fourth refutes it. Retracted, and never compare walls across jobs.

## 3. RECOMMENDATION-affinity-fix.md, rewritten

Against the current stack: v79 binaries and md5s, `FOF_UF_SIZES=0` in both
runnable configs, build rules unchanged. Because of item 2 it does **not** carry
2562.2 or −2.61%; headlines are given as ranges across jobs, with an explicit
warning that between-job spread is as large as some of the effects. It also
fixes the stale leaf default (now 32, paratreet2 005b76f) and notes that the
prefix/sort landing, not this knob, accounts for most of the drop from the old
2825–2882 figure. Previous version kept at
`reports/RECOMMENDATION-affinity-fix-2026-08-21.md.bak`.

## A gate bug of mine

relay83's `gpu-szON` arms print "!!! sizes knob did not take". That is wrong —
when I rewrote `run()` for this job I dropped the `if [ "$sz" = 0 ]` conditional,
so the gate demanded sizes-off behaviour from every arm. Those runs are correct.

## Files

    relay83.txt                    the measurement in full
    RECOMMENDATION-affinity-fix.md the rewrite
    relay83-sharding-16n.sbatch    the job
    build-v80.sh                   binaries from clean trees at 724782d/cd2d9c8
