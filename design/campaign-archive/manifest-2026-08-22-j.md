# uploads/ — 2026-08-22 21:55

**START HERE: `relay89.txt`.** Items 1 and 2 done; item 3 not run. The patch is
`relay89-paratreet2-cachetouch.diff` — report-only, not pushed.

## 1. Touch census: 76.5% of fetched nodes are never examined

    cache_touch: nodes 40,710,543  never 31,149,891 (76.5%)
                 pruned 0 (0.0%)   used 9,560,652 (23.5%)
    cache_touch_depth: canopy(<=3) 73,302 never 82.2% used 17.8%
                       deep 40,637,241    never 76.5% used 23.5%

Both reps agree to three digits. On relay88's 8.7 GB, roughly **6.7 GB is data
no certificate ever asks about**.

**But `pruned` came back exactly zero**, in both reps and in the canopy split,
and that changes the reading rather than being a footnote. The encoding is the
one you asked for — a monotone raise, so "pruned" survives for a node examined
by ≥1 pair and descended by none. The category is recoverable; it is genuinely
empty.

**The reading that fits: pruning happens before the fetch, not after.** A node
is fetched because a traversal committed to descending into its parent, so
everything the walk actually *asks* about it goes on to use; a node the
certificate would reject is rejected at its parent and never requested. **Which
makes the 76.5% prefetch overshoot, not a pruning defect** — relay88 measured
15.5 nodes per reply, so replies ship a subtree, and 23.5% used is ~3.6 useful
of 15.5 shipped. The lever is the bundle (cache share depth `-D`, now 3), not
the certificate.

**Honest limit:** that rests on my instrumentation returning a true zero rather
than losing the state-1 write (state-2 writes clearly work). One run settles it
— a raw counter of `doOpen` calls returning false with a cached target,
independent of the per-node state. I have not run it, and the two answers have
opposite remedies.

**Instrumentation cost: +86.4 ms, +1.9%** (4578.1 vs 4491.7). Your compile-time
gate was the right call — an env branch on that path would have cost something
similar and would have shipped. The plain binary's line is correctly absent, and
the job gated on that both ways. One patch note: `MAKE_OPTS` must reach `src/`,
`fof/` *and* `examples/fof3/`, since `Node` carries the field — instrumenting
only some layers is an ODR violation on the Node layout.

## 2. Request spread: the premise is wrong, in both directions

    DISTINCT DESTINATION PROCESSES per requesting PE (of 128)
      min 1  p10 3  median 6  p90 10  max 17
    top-1 destination: median 52.1% of a PE's requests
    top-4: median 97.1% (min 59.0%)    top-8: median 100%

**Requests do not spread over 127 processes — the median PE talks to six**, and
four carry 97%. So the "~50 ms to fill a 4-item buffer" arithmetic does not
hold. At process granularity (a nodegroup aggregator's view) the median channel
is 2.32 req/ms → a 4-item buffer fills in **1.72 ms**; per-PE it is 0.55 req/ms
→ 7 ms. If aggregation is built it belongs on the nodegroup, not per PE.

**And the bytes argument is the wrong metric.** Request traffic is 2.9% of
bytes but:

    requestNodes  43.7 ms/PE mean, 18.2% of window wall
    addCache      33.5 ms/PE mean, 14.0%
    otherCache     2.3 ms/PE mean,  1.0%

**Serving requests costs more PE time than processing the fills** — 43.7 vs
33.5 ms, at 14.9 µs per request. The ceiling is a share of 18.2% of wall, not of
2.9% of bytes.

What that does *not* say: most of the 14.9 µs is presumably the lookup and
packing of 15.5 nodes, which aggregation batches the message for but does not
remove. I have not separated per-message overhead from per-key work, so the
addressable fraction is unmeasured.

## 3. m2 dump on the GPU arm — not run

Items 1 and 2 took the turn. Scope unchanged, including that the GPU arm needs
its own headroom map.

## What I would do next, in order

1. The one-run check above — it decides overshoot vs pruning defect, and those
   have opposite remedies.
2. If overshoot: sweep `-D`. Existing knob, wall sweep, and the trade (fetched
   bytes against message count) is now quantified on both sides.
3. Only then aggregation, and only after measuring the per-message overhead
   share of that 14.9 µs.

## Files

    relay89.txt                        the above in full
    relay89-paratreet2-cachetouch.diff the instrumentation (NOT pushed)
    relay89-cachetouch-16n.sbatch      the job
    relay89-reqspread.py               request-spread from a trace
    build-v84.sh                       both binaries, MAKE_OPTS to all layers
