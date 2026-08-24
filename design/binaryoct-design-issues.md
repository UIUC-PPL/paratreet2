# eBinaryOct for FoF: design issues (parked, 2026-08-23)

Not a proposal — a record of what switching FoF from `eOct` to `eBinaryOct`
would buy and cost, written while the reasons were fresh. Kale raised it after
we established what a cache "placeholder" actually is.

## What prompted it

A placeholder is a cache node marking "this key exists in the tree, but its
content is not here" (`Node::Type::Remote` / `RemoteAboveTPKey`, empty
`SpatialNode` with `n_particles = -1`). It is created **eagerly at install
time, not on request**: `TreeCache.h:588-608` loops over ALL `branch_factor`
children of every installed node and makes a placeholder for each one not
already local. Most are never requested — they are the frontier of the cached
region. They are also the only node types carrying an OPEN parked list
(`TreeCache.h:308-317`), so they are what makes park-and-resume work: a walker
reaching one parks, the reply installs the real subtree over it via `swapIn`,
and the parked waiters are drained.

With branch factor 8, one install manufactures up to eight frontier markers.
Measured at 2B / 128 nodes: **7.2 placeholders per cached node, 88% of the
pool by count**, ~248 B a slot, `pool_MB 266,618`.

## The arithmetic — the placeholder case is strong

Identical spatial coverage, oct depth D vs binary-oct depth 3D:

| | content | frontier | total slots | frontier:content |
|---|---|---|---|---|
| oct, depth 3 | 585 | 4,096 | 4,681 | **7.0** |
| binaryOct, depth 9 | 1,023 | 1,024 | 2,047 | **1.0** |

Scale-free: 44% of oct's slots at every depth. The 7.0 predicted here matches
the 7.2 measured, which confirms the mechanism is exactly the fan-out. Binary
pays MORE content nodes (it materialises the intermediate 2- and 4-way splits
oct skips) but those get walked; oct's eight-wide frontier mostly does not.
`FullNode` sizes `children[]` by `n_children`, so binary nodes are physically
smaller too — the win compounds beyond the count.

## The counterweights

1. **The canopy grows 7x.** Canopy elements = (treepieces-1)/(b-1), so at
   243,846 treepieces: 34,835 (oct) -> 243,845 (binary). Uncapped, the
   `loadCache` collect goes from 69,670 messages on PE 0 to ~487,690, and the
   O(P^2) ship carries 7x the payload — the exact pathology `-s` was added to
   fix (relay92/93). IMPORTANT CAVEAT: with `-s` set this mostly evaporates,
   because capping keeps whole levels and for equal spatial coverage binary
   collects only ~1.75x more than oct. So binary-oct makes `-s` a hard
   prerequisite rather than a recommendation.
2. **Depth triples**, so ~3x more `open()` tests along a descent path. Walk
   entry methods are only 8-10% of the walk phase against cache service at
   28-34% (relay87), so this is probably tolerable — but it is 3x on exactly
   the term the walk tie-break work was fighting over.
3. **Box shape — the FoF-specific risk.** Alternating single-dimension splits
   give 2:1:1 and 2:2:1 boxes at intermediate levels. Elongated boxes have a
   larger maxdist for the same volume, so **case 2** (both sides uniform and
   maxdist <= b: emit one edge for a whole subtree pair, no descent) fires
   LESS often at intermediate levels. That certificate does most of the
   pruning work, so it could give back more than the placeholder saving. It
   cuts the other way too — each test rejects half the space instead of
   needing eight — which is the classic kd-vs-octree trade. Not predictable
   from first principles; only measurable.
4. **Every `-D` and `-s` number needs re-measuring.** Share depth 3 in binary
   is 8 nodes against 512 in oct, so the measured `-D` optimum (3, ~15.5 nodes
   per reply) moves to roughly 9-12.

## Blockers before it can even be tried

