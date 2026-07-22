# Edge submission to UF_2: batch now, streaming later — and the scaling that decides it

Context: `../fof_design_note.md` §3.2, §6, §8; `phase3.md`, `step4.md`. Records
(a) how phase 3 currently hands its merge-graph edges to the second-level
union-find, (b) why that is safe today, (c) a corrected scaling analysis of the
per-process edge buffer vs. process count, and (d) the streaming/interleaved
alternative and when it becomes necessary.

## Current structure: full walk, then batch submit (NOT interleaved)

Phase 3 (`FoFPhase3.h`, `runFoFPhase3Dist`):

```
startDown<FoFEdgeVisitor>(...)   // whole global boundary walk; edges -> per-PE edge_buf3 (SEEN-deduped)
CkWaitQD()                        // wait for the ENTIRE walk to finish
initUF2(...); fireUF2Edges(...)   // only now: batch-submit each PE's whole buffer (union_requests)
CkWaitQD()
find_components(...)
```

The global traversal completes and all edges accumulate in per-PE buffers
before any are submitted to UnionFindLib. No interleaving.

## Why old paratreet interleaved, and why we can batch

Old paratreet unioned at **particle** granularity, so the edge stream was ~N
(every linked pair). Buffering all of them before submission is infeasible, so
it interleaved the walk with union submission to bound the stored set.

The two-level split unions at **fragment** granularity, so we buffer |E(M)|,
which the design note estimates at ~10^3x fewer than N (§3.2). That reduction
is what makes non-interleaved batch submission affordable — it is the payoff
that let us drop the interleaving.

## The per-process buffer vs. process count P (CORRECTED)

An earlier version of this reasoning claimed |E(M)| is fixed by the partition
(citing §7), hence per-process buffer ~ |E(M)|/P shrinks as 1/P. **That is
wrong.** §7 says *placement* (where chares land) doesn't change |M| for a
**fixed** partition. But partition **granularity** scales with P: more
processes -> less volume each -> a physical halo that sat inside one process
gets cut across several -> it becomes several fragments joined by merge edges
that did not exist at coarser P. So **|E(M)| (and the fragment/vertex count F)
GROWS with P.** It is a larger graph distributed to more processes; the
numerator is not constant.

Which grows faster, |E(M)| or P, is **structure-dependent**:

- Merge edges live on the 2-D **interfaces** between process domains. Total
  interface area for P cubic domains ~ P * (V/P)^(2/3) = P^(1/3) * V^(2/3). If
  edges are interface-limited, |E(M)| ~ P^(1/3), so per-process ~ P^(-2/3):
  shrinks, but far slower than the mistaken 1/P.
- The fragment **vertex** count F can grow faster. A compact halo cut by a fine
  grid fragments as ~(d/L_dom)^3; a space-filling / percolating structure (the
  design note's actual cost center) fragments nearly ~P. In that regime
  |E(M)|/P ~ constant — the per-process buffer **does not shrink at all** with
  P.

So per-process buffer ~ F/P with F between ~P^(1/3) (surface/filament) and ~P
(space-filling): the buffer lands between *slowly shrinking* and *flat*. It
stays **bounded** (F is capped by the linked structure, so F/P is capped) — it
will not blow up — but near percolation it is effectively **flat** at whatever
the ~10^6-edges/process (~16 MB) order estimate works out to, not shrinking.
Density **skew** makes it worse locally: one process straddling a dense
filament can hold far more than the F/P average (design note §6.3d,e).

**Conclusion:** we cannot claim "more processes relieves buffer pressure." At
large P the buffer is roughly constant-to-slowly-shrinking in the benign case
and skew-sensitive in the percolating case. Whether it forces a change is a
**measurement** question (see below), not a scaling-argument one.

## Orthogonal to 3b parking (do not conflate)

Streaming and 3b parking (design/step3.md §4-6) are distinct and independent:
- **3b parking** is discovery-side: it prunes redundant node-pair DESCENTS over
  the same fragment pair -> saves WALK COMPUTE (the 16x redundancy). It does not
  change the edge set (SEEN already emits one edge per pair), so it does nothing
  for buffer size.
- **Streaming** is submission-side: it fires edges to UF_2 as emitted -> bounds
  the edge-buffer MEMORY. It does not change walk compute.
Compute vs memory; same SEEN table, different problem. You can have either,
both, or neither. (The design note calls for both: parking is §4.1, and §6.1
"send edges immediately" is streaming -- which our current batch submission is
the one thing that contradicts.)

## The streaming / interleaved alternative

Fire `union_request`s **as the walk emits edges** (per-emit, or in small
flushes), instead of one post-QD `fireUF2Edges`. Then UnionFindLib unions
concurrently with the walk and the per-process buffer never fully materializes
— the old interleaving, re-expressed as streaming. It is a localized change to
the `-u dist` path (replace the batch flush with incremental fires; keep the
SEEN dedup so a fragment pair is still fired at most once).

htram brings a form of this for free: with aggregation, edge emission streams
through bounded tram flush windows rather than accumulating fully, and firing
incrementally lets UF_2 drain them as they arrive. So "interleaving" partly
returns when htram lands (deferred).

## When streaming becomes warranted (triggers)

- **Density skew** — a process owning a giant/percolating fragment (§6.3e) with
  a disproportionate edge share, regardless of P. (Also addressed by the
  giant-fragment split + phase-3 load balancing; streaming is complementary.)
- **The percolating / space-filling regime**, where F/P is flat, so the buffer
  does not shrink with scale.
- **htram arrival**, which makes streaming the natural expression anyway.
- NOT process count by itself — but process count does not *relieve* it either
  (the earlier error).

## What to measure (design note §8.2)

The decision hinges on curves we do not yet have:
- Realized |E(M)| vs P, split into diffuse and clustered contributions.
- Fragment count F vs P (is it ~P^(1/3), ~P^(2/3), ~P on real data?).
- Per-process edge-buffer high-water vs P, and its skew across processes (max
  vs mean), especially near percolation.
The step-3a instrumentation already reports per-PE peak edge buffer; extend it
to per-process and track it across a real multi-process scaling series.

## The countervailing caveat

Even where the buffer is flat, the design note's honest note (§6.3d) stands:
**UF_2 is not the bottleneck; the boundary walk is.** So streaming buys limited
*performance*; its real value is **bounding the buffer** under skew / the
percolating regime. Weigh it as a memory-footprint mechanism, not a speedup.
