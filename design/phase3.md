# Phase 3 v1: cross-process boundary walk + gather-to-one UF_2

Context: `../fof_design_note.md` §2, §4, §10 step 2. After phase 1,
each process holds frozen tips (process-level fragments) in
`Particle::group_number`, and `upwardPass` has annotated every node's
`FragData` with `min_frag`/`max_frag`. Phase 3 discovers merge-graph
edges between fragments on *different* processes; UF_2 over those edges
plus the tips yields the global FoF.

## Scope of v1 (design-note sequencing step 2)

Correctness first, at small scale:

- **Case-1 pruning only** (`mindist(A,B) > b` -> prune). No positive
  certificate, no SEEN suppression, no parked-list state machine yet
  (those are step 3). Redundant edges are emitted and deduplicated at
  the gather point.
- **Plain point-to-point edge messages** (no htram/tramlib yet —
  decided 2026-07-18). Edges accumulate in per-PE buffers and are
  flushed to a collector at the end of the walk.
- **Gather-to-one UF_2**: all `(tip_g, tip_f)` edges land on PE 0,
  which runs serial UF over them; every tip not appearing stays its
  own root. Result broadcast as a tip -> globalRoot map; each PE
  relabels its own particles (same owner-writes pattern as phase 1
  relabel). Fine to ~1e6 edges; the distributed UF_2 replaces this in
  step 4.
- **Completion detection: CkStartQD** over the walk phase (all walk
  work is entry-method-driven with no external aggregation layer, so
  RTS-level QD is sound here; counter QD becomes necessary when
  htram enters).

## Walk structure

Driven from the FoFPhase1 group (it already owns the local subtree
registry): for each local subtree A and each *remote* subtree root key
B in the tree (discovered via the canopy / CacheManager `local_tps`
complement), start a dual walk (A, B) only if `mindist(box_A, box_B)
<= b`. Ownership rule: the side with the smaller subtree key walks the
pair (each cross-process pair walked exactly once, globally).

The walk itself descends the *remote* side through the CacheManager
(request nodes on miss, resume on arrival — the existing
Traverser/Resumer machinery pattern), while the local side descends
real pointers. Leaf x leaf: for particle pairs within b, emit edge
(local group_number, remote group_number). Remote leaves carry
particles with group_number already set (subtree-side copies were
relabeled in phase 1 before upwardPass; the cache ships those copies).

Keep the walk helpers offset-parameterizable (PBC later; see
phase1.md).

## Test (examples/fof3 or extension of fof1)

- `./charmrun ++local ./FoF3 -f inputs/1k.tipsy -d oct +p2 ++ppn 1`
  (2 processes x 1 PE) and `+p4 ++ppn 2` (2 processes x 2 PEs,
  exercising phase-1 phaseB and phase 3 together). Also keep +p1/+p2
  single-process runs passing (phase 3 finds no remote pairs there,
  or trivially completes).
- End-to-end check: gather (order, position, group_number) after UF_2
  relabel; compare partition against the serial O(n^2) reference
  (canonical min-order representatives) — the same harness as fof1,
  now valid multi-process because the *final* labels are global.
- Leaf-annotation visitor assertions stay on (first true multi-process
  exercise of upwardPass canopy propagation + FragData cache shipping).
- Report edge counts (emitted vs deduplicated) — first data point for
  design-note §8.2.

## Deliberately deferred

Positive certificates, SEEN table + per-pair state machine (step 3);
htram aggregation + counter QD (with htram); distributed UF_2
(step 4); giant-fragment splitting; PBC images.