- FoF hardcodes `eOct` (`examples/fof3/Main.C:44-45`) and `FoFPhase3.h:472`
  enforces matching TreePiece/Partition decompositions, so it is not a flag
  today.
- The non-oct decomposition paths carry `sum_int` on N-scale counts
  (`Decomposition.C:516/528/532`, self-documented as unaudited and unused; see
  design/width-audit-2026-08-23.md #5). Those break at Ritvik's 24B/56B and
  must be widened first.
- `eBinaryOct` DOES already exist as both a decomposition and a tree type
  ("2 children for every node, alternates dimension", branch factor 2, with
  `BinaryOctDecomposition` implemented) — so this is a configuration problem,
  not a rewrite.

## The experiment, if we pick this up

Enable `eBinaryOct` for FoF, gate exactness on the laptop matrix (1m at 16x1
and multi-PE-per-process, 8m-uniform, 16m), then one 2B/16-node arm against
the oct arm reporting: `placeholders` and `pool_MB` (the predicted win),
`phase3_walk` (the depth cost), and the **`positive` prune counter** — that is
the case-2 certificate count, and it answers the only question the arithmetic
cannot.

## Kale's hybrid, worth trying first

Use `eBinaryOct` for the CACHE (storage, placeholders, install granularity)
but make the opening decision **three levels at a time**, so certificates are
still evaluated on cubic boxes with an eight-way fan-out — which would keep
oct's pruning quality and effective traversal depth while paying binary's
much smaller frontier.

## The dilution cost is now MEASURED, not just argued (2026-08-24, relay110)

The premise above — that a pool diluted with full-size placeholders costs
real time — was an inference when this note was written. It has since passed
the discriminating test, on the GPU arm at 2B/128 nodes:

    cap U -> 128:  leaf_visits -0.067%   prunes_negative -0.15%
                   same_frag BYTE-IDENTICAL   edges_emitted identical
                   requests -0.8%   ->   phase3_walk -16.4%

**Same work, 16.4% less time.** Capping removes ~77 MB of pool per process
and 291M placeholder nodes across the job, and the walk does measurably the
same traversal in measurably less time. Every explanation resting on extra
fetches, extra misses or extra traversal is dead — requests are flat on BOTH
the GPU and CPU paths (-0.8%, -0.7%), so there is no extra miss traffic.

Still inference: that the residue is cache/TLB locality specifically. "Same
work, less time" is consistent with several memory-side stories and this
evidence cannot separate them. PAPI (TOT_INS flat as control, L3_TCM and
TLB_DM as signal) would name it; parked on a charm toolchain blocker.

WHY THE CPU SIGN DIFFERS, a reading consistent with the numbers and not yet
tested. Capping removes the same absolute number of canopy entries on both
paths (fills +347,600 CPU, +343,844 GPU), but the pack is a much larger
FRACTION of the GPU's cache, because the GPU walk fetches less to begin with
(487k edges against CPU's 1.24M, leaf 128 against 32):

    pack share of the uncapped pool:   GPU 51.8%   CPU 29.6%   (1.75x)

So both arms pay the same fill cost and both get dilution relief, but the
GPU's relief is 1.75x larger relative to its pool and outweighs the cost,
while the CPU's does not. Same two competing effects, different balance.

CONSEQUENCE FOR THIS DOCUMENT: the placeholder count is worth attacking, and
there are two independent levers that compose — eBinaryOct cuts the frontier
~7x (this note), and slim placeholders cut what remains by most of its bytes.
A placeholder is a full `FullNode`: fixed-size pool slot, and because it is
built with `n_particles = -1` the constructor gives it a full BRANCH_FACTOR
children array (8 atomic pointers, 64 B) that is initialised to null and
never written — `installSubtree` builds a NEW node and `swapIn` replaces the
placeholder wholesale. At ~248 B/slot that is ~26% of every placeholder, and
at 128 nodes uncapped roughly 60 GB of null pointers across the job.
