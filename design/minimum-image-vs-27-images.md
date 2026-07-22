# Why one "minimum-image" walk replaces 27 literal image walks (PBC)

This explains the choice made in `design/pbc.md` / the PBC implementation:
paratreet2 finds periodic (wraparound) FoF links with a SINGLE tree walk using
a periodic distance function, rather than old paratreet FoF's 27 separate
walks over shifted copies of the box. The two give identical FoF groups when
the linking length `b < L/2` (the box side over two) — which always holds for
FoF. Here is the reasoning in full.

## 1. What PBC has to find

The simulation box is periodic with side `L`: the face at `x = 0` is glued to
the face at `x = L`, and likewise in y and z. A particle just inside one face
is physically a neighbor of a particle just inside the OPPOSITE face — they are
a small distance apart "across the wrap", even though their raw coordinates
differ by nearly `L`. FoF must link such pairs (any two particles within `b`,
measured the periodic way).

Equivalently: imagine tiling all of space with copies ("images") of the box,
offset by every vector `(iL, jL, kL)` for integers i, j, k. A particle `a` in
the central box should link to a particle `b` if `a` is within `b` of `b` OR of
ANY image of `b`.

## 2. The 27-image approach (old paratreet FoF)

Only the 26 images immediately surrounding the central box can matter (a link
of length `b` cannot reach past one box when `b < L`). Together with the
central box that is a 3×3×3 = **27** arrangement, with offset vectors
`v ∈ {-L, 0, +L}³`.

So old FoF does the whole boundary walk **27 times**: once for each offset `v`,
each time comparing region A against region B shifted by `v`. Most of the 27
prune almost immediately (a shifted copy of B is far from A), but the machinery
runs 27 times.

```
        image  image  image          For a target region A near the left face,
      +------+------+------+          only the images of B shifted by +L in x
      |      |      |      |          (the "+L" column) sit near A. The other
      | B-L  |  B   | B+L  |   ...    24 offsets are far and prune. But the
      |      |  A   |      |          walk still visits all 27 to find that out.
      +------+------+------+
```

## 3. The minimum-image approach (paratreet2)

Instead of moving the boxes, we move the DISTANCE FUNCTION into the periodic
world. For two points `a` and `b`, per axis take the raw difference and fold it
into the half-open period `[-L/2, +L/2]`:

```
    d      = a - b
    d_wrap = d - L * round(d / L)     // nearest multiple of L removed
```

`d_wrap` is the signed distance to the **nearest image** of `b` along that
axis. Combine the three axes and you have the distance to the single closest
image of `b` — no box replication, no offset loop. FoF's per-pair test becomes
`periodicDistSq(a, b, L) <= b*b`, and the tree-walk box pruning uses the same
idea on intervals (`mindist2(boxA, boxB, L)` folds the inter-box gap the same
way). One walk, done.

## 4. Why the two are IDENTICAL when b < L/2

This is the crux. The images of `b` are spaced exactly `L` apart. Pick the
nearest one; call its distance `r` (so `r <= L/2` by construction — the nearest
image is never more than half a period away). The NEXT-nearest image is a full
period further along that axis, so it is at distance at least `L - r`.

If `b < L/2`, then `L - r >= L - L/2 = L/2 > b`. So the second-nearest image is
strictly farther than `b`: it can never be a neighbor. Therefore **at most one
image of `b` can lie within `b` of `a` — the nearest one.**

