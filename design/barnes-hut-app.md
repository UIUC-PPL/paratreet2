# Barnes-Hut gravity: the second application (monopole-only)

**STATUS: IMPLEMENTED (2026-08-04, examples/gravity). Kale's review
decisions: name `gravity`; old defaults kept (theta 0.7,
nMinParticleNode 6); FIXED timestep (-T, default 0.01) instead of the old
adaptive rule. Decisions from Kale: monopole only; UIUC-PPL/barnes is
background reading, NOT an oracle (unvalidated); the exact reference is
direct summation. Port source: old paratreet's gravity example
(examples/GravityVisitor.h, Gravity.C, CentroidData.h,
MultipoleMoments.h) — the monopole path is exactly its `#ifdef BARNESHUT`
configuration.**

## Implementation deltas vs the plan below (what running it taught)

1. **"theta -> 0 exactness" was structurally impossible as designed**: the
   acceptance criterion is scale-relative, so for ANY positive theta some
   deep small node is accepted against a distant target — no positive
   theta degenerates the walk. Measured confirmation: the error follows
   the theta^2 monopole law all the way down (1k: rms 4.3e-3 at theta
   0.7 vs 6.4e-7 at 0.005; ratio ~ (0.005/0.7)^2). The exactness gate is
   instead an explicit always-open mode, `-o 0`.
2. **This build's Real is float** (USE_DOUBLE_FP unset), so even the
   always-open walk differs from a same-precision reference by
   accumulation order. The harness computes the reference in DOUBLE (a
   true oracle; its antisymmetry residual is ~1e-16) and gates exact mode
   at the float-accumulation band (max < 1e-4, rms < 1e-5; measured
   max 4.3e-6 / 7.0e-6, rms 3.4e-7 / 1.1e-6 at 1k / 10k) — still ~100x
   below any structural error (one missed or duplicated 12-particle leaf
   shifts a neighbor by ~1e-2). A -DUSE_DOUBLE_FP stack build would allow
   a true roundoff gate; recorded as a possible later arm.
3. **The framework's kick/perturb were STUBS** — the per-particle calls
   were commented out in the inherited code (old paratreet too), so
   perturb_particles apps computed forces but never moved anything (the
   "Perturbations" cost at 2B was the copy/box machinery only). Restored
   as part of this work: Particle::kick/perturb (KDK leapfrog; the SPH
   internal-energy lines stay out with the commented-out u field),
   SpatialNode::kick, live loops in Partition::kick/perturb. Gated by
   config.perturb_particles as before; full FoF/annotate/searchAlgos
   regression unchanged (fof3 opts out; the others are single-iteration).
4. Verified in vivo: 5-iteration 1k run shows the Plummer sphere
   contracting (universe box shrinks each step, maxVelocity evolves,
   timestep fixed at 0.01).

## Measured baselines (2026-08-04, laptop, float build)

- 1k theta 0.7: rms_rel 4.325e-3, max_rel 7.031e-2 (identical +p1/+p2
  and 2-proc — same rms to the printed digits).
- 10k theta 0.7: rms_rel 3.770e-3, max_rel 3.754e-2.
- Exact mode (-o 0): see band above. Reference momentum residual ~1e-16
  (double); tree-sum third-law violation ~2e-4 relative (reported, not
  gated).
- make test = 7 runs (theta 0.7 + exact mode, single/multi-process, 1k +
  10k, plus a 5-iteration -i 5 smoke), all must print GRAVITY TEST
  PASSED (the -i 5 run prints the iteration-0 check).

## Why now (role in the structural program)

1. First test that the fof extraction boundary is real from the app side: a
   second application whose Makefile links `-lparatreet` alone — no
   unionfind, no fof/, no FoF headers.
2. First MULTI-ITERATION application: exercises kick/perturb/rebuild —
   paths FoF (single-iteration, perturb-skip) never touches, and exactly
   the paths single-distribution mode reshapes next.
3. The gravity-class vehicle the single-distribution design note requires
   for the partition-LB-vs-subtree-LB experiment.

## Scope

IN: monopole (center-of-mass + total mass) node acceptance; softened
direct summation at leaves (cubic-spline SPLINE kernel, ported verbatim);
the standard opening criterion; transposed downward walk; multi-iteration
leapfrog via the framework's existing kick/perturb/getTimestep flow;
direct-sum verification harness.

OUT (deferred, in rough order of later interest): quadrupole/hexadecapole
moments (`node()` swaps in momEval — the visitor seam is unchanged),
periodic boundaries/Ewald, prefetch (`startParentPrefetch`, commented out
even in old paratreet), dual-tree gravity A/B (dual ignores CallSelfLeaf —
known Traverser caveat — so it needs its own look), potential output
(monopole node path computes acceleration only), load balancing (that IS
the later experiment).

## Files: `examples/gravity/` (binary `Gravity`)

Plain example like annotate — no module, no .ci chares of its own beyond
the mainmodule. Makefile mirrors annotate's (links `-lparatreet` only).

- `GravityData.h` — the Data type (below).
- `GravityVisitor.h` — the visitor (below).
- `Main.C / Main.h / Main.ci` — annotate-pattern mainmodule with
  `extern entry void Partition<GravityData> startDown<GravityVisitor>`.
- `DirectSum.h` (or in Main.C) — the verification harness.

## GravityData (monopole CentroidData, slimmed)

