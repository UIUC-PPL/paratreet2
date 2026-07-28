# Single-distribution mode: making Partitions optional

**STATUS: DESIGN DIRECTION (Kale, 2026-07-28) — decision Kale's, with
discussion with Ritvik. Decision context: Kale is certain of the direction as
long as both options exist, and skeptical that the partition degree of
freedom ever bought much even for load balancing.**

## The situation being fixed

Old ParaTreeT imposed DUAL particle distributions on every application:
Subtrees (tree ownership) and Partitions (traversal work), each its own
chare array with its own decomposition. paratreet2 inherited the
structure. The storage half of the objection is already solved: with
matching decompositions (the FoF configuration and the default
expectation), each particle is stored ONCE — Partition leaves are the
Subtree leaf nodes by POINTER IDENTITY (`verifySharedLeaves` enforces
`leaves[i] == tree_leaves[i]`), and the 2B run confirms zero
duplication at scale (`numPSParticleCopies = 0; numPSParticleShares =
0` at 1.98B particles).

What "identical distributions" still costs:

1. **A second chare array + a second decomposition pass.** At 2B:
   67,992 Partitions created (1.8 s), a partition assignment pass
   (0.55 s), splitters computed for both. Startup work for an array
   that, on FoF's default path, is nearly vestigial — the dual walk
   runs from `subtrees.startDual`; Partitions serve only the
   transposed oracle walk, the FragCheck harness, and LB hooks.
2. **Dual code paths in the core.** The non-matching case
   (`requestCopy`/`receiveSubtree`/`refreshCopies`, the copy/share
   machinery) coexists with aliasing. This duality produced a real bug
   (post-build mutations invisible to shipped copies, 2026-07-21) and
   complicates every cache-lifecycle change (the cached-particle work
   had to guard exactly these paths). Apps that never use the second
   path still pay its complexity.
3. **API surface**: every application receives both proxies
   (ProxyPack) and must know which drives what.

## What Partitions are genuinely for — and the open doubt

The design intent: traversal work distributed independently of tree
ownership, so the load balancer can migrate walk work (Partitions)
without moving the tree (Subtrees). That freedom only matters if
(a) walk load and ownership load diverge enough, and (b) partition
migration actually recovers the difference. NEITHER HAS BEEN
DEMONSTRATED in this codebase: fof3 runs LB-free; no measurement in
the project record shows partition-granularity LB beating
subtree-granularity LB. Kale's assessment: unclear it ever bought
much. An honest experiment (gravity-class app, clustered data,
partition-LB vs subtree-LB) belongs on the list, but the mode below
does not wait for it.

## The mode

An application-level choice (Configuration flag or app hook):
`single_distribution = true` means NO Partition array is created.

- Traversals run from Subtrees (`startDual` already does; a
  subtree-driven `startDown` covers transposed-style walks).
- Decomposition computes ONE set of splitters; the partition
  assignment pass disappears.
- LB hooks (AtSync/UserSetLBLoad) move to Subtree — coarser
  granularity, which is exactly the experiment above.
- The copy/share machinery (`requestCopy`/`receiveSubtree`/
  `refreshCopies`) is unreachable in this mode; with all current apps
  opting in, it can eventually be quarantined behind the dual mode.
- fof3 consumers of Partition today and their disposition:
  `verifySharedLeaves` (vacuous — nothing to verify), the transposed
  oracle walk (gate behind dual mode, or port to subtree-driven
  startDown), FragCheckVisitor harness (same), prefetch/pause
  machinery (audit; walk-pause is Partition-resident today).

Both modes remain supported: dual distribution stays for applications
that want the LB freedom (or until the experiment settles the doubt);
single distribution becomes the default recommendation for apps whose
walk distribution equals ownership — FoF first.

## Expected wins

At 2B: ~2.4 s of startup (partition creation + assignment), one fewer
array to create/register/balance, and — the larger prize — retiring
the dual-path complexity from the core's maintenance surface for apps
that opt in. Memory: none (aliasing already avoids duplication);
this is a complexity-and-startup change, not a footprint change.

## Rough plan

1. Config flag + ProxyPack shape (partition proxy absent/null in
   single mode); Driver skips partition creation and assignment.
2. Subtree-driven transposed walk (port of `Partition::startDown`'s
   driver loop) so the oracle survives in both modes.
3. LB hooks on Subtree; the partition-vs-subtree LB experiment when a
   gravity-class app is exercised.
4. fof3 flips to single mode; full validation suite (the outputs must
   be byte-identical — the walk reads the same aliased leaves either
   way).
