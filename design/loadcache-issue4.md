# loadCache at scale (issue #4): where it goes, and lazy canopy placeholders

2026-11-02. FoF3 on the 2B cosmo25 snapshot, shipping GPU config (device
phase 1, leaf 128), 8 processes x 7 PEs per node, stack as in
fof3-2b-scaling.md section 0.

## What loadCache is

`Driver::loadCache` sorts the canopy entries the TreeCanopy elements sent
to PE 0, then broadcasts them to the CacheManager nodegroup as the
"starter pack". Every process installs every entry
(`CacheManager::recvStarterPack` -> `TreeCache::installBoundary`) and
contributes to a reduction back to the driver.

The pack is the internal nodes of the octree above the TreePieces, so it
grows linearly with process count: 34,835 entries (4.46 MB, 128 B each)
for 243,846 TreePieces at 128 nodes; 9,135 at 32 nodes.

## Where it went, 128 nodes, before

loadCache 0.134 s (iteration 1, re-sweep job 5833410, rep 2):

| part | time |
|---|---|
| sort on PE 0 (`sort_s`) | 11 ms |
| per-process install, mean / max (`loadcache_install`) | 42 / 66 ms |
| rest: broadcast, reduction, scheduling | about 57 ms |

Install was not the 35K pack entries themselves. `insertNode` gives every
installed canopy node a placeholder child for each child slot that is not
a local TreePiece root, and the canopy's children are mostly remote
TreePiece roots. Every process therefore built one placeholder per
TreePiece in the whole tree: about 244K per process at 128 nodes, plus
the 35K canopy nodes, ~250 B each. That is about 60 MB per process at 128
nodes, and the count doubles with each doubling of the node count.

The walk does not need them. The 2-node runs below show eager installs
creating ~242K more placeholders per process than lazy ones over the
whole iteration, so a process's walk asks for almost none of the remote
TreePiece roots under canopy nodes it never opens.

## Change

`PARATREET_LAZY_CANOPY` (src/TreeCache.h, src/Node.h; on by default, `0`
restores the eager install):

- `insertNode` still adopts local TreePiece roots, but leaves the other
  above-TreePiece child slots null.
- `FullNode::getChild` on a `CachedBoundary` node with a null slot calls a
  process-wide hook that creates the `RemoteAboveTPKey` placeholder on the
  asking PE's pool lane and publishes it with a CAS. The PE that loses
  the race uses the winner's placeholder.
- Canopy nodes that are themselves in the pack fill their slot with their
  own install (`swapIn` into a null slot). The pack is in key order, so a
  parent is always installed before its children.
- Read-only enumerations (cache tally, teardown, dot) use `peekChild` and
  never create nodes.
- `getDescendant` uses a fixed array instead of a heap-allocated vector
  for its path (one call per pack entry).

Also in the same commit: `Driver::sortStorage` sorts (key, position)
pairs and gathers, instead of `stable_sort`ing the 128-byte entries. It
keeps the newest of each key, as before. The sort on 65,000 entries
dropped from 9.5 ms to 6.8-7.7 ms at 2 nodes, and at 128 nodes it is 7.3
ms in both arms below. A version that also deduplicated on arrival sorted
in 3-4 ms, but it put 65K hash-map operations on PE 0 during the upward
pass (upwardPass 0.100 -> 0.104 s), so it was dropped.

## Measured

2-node bench, lambb with `-n 50000 -p 50000`, which gives a 32,500-entry
pack, the 128-node size (jobs 5833781, 5833900). Install mean 40-47 ms ->
12-13 ms, max 54-67 -> 13-15 ms, loadCache 68-81 -> 28-31 ms.

At scale, eager and lazy interleaved in one job, 3 reps each, iteration
1 medians (jobs 5833938 at 128 nodes and 5833939 at 64; all 28 runs exact,
label_digest 2e8eaba371ff1d31):

| nodes | arm | iteration | loadCache | install mean / max | phase3_walk | cache node slots |
|---:|---|---:|---:|---:|---:|---:|
| 64 | eager | 0.836 | 0.055 | 16.8 / 27.4 ms | 0.293 | 319M |
| 64 | lazy | 0.781 | 0.021 | 3.1 / 4.5 ms | 0.275 | 247M |
| 128 | eager | 0.742 | 0.128 | 37.3 / 59.7 ms | 0.261 | 634M |
| 128 | lazy | 0.669 | 0.086 | 6.5 / 10.7 ms | 0.252 | 349M |

Speedups (eager / lazy): loadCache 2.62x at 64 nodes and 1.49x at 128;
iteration 1.07x and 1.11x. At 128 nodes the iteration gains 73 ms, more
than loadCache's 42 ms. The walk accounts for another 9 ms; the rest is
not attributed. Cache node slots (all processes, used_nodes) drop 45% at
128 nodes.

## What is left at 128 nodes

Lazy loadCache 0.086 s: sort 7 ms, install max 11 ms, and about 68 ms of
broadcast, reduction and scheduling. The broadcast is now the main term.
Reconverse sends the 4.46 MB pack store-and-forward down an arity-4
spanning tree, about 5 levels for 1,024 processes, copying it for each
child (`src/collectives.cpp`, `CmiNodeBcastForward`). The pack still
grows linearly with process count. Candidates, in order:

1. Measure the broadcast on its own: an empty and a pack-sized broadcast
   plus reduction just before the real one, timed at the driver.
2. Pipelined starter pack: send it in chunks so the tree's levels overlap,
   and install each chunk as it arrives (parents come first in key order).
3. Smaller entries: 128 B per entry; check which `SpatialNode<Data>`
   fields the walk reads from a canopy node.

The `-s` cap (ship only the top of the canopy, fetch the rest on demand)
is the other lever. In August it crossed over rather than having an
optimum: uncapped was best at 16 nodes, and cap 128 was worst at 64
(campaign-report-2026-08.md). It was measured before the walk fixes
(issue #5) and before this change, so its trade-off may have moved.
