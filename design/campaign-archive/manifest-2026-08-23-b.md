# uploads/ — 2026-08-23 12:05

**START HERE: `relay93.txt`** — jobs 5331227 / 5331367 / 5331402, 128 nodes,
**all 32 arms EXACT**. Code is on a branch (below); this folder is findings only.

## The stall is broken

    cap      Iter0                  walk   loadCache
    unset   1810.7 / 1852.3        0.417   0.342/0.364
    2048    1553.7                 0.508   0.028
    512     1540.8 [1534.6..1551.9] 0.509  0.018
    128     1499.5 [1467.2..1553.6] 0.469  0.016   <- BEST
    1       1600.8 [1494.1..1724.5] 0.563  0.012
    64-node reference 1728.9

**128 nodes is now 13.3% faster than 64.** Genuine interior optimum at ~128:
too large and the O(P²) ship dominates, too small and the walk pays — `-s 1` is
+101 ms with a 230 ms spread against 86.

## It is not the trade I predicted

    arm     cached_nodes    requests    canopy_fills   per process
    unset    131,038,862  12,098,928             0
    128       95,813,837  12,002,873       347,592          339
    1         95,504,825  11,974,523       371,745          363

`cached_leaves` and `cached_particles` unchanged to three digits across every
cap. **347,592 fills over 1024 processes is 339 per process — the uncapped run
broadcasts 34,835 canopy entries to every process and each needs about 1% of
them.** Not a trade between two costs; declining to broadcast waste.

And `-s 1` is worse on only 7% more fills, so it is *which* levels, not how
many — the top levels are wanted by everyone and served by a handful of chares,
which turns a broadcast into a fan-in.

## TreeCanopy serialisation confirmed at 128 nodes

    recvTC             69,670 calls   1 PE (PE 0)   37.6 ms, over a 481 ms window
    recvData(canopy)  557,360 calls   7,167 PEs     208 apiece

One PE doing 69,670 things nobody else does, beside a tree reduction already
running in parallel. **But I over-predicted the cost** — relay92 §11 said ~0.14 s
from a 2 µs/invocation assumption; it is 37.6 ms. Item 2 is justified on
structure, not on a large measured cost, and you should weigh it that way.

## Two corrections of mine

relay93's cap-512 anomaly **did not survive** a third rep (1634.3 → 1540.8, now
the tightest arm). And relay93's "requests flat" was measured on the wrong arm —
`requests_served` misses the `restoreData` path, which is exactly what the cap
redirects; `canopy_fills` fixes it.

## §4 answers your `-s` formula question

Model only, not measured: both cost terms carry P, so **P cancels** and
`s* = sqrt(β/α)`. The discriminating test is cheap — if that holds, `-s 128` is
also optimal at 16 and 64 nodes; if the optimum moves as 1/P, the lazy term is a
latency chain and the model is wrong. Clustering is where I expect real
dependence and the model does not capture it.

## Files

    relay93.txt                 the above in full
    relay93-ssweep-128n.sbatch  the -s sweep
    relay94-s1-128n.sbatch      -s 1 and the 512 recheck
    relay95-canopy-128n.sbatch  canopy_fills + the 128-node trace
    build-v87.sh                the binaries
