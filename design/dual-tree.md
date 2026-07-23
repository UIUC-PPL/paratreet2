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

## v2: alternating split + closest-first ordering (same day, after review)

Kale's review raised two points against the v1 64-way joint split: quantify
its selectivity loss, and ask whether child pairs are explored
closest-boxes-first (they were not — plain LIFO, Morton order; the design
note §6.2 priority idea was never built).

Quantified (1M b0.2, from the pre/post-leaf-gate counter deltas): v1 dual
made 2.4x FEWER internal-level tests than transposed (5.76M vs 13.6M) but
its leaf frontier was only 4.9% selective vs transposed's 16% — 20.4M leaf
pairs generated, 19.4M chaff — because the last test compares equal-depth
boxes ~2x leaf size on both sides. Net +40% total tests.

Fix (Traverser.h, opt-in trait `SplitLargerOnly`, same idiom as
maybeSetKeys — trait-less visitors keep the old behavior bit-for-bit):
split ONLY the shallower (larger-box) side per step (8-way alternating, tie
-> source), inserting a pruning test at every single-level refinement; and
push the 8 children farthest-first so the LIFO stack explores the
closest-box pair FIRST (dualBoxMinDist2 on the generic Data::box) — the
local form of §6.2's witness-first priority, so SEEN suppression covers the
rest of the expansion. Effect at 1M b0.2: total tests 25.2M -> 7.9M (2.3x
FEWER than transposed now), suppression prunes 209k -> 53k, same_frag 1.0M
-> 395k (witnesses land before siblings are even generated).

## Results (laptop, 2026-07-23, classic Converse, htram-on build)

Identical outputs everywhere: 100/1k/10k grid-verified single- and
multi-process (72/390/3549); 1M b0.2 -> 333,889 and b0.8 -> 41,315
grid-verified; 8M/16M stats-mode component count + max_size + full histogram
bit-identical to the transposed walk (2,657,656/2,055,507 and
5,317,213/4,094,096).

Walk time (seconds), transposed vs dual v1 (64-way joint split) vs dual v2
(alternating split + closest-first), leaf gate active in all:

| config               | transposed | dual v1 | dual v2 | v2 speedup |
|----------------------|-----------|---------|---------|------------|
| 1M  b0.2, 4proc x 2PE | 1.035     | 1.177   | 1.092   | 0.95x (par) |
| 1M  b0.8, 4proc x 2PE | 0.456     | 0.431   | 0.481   | ~par (noise) |
| 8M  b0.2, +p2 (1proc) | 43.53     | 3.07    | 2.11    | **20.7x** |
| 8M  b0.2, 4proc x 2PE | 4.59      | 2.84    | 2.58    | 1.8x  |
| 16M b0.2, +p2 (1proc) | 159.15    | 6.36    | 4.26    | **37.4x** |

Dual is LINEAR in N at fixed cores (v2: 2.11 -> 4.26 for 8M -> 16M);
transposed is superlinear (43.5 -> 159). At 1M-per-8PE dual v2 is at parity
despite doing 2.3x fewer tests — transposed's flat bucket sweep is very
cache-friendly at small leaves-per-partition, while the dual stack pays
push/pop + sort overhead per node; the gap closes as local N grows and
transposed's sweep degrades.

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
- ~~Default stays `-w transposed` pending cluster validation~~ **FLIPPED to
  `-w dual` after the Anvil A/B below.** Transposed stays permanently as the
  A/B oracle: two independent walk implementations producing bit-identical
  counts is a standing correctness check.

## Anvil 80M A/B results (2026-07-23, Ritvik; the flip decision)

80M LAMBS (lambb.00500), 15 PEs/proc, b_factor 0.2, htram-on, reconverse.
Identical outputs between walks at every P (Ritvik-confirmed; fragments
lines bit-identical). walk_s:

| P  | transposed | dual  | speedup |
|----|-----------|-------|---------|
| 1  | 80.03     | 4.00  | 20.0x   |
| 2  | 28.83     | 1.90  | 15.2x   |
| 4  | 7.45      | 0.975 | 7.6x    |
| 8  | 2.51      | 0.855 | 2.9x    |
| 16 | 1.00      | 0.635 | 1.6x    |

Dual wins at EVERY P — better than the laptop extrapolation (which only
promised compression of the low-P rows). Also: the redundancy
concentration collapses under dual (max_per_pair 32-37 vs transposed's
1621-5650 spikes) — closest-first exploration resolves hot pairs
immediately; 3b is buried for good. The reconverse gate PASSED: ten
multi-node runs through the tram quiesce loop and the dual resume path,
no hangs. htram-on uf2 at P=16 measured 0.44-0.55s vs 0.14s htram-off
(the 10us flush machinery is a small net cost at 33k edges — harmless,
kept as insurance for multi-billion-particle scale).

Post-flip picture at P=16: walk 0.64s; phase1 + tip_encode + upwardPass
~8.4s = ~90% of algorithmic time — phase 1 is the scaling frontier
(design/phase1-scaling.md; the suppression layer on main targets it).

## Anvil test instructions (branch `dual-tree`)

This branch = main (which already includes htram-aggregation-ON, step4.md
4a) + the dual walk. Build:

1. `unionfind`: rebuild aggregation-ON — `make clean && make PROFILE=`
   (AGGREGATION defaults on there). htram must be built first (unchanged).
2. `paratreet2`: `git fetch && git checkout dual-tree`; then
   `make clean && make` in src/ and examples/fof3 (CHARM_HOME defaults to
   $HOME/charm_reconverse). -DAGGREGATION now defaults ON in paratreet2 and
   MUST match the unionfind build (ABI); htram-off requires
   `make AGGREGATION=` in BOTH trees plus cleans.
3. Reconverse gate FIRST (two untested combinations: tram+reconverse QD,
   dual+reconverse resume path): a small multi-node run, e.g.
   `FoF3 -f 100k.tipsy -d oct -u dist -w dual` on 2+ nodes, then the same
   with `-w transposed`; counts must match each other and the known 100k
   value, the banner must say "Compiled with aggregation optimizations",
   and the FOF3STAT config line shows `walk dual`/`walk transposed`.
4. The A/B that decides the default: the §6h sweep (80M LAMBS, 15 PEs/proc,
   P = 1 2 4 8 16, b_factor 0.2) run twice — add `-w dual` / `-w transposed`
   to the LAUNCH line of examples/fof3/redundancy_sweep.sh (the script now
   echoes every FOF3STAT block, so phase times, balance lines, and
   histograms all land in the table output this time). Compare walk_s
   between walks per P, and component counts across ALL runs (stats-mode
   determinism lines must be identical). Expected: transposed P=1 ~84s
   compresses to single digits under dual; P=16 roughly unchanged. The uf2
   column doubles as the htram-on-vs-off comparison against the 2026-07-23
   logs (expect parity at these edge counts).
5. Watch: FOF3STAT memory_MB (dual's pair stack), any hang in the walk or
   uf2 phase (reconverse QD is the temperamental partner), and per-process
   balance lines (dual has no parallel-help/pause machinery yet).
