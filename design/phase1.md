# Phase 1: intra-process FoF (UF_1)

Context: `../fof_design_note.md` §2–3. Phase 1 computes, per process, the
connected components of the linking graph restricted to the process's own
particles ("process-level tips"). Tips freeze at the end of phase 1; the
distributed union-find (UF_2) later runs over tips, never particles.

## Decisions (agreed 2026-07-18)

- **Oct decomposition** (`-d oct`) is the FoF configuration: subtree
  boundaries on oct-node boundaries make every subtree a cube, which keeps
  the Minkowski-inflated box test tight in phase 3. SFC keys still drive
  the sort underneath.
- **No merged per-process tree.** Phase 1 walks the existing per-subtree
  local trees pairwise via real pointers. The CacheManager's `local_tps`
  registry (and the canopy replicas above it) already provide the only
  process-level structure needed. A merged tree is a measured fallback,
  not a starting point.
- **SMP parallelism without atomics**, by recursive application of the
  frozen-tip idea (no mutable parent pointers are ever read or written
  concurrently):

  1. **(a) Per-PE UF.** Each PE runs serial UF (path compression + union
     by min-root) over the particles of the subtrees resident on that PE.
     Neighbor discovery: dual walks over all pairs of that PE's subtrees
     (including self-pairs), pruning on `mindist(box_A, box_B) > b`;
     leaf-leaf does pairwise distance checks and unions. Walks that would
     cross to another PE or process prune (deferred to (b) / phase 3).
  2. **Freeze + compress.** Full path compression; every particle stores
     its PE-tip id in `Particle::group_number`. Tip id = global particle
     id (`order`) of the component's min-order root — one namespace for
     PE-tips, process-tips, and later UF_2 vertices.
  3. **Barrier** (reduction), then **(b) cross-PE edge emission.** For
     each subtree pair spanning two PEs of the same process, the
     lower-PE-id side walks the pair and emits `(tip_i, tip_j)` edges
     into its own PE-local buffer (per-PE SEEN set dedups per tip pair).
     Reads only frozen data; writes only own buffer. No atomics.
  4. **Barrier, then serial merge.** One PE per process runs a small UF
     over the collected edge lists (touches only tips appearing in
     boundary edges) producing the PE-tip -> process-tip map.
  5. **Parallel relabel.** Each PE rewrites its own particles'
     `group_number` through the map. Owners write; no contention.

  The apply step is a UF, not a scatter of pointer writes: noted merges
  chain (a->b on one PE, a->c on another implies b~c), which is why step
  4 is a real union-find and why it is serialized (it is tiny) rather
  than applied concurrently by owners.

- v1 uses **global reductions** between sub-phases (correctness first).
  The design note's "no global barrier" (§2.1) concerns the phase 1 -> 3
  transition at scale; revisit after §8 measurements.
- Periodic boundary images are **out of scope for v1**.
- Linking length `b` is passed at the API call, not baked into
  Configuration; tests derive it from the universe box
  (`b = 0.2 * (V/N)^(1/3)`) or take a flag.

## Validation

On a single process (+p1 and +p2 SMP), the process owns all particles,
so phase 1 alone computes the complete FoF: `group_number` must induce
exactly the same partition as a serial O(n^2) reference FoF over the
gathered particles (canonicalized by min particle order per component).
Multi-process runs produce partial tips by design; they are validated
later, in the phase-3 test.

## After phase 1

`upwardPass` with an FoF `Data` type annotates `min_frag`/`max_frag`
per node from `group_number`. Ordering constraint (already documented):
phase 1 + upwardPass must complete before the traversal's cache loading.
