# uploads/ — 2026-08-22 21:20

**START HERE: `relay88.txt`.** Item 1 (cache traffic) done from the arm-B trace,
no job needed. Item 2 (per-pair m2) scoped but not run — see below.

## Headline: 2.64M fetches, 8.7 GB in 290 ms, and none of it duplicate

    group          total msgs   total MB    per-PE msgs          per-PE MB
                                            mean   p90    max    mean  p90   max
    requestNodes    2,635,020      253.0    2941  4,171  5,405   0.28  0.40  0.52
    addCache        2,635,019     8716.6    2941  4,278  6,778   9.73 14.28 20.31
    requestData        82,110        7.9      92    166    344   0.01  0.02  0.03

8.7 GB in 290 ms is **30 GB/s aggregate, ~1.9 GB/s per node**. The request side
is only 253 MB — the traffic is almost entirely replies. Skew is 2.1–2.3× on
cache bytes against 3.89× on walk EPs: again the dominant consumer is the more
balanced one. requestNodes:addCache is exactly 1:1, so there is no retry or
amplification in the protocol.

## Both duplicate mechanisms are zero *by construction*

That is a stronger answer than a small measured number.

**Same key, multiple PEs in a process:** `CacheManager` is a **nodegroup** — one
branch per process shared by all 7 PEs. There is no per-PE cache to duplicate
into; a second requester hits the shared entry or joins the pending request.

**Refetched across the walk:** **there is no eviction path at all.** Nothing in
`TreeCache.h` or `CacheManager.h` evicts; the only clearing is `destroy()`,
which wipes everything between *iterations*. At `-i 1` a fetched node can never
be dropped, so it can never be refetched.

The counts agree: 20,586 addCache messages per process × 15.5 nodes each ≈
319,000, against 318,415 distinct nodes cached per process. About one fetch per
distinct node.

**So a cache-fetch reduction cannot come from deduplication.** It has to come
from fetching *fewer distinct* nodes (pruning), fetching them *earlier*
(prefetch, to overlap the 28–34% of wall in cache service), or making each node
*smaller* (214 B today).

## A labelling bug in a line you already print every run

    FOF3STAT cache: pool_MB 100924.9 used_nodes 40757191 cached_leaves 29315631
      cached_particles 263289108 ... amplification 0.133 avg_MB 837.8 max_MB 1070.0

`FoF3.C:783` packs `{pool_bytes, cached_nodes, cached_leaves, cached_particles,
total_bytes}` — the second element is **cached_nodes** — but the printf labels
`sums[1]` as `used_nodes`. Those are different quantities in `TreeCache.h:237`
(`used_nodes` is pool slots including placeholders), and the real `used_nodes`
is never reported. The 40,757,191 is fetched content, which is the number you
want, under the wrong name.

## The one thing still unmeasured — and it is the valuable one

The certificate-touch fraction. The trace has counts and sizes but no keys, and
`cacheStats` tallies what is *resident*, not what was *read*. If a large share
of those 318,415 nodes per process is fetched and never touched, that is a pure
pruning defect — and it is the only lever section 3 leaves open.

It needs one bit or counter per cached node set at the certificate read sites,
tallied in the post-QD sweep `stats()` already does (it walks the whole cache
tree anyway, so the tally is free). Small, report-only, confined to
`TreeCache.h` plus the read sites in `Traverser.h`. **I have not written it** —
I would rather it be agreed than guessed at.

## Item 2, per-pair m2: scoped, not run

Item 1 was the priority and took the turn. Scope unchanged: add per-pair m2 to
the dump record (today 24 bytes of src/tgt/visits/leafints), re-run on the GPU
arm, score with `relay69-simrules.py`. One caveat for whoever runs it — the
headroom map (current 5.22×, piece degree 4.51×, oracle 3.57×, LPT bound 2.33×)
was measured on the **CPU** arm's leafints and is not transferable; the GPU arm
needs its own dump and its own map, which is the same leaf-128-vs-32 confound
that made relay87 necessary.

## Files

    relay88.txt                  the above in full
    relay88-cachetraffic.py      per-PE cache counts and bytes from a trace
