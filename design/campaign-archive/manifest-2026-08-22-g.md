# uploads/ — 2026-08-22 19:20

**START HERE: `relay86.txt`.** Job 5328371, `-u dist` vs `-u serial` vs
`-u gather`, 2 reps each, all EXACT. No traces in this tranche.

## The algebra gate passed, and it is the strongest result here

All three modes printed a **bitwise identical** components line — the full
27-bucket log2 histogram, not just the two gold numbers. The contraction
argument in `design/staged-gather.md` is now tested at 424,897,832 components
and 1.23M edges instead of at 1m, and it reproduces the flat serial labels
exactly.

## The walls

    A  -u dist    4486.6 ms [4465.3..4507.8]
    B  -u serial  5975.4 ms [5960.1..5990.6]   +1488.8 ms  +33.2%  SEPARATED
    C  -u gather  4783.5 ms [4780.4..4786.7]   + 297.0 ms  + 6.6%  SEPARATED
                                        C vs B -1191.9 ms  -19.9%  SEPARATED

Phase-3 rows (s), which explain all of it:

    arm   uf2_setup  phase3_walk  edge_gather   uf2    relabel   total
    A       0.009      0.397        0.001      0.038   0.125     0.570
    B       0.000      0.274        0.009      1.197   0.197     1.677
    C       0.000      0.265        0.018      0.353   0.137     0.773

**Read the walk row first.** A's walk is 0.397 s against 0.274/0.265 — that gap
is not the walk getting faster, it is A's walk *carrying the concurrent
cascade*, which is what streaming buys and what B and C forfeit. 132 ms, close
to relay76's ~104 ms. A then pays only 0.038 s of residue. C wins 132 ms on the
walk and loses 315 ms on the serial finisher.

## Q1 — does contraction repair serial's penalty? Mostly

    gathered edges  B 1,227,700 unique   C 487,352 contracted   -60%
    serial finish   B 1.197 s            C 0.353 s              -70%

**80% of the penalty removed** (1488.8 → 297.0 ms), by exactly the predicted
mechanism. The finisher is superlinear in its input — 40% of the edges cost 30%
of the time.

    FOF3STAT gather: local 739,222   cross 495,102   contracted 487,352

Predicted ~790k/~455k; measured 739k/495k, so local is 60% not 63.5%. The two
count different things and neither is wrong: relay83 counted uf2 *submissions*
(same-chare fast path vs distributed chains), this counts *staged edges* by
endpoint ownership. Worth remembering before quoting either as "the" fraction.

## Q2 — does C approach or beat A? Approaches, does not beat

+6.6% with separated ranges. Both halves were predicted: −132 ms of forfeited
walk concurrency, +315 ms of serial finisher against dist's 0.038 s residue. And
the direction of scaling is against it — the finisher grows with process count
while dist's cascade does not, so the gap widens.

## Q3 — what the root-pair collapse buys: almost nothing

    cross 495,102 -> contracted 487,352      ratio 0.9843

**The collapse removes 1.6%**, far below what the framing assumed. After each
process replaces locally-owned endpoints with local roots, essentially every
cross edge still names a distinct pair of roots — local components are
fine-grained relative to the cross edges.

**This is the part that matters for the hierarchical variant.** C's win over B
is *not* the collapse; it is the local/cross split — 739k edges retired for
being same-process, against 7,750 removed by dedup. So a hierarchical variant
gains from **retiring edges internal to a coarser group** (a node = 8
processes), not from further dedup — and this job does not measure that. What
fraction of the 495k cross-process edges are also **cross-node** is the number
it lives or dies on, and it is one cheap census away (classify staged cross
edges by owner mapped to node). I have not run it; say the word.

## A provenance correction you should know about

`merged/FoF3.gpu-prod-v81`, the GPU binary relay85 cites at md5 `48e8ddcf…`, no
longer exists. Deriving `build-v82.sh` from `build-v81.sh`, my sed renamed only
the CPU output, so the GPU line still said v81 and rebuilt over it from the new
refs. I renamed the overwritten file to `FoF3.gpu-prod-v82` (which is what it
is — a clean a65da5a build, no census patch) so it cannot masquerade, and
appended the correction to `relay85.txt`. No relay85 result is affected and the
binary is exactly reproducible from `patches/relay85-*.diff` on 724782d/cd2d9c8.

## Files

    relay86.txt                  the above in full
    relay86-gather-16n.sbatch    the job
    build-v82.sh                 the binary, from clean trees