Old CentroidData = MultipoleMoments + box + SPH baggage (pps neighbor
lists, balls — Collision/SPH only). The monopole app needs:

```
struct GravityData {
  OrientedBox<Real> box;     // framework contract (Subtree LB load reads it)
  Vector3D<Real> moment;     // running sum of m_i * x_i
  Real totalMass = 0;
  long count = 0;
  Real radius = 0;           // opening radius, from cm and box
  // cm() = moment / totalMass
};
```

- Leaf ctor: accumulate moment/mass/box over particles;
  radius = calculateRadiusBox (half box diagonal) — old code's leaf rule.
- `operator+=`: grow box, add moment/mass/count;
  radius = calculateRadiusFarthestCorner (distance from cm to farthest box
  corner) — old code's internal-node rule. Both functions port as ~6-line
  statics on GravityData (no HEXADECAPOLE rescale arm).
- count<=1 nodes: radius = 1.0 (old convention, keeps the acceptance test
  harmless for singletons).
- No `soft` member: the non-HEX `open()` never reads it (openSoftening is
  HEXADECAPOLE-only) and `leaf()` uses per-particle softening.
- Computed entirely at BUILD time (leaf ctor + upward `+=`), like old
  paratreet; no post-build upwardPass needed. Rebuild each iteration
  refreshes it after perturb.
- peanoKey readonly defined in Main.ci (framework contract).

## GravityVisitor (the BARNESHUT monopole path, ported)

Traits: `CallSelfLeaf = true` (a bucket must interact with its own leaf;
transposed walk honors it). Keep `TargetMustBeLeaf`/`ForceEvenDepth`
declared for a future dual arm but the shipped walk is transposed.

- `open(source, target)`: port verbatim non-HEX logic —
  `source.count <= nMinParticleNode(=6)` opens; else open iff
  `Space::intersect(target.box, cm, radius^2 * (4/3)/theta^2)`
  (opening_geometry_factor_squared = 4/3, gravity_factor precomputed from
  theta in the ctor). Space.h is already in utility/structures.
- `node(source, target)`: monopole acceleration — for each target
  particle, `a += (cm - x) * M / |cm - x|^3`, via applyAcceleration
  (kept in paratreet2's SpatialNode). No softening here (acceptance
  guarantees separation) — matches old BARNESHUT exactly.
- `leaf(source, target)`: full pairwise with SPLINE softening (ported
  verbatim; twoh = soft_i + soft_j), skip r=0 self-pairs.
- `cell()`: `!Space::enclose(source.box, target.box)` (dual only; inert
  in the transposed walk).
- No offset member (periodic-replica machinery dropped with Ewald).
- G = 1 convention (old code's "note gconst = 1").

## Driver flow (Main)

Standard `paratreet::Main<GravityData>`: preTraversalFn = loadCache;
traversalFn = `partition.startDown<GravityVisitor>(GravityVisitor(theta))`;
getTimestep = old rule (universe_length / max_velocity / cbrt(N), capped
by -T max_timestep). perturb_particles stays TRUE (default) — this app
WANTS the kick/perturb/rebuild cycle. Flags: `-o <theta>` (default 0.7),
`-i <iterations>` (framework), `-c <mode>` (verification, below),
`-T <max_timestep>`.
(Flag letters checked against the framework getopt set at implementation
time; use release_arg if one collides, as fof3 did.)

## Verification (the part that gates)

1. **Direct-sum reference (-c full, default on <=10k inputs)**: gather all
   particles (Writer-style collect at iteration 0, before any kick),
   compute O(n^2) SPLINE-softened accelerations serially, compare per
   particle: report max and rms of |a_tree - a_direct| / |a_direct|.
   GATE: rms below the standard monopole bound for theta=0.7 (~1e-2
   territory); exact threshold recorded from first runs and then frozen as
   the regression value, FoF-style.
2. **theta -> 0 exactness**: at theta small enough that every node opens
   (walk degenerates to all-leaf direct sum with the SAME SPLINE kernel),
   tree accelerations must equal the reference to roundoff (tight
   tolerance, e.g. 1e-12 relative). This checks the walk/cache/visitor
   plumbing EXACTLY, independent of multipole error — the analogue of
   FoF's bit-identical gate, adapted to floating point.
3. **Multi-configuration invariance**: +p1 / +p2 / 2-proc x 2PE at fixed
   theta agree within accumulation-order tolerance (float sums reorder
   across configs; NOT bit-identical by construction — tolerance ~1e-12
   relative, recorded).
4. **Momentum sanity**: |sum m_i a_i| / sum m_i |a_i| small for the
   reference (exact antisymmetry) and reported for the tree sum.
5. **Multi-iteration smoke**: 5 leapfrog iterations on 1k, no aborts,
   center-of-mass drift consistent with initial momentum.
6. UIUC-PPL/barnes: background reading only (Kale 2026-08-04: written
   long ago as an example, never validated for accuracy — NOT an oracle).

## Open questions for review

1. Directory/binary name: `examples/gravity` / `Gravity` (matches old
   paratreet) vs `examples/barnesHut`. Proposal: gravity.
2. nMinParticleNode = 6 and theta default 0.7 carried from old code —
   keep both as flags-with-old-defaults?
3. Timestep rule ported from old Gravity.C — acceptable for the smoke
   test, or want fixed -dt for determinism in regressions?