That collapses the 27-image question ("is `a` within `b` of ANY image of
`b`?") to the minimum-image question ("is `a` within `b` of the NEAREST image
of `b`?"), which is exactly what `d_wrap` computes. Same links, hence the same
FoF groups.

### 1-D worked example (period L = 1)

Particles at `a = 0.02` and `b = 0.97`, linking length `b = 0.1`.
- Raw distance: `|0.02 - 0.97| = 0.95` — looks far, would NOT link.
- Minimum image: `d = 0.02 - 0.97 = -0.95`; `round(-0.95/1) = -1`;
  `d_wrap = -0.95 - 1*(-1) = 0.05`. Distance `0.05 <= 0.1` — they DO link
  (across the wrap). Correct: `a` and `b` are 0.05 apart around the boundary.
- 27-image check would find the same: `b` shifted by `-L` sits at `-0.03`,
  distance `|0.02 - (-0.03)| = 0.05`. Identical answer, one arithmetic step
  instead of a shifted walk.

## 5. What minimum-image changes in the tree-walk opening criterion

The dual-tree walk decides, for a node pair `(A, B)`, whether to PRUNE (stop)
or OPEN (descend into children). The criterion is a box-gap test: prune when
`mindist2(A.box, B.box) > b*b`, else open. Under PBC that test becomes the
PERIODIC box gap, `mindist2(A.box, B.box, L)` (the per-axis fold of §3), so the
opening criterion genuinely changes:

- **Wrap-adjacent pairs now open that previously pruned.** Two boxes near
  OPPOSITE faces have a direct gap ~L (would prune) but a small wrapped gap
  (now opens) — exactly the pairs that can hold periodic links, which the walk
  must descend into. Interior pairs are unchanged: their wrapped images are
  far, so periodic `mindist2` == plain `mindist2` and they prune identically.

- **Still correct — pruning never drops a real link.** Pruning is safe only if
  the criterion is a valid LOWER BOUND on the true (periodic) closest
  point-pair distance. The periodic box gap is that lower bound *provided each
  box fits within one period* (`period >= box extent`), because then the
  `{-L,0,+L}` fold captures the nearest image. That is exactly why §8 checks
  `period >= box extent` at startup: violate it and a box could span more than
  a period, the fold could miss the nearest image, and the bound could be
  wrong.

- **Same opening decisions as the 27-image walk.** A pair opens under
  minimum-image iff SOME offset would open it in the 27-image scheme
  (`min over v of mindist2(A, B+v) <= b`). The two explore the same pairs and
  reach the same links; minimum-image just folds the 27 box-shifted walks into
  one whose distance function already knows the nearest image. As the walk
  descends, each child sub-box gets its own fold, so if child A1 is nearest to
  B directly while A2 is nearest to B's +L image, both are handled in the one
  traversal.

- **Cost: coarse boxes near a face cannot prune.** A box spanning a large
  fraction of L has a periodic gap ~0 to almost anything (its images tile the
  domain), so it always opens; the walk descends until sub-boxes are small
  enough that the wrapped gap exceeds b. That extra descent near the faces is
  the inherent price of PBC — the same work old FoF pays across its 27 images,
  here localized to the boundary.

- **The positive certificate (case 2) is disabled under PBC.** In phase 3,
  open() also has a `maxdist2 <= b*b` "all pairs within b -> guaranteed edge"
  shortcut. A periodic "farthest image" is ill-defined, and the certificate
  never fires in practice anyway, so under PBC open() uses only the periodic
  `mindist2` negative certificate plus SEEN suppression. The pruning stays a
  clean lower-bound test.

## 6. Does FoF actually satisfy b < L/2? (yes, hugely)

FoF's linking length is `b = 0.2 * mean interparticle spacing`, and the mean
spacing is `L / N^(1/3)` for `N` particles in the box. So
`b = 0.2 * L / N^(1/3)`. Even at `N = 8` that is `0.1 L`; for a realistic
`N = 10^6` it is `0.000002 L`. Even the aggressive `b_factor = 1.0` we used in
percolation tests gives `b = L / N^(1/3) << L/2`. The `b < L/2` condition holds
by orders of magnitude, always. Minimum-image is not an approximation here — it
is exact.

## 7. Why we prefer it

- **Cost.** One walk instead of 27. In phase 3 the boundary walk rides the
  framework traverser; 27 images would mean 27 full traversals (setup, cache
  fetches, quiescence) versus one.
- **Simplicity.** A periodic distance function, no offset bookkeeping, no
  SEEN-table interaction across 27 passes.
- **Same answer.** Validated against old FoF's 27-image walk (see
  design/pbc.md): identical to the codes' existing ~0.01% systematic offset at
  moderate `b`, tracking together through percolation.

## 8. The validity constraints (checked at startup)

Minimum-image (and the box-gap pruning) are exact only when:
1. `b < L/2` — the argument in §4. If violated, a particle could reach two
   images and the single nearest-image test would miss links.
2. `L >= the particle bounding-box extent` on each axis — the period must be
   the real box, so particles live within one period and node-box gaps fold
   correctly. (`L` = box side; passing a period smaller than the data is a
   user error.)

Both are cheap to check once the universe box and `b` are known, so the FoF
app aborts at startup with a clear message if either is violated rather than
silently producing wrong groups (see the `-P` handling in
`examples/fof3/FoF3.C`).
