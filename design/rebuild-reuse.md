# Rebuild without recreation: reusing arrays and maps across iterations

**STATUS: LATER DESIGN OPTION — not scheduled, no code change intended now.**
Recorded 2026-07-24 (discussion with Kale) so the multi-iteration era
(Barnes-Hut, SPH) inherits the analysis instead of rediscovering it. fof3 is
single-iteration and unaffected; the interim QD-barrier fix for the map-race
(Driver.h, see the comments there) is sufficient for current work.

## Current behavior (traced 2026-07-24)

Per-iteration, Driver::run decides `complete_rebuild` (imbalance ratio or
flush period). The two paths:

- **complete_rebuild**: every Partition dumps its particles BACK TO THE
  READER GROUP on its PE (`Partition::rebuild(if_flush=true)` ->
  `readers[CkMyPe()].receive`), then `decompose(iter+1)` reruns the whole
  pipeline from the readers: findSplitters (distributed sample
  sort/histogramming), a FRESH Partition array + FRESH DecompArrayMap, a
  key-routed flush into partitions, a FRESH Subtree array + fresh map,
  another key-routed flush. Keys are recomputed per particle
  (adjustNewUniverse) since the universe box moved. The old arrays are
  destroy()ed. Cost: a dump round-trip, full splitter recomputation, two
  all-to-alls, and array/map/location-manager reconstruction — plus the
  post-init map-creation race this recreation exposes (bindTo + fresh
  setMap; see Driver.h comments and charm notes).
- **!complete_rebuild**: particles route DIRECTLY Partition -> Subtree via
  the EXISTING subtree decomposition's flush, same arrays, reset() only.
  Limitation: it reuses the STALE splitters, so imbalance accumulates until
  the ratio trigger fires the sledgehammer path above.

Why a fresh map each rebuild: mechanically, a new array with a new size
needs a map, and n_partitions/n_subtrees are data-dependent outputs of
findSplitters each time. The map content itself (DecompArrayMap:
pe_intervals — a prefix-sum walk of per-splitter particle counts against a
per-PE threshold) is a pure function of the current splitters: a SNAPSHOT.
Nothing about it requires a fresh GROUP — it is mechanical enough to be
UPDATED in place (broadcast the new pe_intervals) rather than recreated
(Kale's observation). Recreation is a design artifact of "arrays are
disposable per-rebuild objects," not a necessity.

## The proposal (the missing middle path)

**Recompute splitters, keep the arrays, update the map.**

1. **Long-lived map group, created once at init** (mainchare-constructor
   time — inside Charm++'s sequenced-installation regime, so it exists
   everywhere forever) with an `update(pe_intervals)` entry invoked each
   rebuild. Eliminates the bindTo+fresh-setMap race BY CONSTRUCTION (no
   post-init group creation at all) and removes the QD barriers.
2. **findSplitters over the Partitions, not the Readers**: the dump-back
   round trip exists only because the splitter code is written to read
   from the Reader group. Histogramming over the particles where they
   already live (Partitions) removes one full all-to-all per rebuild.
3. **Particles move within the SAME arrays under the NEW splitters**: the
   !complete_rebuild path already demonstrates direct Partition -> Subtree
   routing in-place; feed it freshly computed splitters instead of stale
   ones and the sledgehammer path disappears.
4. **The variable-count question**, three options by ambition:
   (a) pad to a fixed maximum count and tolerate empty elements — cheap
       (empty subtrees already exist in the system as EmptyLeafs do in
       trees); simplest and probably sufficient;
   (b) dynamic insertion/removal of the delta on the same array;
   (c) keep variable counts but recreate ONLY when the count changes
       materially — after the first few iterations of a real simulation it
       rarely would.

Also folds two decomposition ships into one: today each rebuild marshals
the Decomposition to every PE twice (treespec.receiveDecomposition AND
inside the fresh map's constructor); an updated map receives just the
interval table.

## What it buys

- Multi-iteration apps (Barnes-Hut/SPH, flush_period-driven rebuilds) stop
  paying reader-round-trip + reconstruction per rebuild.
- The post-init group-creation race class is eliminated rather than
  barriered around.
- LB state and location caches survive across rebuilds (today destroyed
  with the arrays).

## Open questions for the eventual design

- bindTo(partitions) for subtrees under matching decomps: binding is fixed
  at creation; with reused arrays it simply persists — but verify bound
  co-migration interacts correctly with in-place re-flushing.
- CollocateMap (non-matching decomps path) has the same snapshot->update
  conversion, keyed on partition_locations after LB.
- Whether findSplitters-over-Partitions changes the sample-sort quality
  (readers hold key-sorted slabs; partitions hold spatially-coherent sets —
  histogramming should be indifferent, but verify).
- Upstream note: independent of this refactor, CkCreateArray's bindTo path
  should declare a group dependency on a freshly set map (reported/to be
  reported against charmplusplus/charm).
