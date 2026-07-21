# Periodic boundary conditions (PBC)

Context: `../fof_design_note.md`, `phase1.md` (PBC deferred-without-prejudice
note), `phase3.md`. Cosmological FoF runs in a periodic box: a particle near
one face can link to one near the opposite face by wraparound. Old paratreet's
FoF has this (`-pbc 1`, `-px/-py/-pz`, a 27-image walk); paratreet2 did not.
This adds it and validates against old FoF.

## Approach: minimum-image distance (single walk), not 27 explicit images

Old FoF does 27 traversals (one per offset in {-L,0,L}^3). We instead make the
distance functions periodic (minimum-image) and keep ONE walk / ONE traversal.
This is **equivalent** for FoF because the linking length always satisfies
`b < L/2` (b ~ 0.2 mean-spacing, L = box), so each particle has at most one
image of any other within b — the nearest. Minimum-image is cleaner (no 27x
overhead, no 27 startDown runs in phase 3) and gives identical components.
Origin-agnostic: `dx -= L*round(dx/L)` wraps to [-L/2, L/2] for any box origin
(our LAMBS box is centered [-0.5,0.5]).

### Periodic helpers (add near mindist2/maxdist2 in FoFPhase1.h)

- `periodicDistSq(a, b, period)`: sum over axes of `wrap(a-b)^2`, where
  `wrap(d) = d - period*round(d/period)` when `period > 0`, else `d`.
  Replaces `(posA - posB).lengthSquared()` in every leaf distance test.
- `mindist2(a_box, b_box, period)`: per-axis periodic interval gap. For each
  axis, the gap between intervals [a.lo,a.hi] and [b.lo,b.hi] under the period
  (consider b shifted by 0, +L, -L; take the min non-negative gap). Combine as
  sum of gap^2. With `period == 0` per axis, reduces to the existing
  open-boundary gap. Keep the existing 2-arg `mindist2` as `mindist2(a,b)` =
  `mindist2(a,b,{0,0,0})`.

`maxdist2` (case-2 positive certificate) is NOT made periodic: the "farthest
image" is ill-defined, and case 2 never fires in practice (design/step3.md
6a/6c — 0 across all data incl. through percolation). Under PBC, **skip case 2
entirely** (open() falls straight to the periodic case-1 mindist2 + descend).
No correctness loss; suppression + leaf witnesses do all the work.

## The period parameter

- App flag `-P <L>`: cubic period (all axes = L). Default 0 = open boundaries
  (exact current behavior; the periodic branch is a no-op). Per-axis is a
  trivial later extension (store a Vector3D<Real>); cosmological boxes are
  cubic so one L suffices. Check for a framework-field letter collision and
  `conf.release_arg("P")` if needed (as -b/-c/-u/-m did).
- Thread it in: `FoFPhase1` group gains `Vector3D<Real> period_` (set at
  registration or via a param on the phase-1 driver); `FoFEdgeVisitor` gains a
  `Vector3D<Real> period` member (pupped). fof3 reads `-P`, passes L into
  `runFoFPhase1(...)` (phase 1) and constructs the visitor with the period
  (phase 3).
- b is unchanged (0.2*(V/N)^(1/3)); for the comparison we feed the SAME numeric
  b to old FoF via `-ll`, as in the earlier PBC investigation.

## Integration points

Phase 1 (`FoFPhase1.h`):
- `walk(a, b, leaf_fn)`: prune on `mindist2(a->data.box, b->data.box, period_) > b2_`.
- `leafLeafUnion` / the phaseB edge leaf: distance test via
  `periodicDistSq(posA, posB, period_) <= b2_`.
- Both phaseA (intra-PE unions) and phaseB (cross-PE edges) walks. Single
  process owns the whole box, so phase 1 alone must apply PBC for the
  single-process result to be correct (it's the full FoF there).

Phase 3 (`FoFPhase3.h`, `FoFEdgeVisitor`):
- `open()`: case-1 uses `mindist2(source.box, target.box, period)`; skip case 2
  when any period axis > 0.
- `leaf()`: particle distance via `periodicDistSq(..., period)`.
- SEEN/same-frag logic unchanged. The framework Traverser still drives one
  traversal; the visitor just uses periodic distance.

## Validation (against old paratreet FoF)

Old FoF: `-pbc 1 -px L -py L -pz L -ll <b> -c 0`. paratreet2: `-P L`, same b.

PBC only matters where structure reaches the box faces, i.e. near/above
percolation (design/step3.md 6c: at b_factor 0.2 PBC changed LAMBS by 0.12%).
So validate where PBC is actually exercised (PBC-on != PBC-off):
- **Uniform box** (inputgen `uniform`, L=1.0) at b_factor ~0.5-0.8 (near
  percolation, structure straddles faces): the strongest PBC test.
- **LAMBS 1M** at b_factor 0.2 (baseline, PBC ~ no-op — confirms parity) and a
  higher b_factor where PBC-on differs from off.

Four-way check at each (data, b) where PBC matters:
- paratreet2 PBC-off  ==  old FoF PBC-off  (existing open-boundary agreement,
  ~0.03% boundary offset)
- paratreet2 PBC-on   ==  old FoF PBC-on   (validates OUR PBC)
- paratreet2 PBC-on   !=  paratreet2 PBC-off (proves PBC is exercised)
Use the grid-hash serial reference too, extended to periodic distance, as an
independent oracle (paratreet2's own -c full check).

## Non-goals (this step)
Per-axis distinct periods (trivial extension); non-cubic/tilted boxes;
optimizing the periodic branch (it's already single-walk).
