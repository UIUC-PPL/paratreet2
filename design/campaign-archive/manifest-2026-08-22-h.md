# uploads/ — 2026-08-22 20:50

**START HERE: `relay87.txt`.** Job 5328672, all arms EXACT. The held cross-node
census is folded in, as asked. No traces in this tranche.

## The answer: A is not less imbalanced than B — it is slightly worse

    per-PE busy, WALK EPs, narrow window (to walk-99%)
    arm                        mean  median   p90    max   MAX/MEAN
    A  FOF_PE_SETS=1 (OFF)     23.3   20.6   47.6   93.2    4.00x
    B  defaults (AUTO, 7)      24.3   21.3   49.2   94.5    3.89x

Removing the local-local pairs does not reduce the imbalance; it nudges it up.
**By your own rule that closes the within-process walk phase** — the skew is in
the cross-process pairs, which that phase cannot touch by construction, and it
would additionally give up the latency hiding §2 shows is carrying most of the
PE time.

**Top-5 walk PEs, the clincher:**

    A  PE342 PE419 PE361 PE825 PE754
    B  PE342 PE419 PE825 PE754 PE292
    (cpu-szOFF, a different job/day: PE419 PE342 PE825 PE754 PE292)

Four of five shared, top two identical in order, same set recurring across
jobs. **The straggler is a fixed property of the decomposition, not the split**
— your hypothesis, now on a controlled comparison. And because it is stable, a
*static* remedy is the right shape.

Walls, for completeness: A 5144.4 [5131.5..5157.3], B 4507.7 [4471.4..4544.0].
The split is worth 14.1%; arm A is a measurement configuration, not a candidate.

## Fetch-bound: confirmed

    share of window wall     WALK EPs            CACHE EPs
    arm                  mean%  max%  max/mean   mean%  max%  max/mean
    A                     10.2  40.7   4.00x      34.0  64.4   1.89x
    B                      8.4  32.6   3.89x      27.7  57.7   2.08x

Your 33–35% vs 7.6–10.7% reproduces on the current stack. Note **which group is
balanced**: cache is 1.89–2.08× while walk is 3.89–4.00×. The big consumer is
the even one and the skewed one is small — consistent with your ~74 ms bound.

*Method note:* these shares depend entirely on the window. Over the full
walk-active span the same data reads walk 4.7% / cache 15.3%; over the narrow
window 8.6% / 28.4%. Yours are the narrow ones. The script prints both.

## The cross-node census

    arm   edge_SAME_PROC   edge_INTRA_NODE   edge_INTER_NODE
    B        739,044           320,912           174,203
    A              0           315,784           171,568

**Of the 495,115 cross-process edges, 64.8% are intra-node.** A node-level stage
could retire nearly two thirds, leaving 174k to gather instead of 487k.

Two free gates it passed: arm A reports `edge_SAME_PROC = 0` exactly (no
same-process pairs by construction), and arm A's total is **487,352** — exactly
relay86's contracted count, from a separate code path. Contracting away the
split's same-process edges recovers precisely the no-split edge set. Same
integer, two independent measurements.

Arithmetic, not a result: relay86 had gather at +297 ms with a 0.353 s finisher
on 487k. Feeding 174k instead scales that to ~0.10–0.13 s, landing gather near
+50 to +70 ms against dist before charging the node stage. The hierarchy closes
most of the gap and still does not open one.

## m2 scoring — blocked, and here is what it needs

**It cannot be scored from the existing dump.** `relay69-simrules.py` reads
`s3ab/5324186/pairs-C-raw`, a 24-byte record of (src, tgt, visits, leafints) —
no m2. m2 is the pool's static expected-pairs estimate (`FoFPhase1.h:1022`,
`:2773`). It needs a per-pair m2 added to the dump and one run.

What I could score for free is the headroom map:

    MEASURED / current parity   5.22x      w=piece cost         4.62x
    hash coin, w=1              4.97x      w=PE cost (ORACLE)   3.57x
    w=piece degree              4.51x      LPT bound            2.33x
                                           sub-pair bound       2.12x
    heaviest single pair = 47.2% of ideal per-PE load; none exceed it

The best static weight in hand is piece degree at 4.51× against the current
5.22× — about a third of the way to the oracle, and no single pair is big enough
to be the obstacle. m2 is the natural candidate to beat degree since it is the
quantity the pool already trusts (0.87 R² at 2B), but it has to be dumped first.

## Files

    relay87.txt                  the above in full
    relay87-walkbalance.py       per-PE per-group busy, both windows
    relay87-pesets-16n.sbatch    the job
    relay87-unionfind.diff       the census patch (NOT pushed)
    build-v83.sh                 both binaries
