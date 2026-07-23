# Dual-tree phase-3 walk (`fof3 -w dual`)

Status: prototype on branch `dual-tree`, laptop-validated 2026-07-23.
Motivation: design/step3.md §6f — the transposed walk is source-driven
against a FLAT list of local target leaves, so far target regions are pruned
per-leaf, never per-subtree; and the walk's observed superlinearity in local
N (§6b, §6h) made it the dominant low-P cost. Kale's directive: prototype on
a branch, measure on the laptop, keep the single-tree traversal INTACT for
other applications (Barnes-Hut etc.) — which this does: `startDown`/
`TransposedDownTraverser` are untouched; the dual walk reuses the framework's
existing, previously-unused `DualTraverser` + `Subtree::startDual`, opted
into by fof3 only.

## Mechanism

`-w dual` (with `-u dist`; default remains `-w transposed`) launches phase 3
as `subtrees.startDual<FoFEdgeVisitor>` instead of
`partitions.startDown<FoFEdgeVisitor>`. DualTraverser walks (source node,
target node) PAIRS: the global source tree against each subtree's live local
tree — whose internal nodes upwardPass annotated in place, so internal x
internal pruning becomes available. Same visitor, same SEEN table, same edge
emission; the edge set and final labels are identical by construction and
verified below.

Visitor additions (FoFPhase3.h, FoF-only — no framework interface change):
- `TargetMustBeLeaf = true`: leaf() runs the two-sided particle loop, so a
  leaf source vs an internal target descends the target via
  runInvertedTraversal; leaf() only ever sees leaf x leaf.
- `ForceEvenDepth = true`: source-only splits until depths equalize, then
  joint splits — compared boxes stay size-matched.
- `cell(source, target)`: consulted by DualTraverser for internal x internal
  pairs. PURE PREDICATE mirroring open()'s verdict (same-tip / SEEN /
  positive-certificate / mindist tests) with NO counters and NO side
  effects; a pair cell() rejects flows into runInvertedTraversal, whose
  first doOpen() re-tests it once WITH counters and terminates. Verdicts can
  diverge only if another PE inserts SEEN between the calls — which turns a
  descend into a prune, the safe direction (SEEN only grows during a walk).
  Stats consequence: internal x internal joint descents bypass open(), so
  both_uniform_descents/redundancy UNDERCOUNT under -w dual (observed: 76 vs
  17,990 at 1M — the co-descent also genuinely resolves pairs at internal
  level almost immediately). Prune counters are unaffected.

Two supporting fixes that also stand alone:
1. **Leaf-level box gate in leaf()** (both walks): neither traverser applies
   open() to a leaf x leaf pair — transposed's last test used the source
   PARENT's looser box; dual's used equal-depth internal boxes ~2x leaf
   size. Without the gate the dual walk's leaf visits blew up 4x (20.4M vs
   5.3M at 1M) and erased its win; with it, 1M leaf visits drop to ~0.9M in
   BOTH walks. Gated pairs count as negative prunes, so leaf_visits still
   means "leaf pairs actually processed".
2. **`use_subtree` reset in Partition::startDown** (framework, one line):
   Subtree::startDual sets the per-PE Resumer to route cache resumes to the
   subtree proxy; Partition::startDown never reset it, so any later
   partition-driven walk on that PE (the FragCheckVisitor pass) would
   misroute. Latent until fof3 -w dual became the first app to mix both walk
   types in one run.

## Results (laptop, 2026-07-23, classic Converse, htram-on build)

Identical outputs everywhere: 100/1k/10k grid-verified single- and
multi-process (72/390/3549); 1M b0.2 -> 333,889 and b0.8 -> 41,315
grid-verified; 8M/16M stats-mode component count + max_size + full histogram
bit-identical to the transposed walk (2,657,656/2,055,507 and
5,317,213/4,094,096).

Walk time (seconds), transposed vs dual, leaf gate active in both:

| config               | transposed | dual  | ratio |
|----------------------|-----------|-------|-------|
| 1M  b0.2, 4proc x 2PE | 1.035     | 1.177 | 0.88x (dual slower) |
| 1M  b0.8, 4proc x 2PE | 0.456     | 0.431 | 1.06x |
| 8M  b0.2, +p2 (1proc) | 43.53     | 3.07  | **14.2x** |
| 8M  b0.2, 4proc x 2PE | 4.59      | 2.84  | 1.6x  |
| 16M b0.2, +p2 (1proc) | 159.15    | 6.36  | **25.0x** |

Dual is LINEAR in N at fixed cores (8M -> 16M: 3.07 -> 6.36); transposed is
superlinear (43.5 -> 159). At small N-per-PE the two are comparable (dual
~13% slower at 1M b0.2 — the 64-way joint-split fanout near threshold b is
slightly less selective than per-leaf tests; it flips to a dual win by b0.8).

## The superlinearity root cause (solved)

The 8M +p2 A/B is diagnostic: dual performs MORE box tests than transposed
(199M vs 152M negative prunes) yet runs 14x faster. So the transposed cost
was never the geometry tests — it is the per-opened-node sweep over the
ENTIRE flat target-leaf list: recurse() allocates and scans an
active_buckets vector sized |leaves| for EVERY opened source node, active or
not. Cost ~ opened_nodes x TOTAL local leaves — quadratic-ish in local N,
which is exactly the §6b/§6h superlinearity, and why it self-heals as P
grows (leaves/partition shrink). Anvil's 80M P=1 walk of 84s (vs 1.1s at
P=16) is this effect at full size.

## Caveats / not done

- DualTraverser has NO pause/parallel-help support (Transposed's
  wantsPause/WorkMonitor machinery): the shared-memory tail-imbalance help
  is unavailable under -w dual. Not needed for the laptop measurement; check
  at scale.
- `CallSelfLeaf` is not honored by DualTraverser (self leaf pairs run
  leaf(); harmless for FoF — same-tip skip — but a semantic gap if another
  visitor adopts dual).
- -u serial + -w dual is rejected (CkAbort); the dual walk is wired to the
  dist path only.
- Reconverse/Anvil untested (same gate as htram).
- Default stays `-w transposed` pending cluster validation; flip after an
  Anvil A/B (expect the P=1..4 rows of the §6h sweep to compress toward the
  P=16 row).
