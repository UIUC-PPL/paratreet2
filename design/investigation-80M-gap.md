# Investigation: the 80M "23M vs 2M components" gap

Status: RESOLVED (2026-07-20). Cause = min-component pruning convention,
NOT periodic boundary conditions.

## The report
A student ran paratreet2 (no PBC) on an 80M cosmological dataset and got
~23M FoF components; old paratreet FoF reported ~2M on the comparison.
The gap (~11.5x) was tentatively attributed to paratreet2 lacking the
periodic-image (PBC) walks that old FoF has.

## What we tested (controlled, on data we fully control)

### Round 1 - synthetic data (Plummer + uniform box), <=100k
Old paratreet FoF (examples/FoFApp, built against the sibling unionfind)
run PBC-on vs PBC-off at the *identical* numeric b (taken from
paratreet2's own FOF3STAT config line, fed via -ll):
- PBC on/off changed component count by 0-0.01%. Not the gap.
- paratreet2 (no PBC) matched old-FoF-PBC-off to 0.03-0.4%.
Inconclusive for the real regime: synthetic data isn't filamentary /
near-percolation.

### Round 2 - REAL cosmological data (LAMBS subsamples)
`lambs.00200_subsamp_1M` and `_30K` (paratreet/{examples,tests}); box
[-0.5,0.5]^3, period 1.0. Same three-way comparison at identical b
(1M: b = 0.00200000, factor 0.2):

| Config                    | Components | 
|---------------------------|-----------|
| paratreet2 (no PBC)       | 379,884   |
| old FoF, PBC off, -c 0    | 380,000   |
| old FoF, PBC on,  -c 0    | 379,562   |

- **PBC on/off: 0.12%** (438 of 380k). PBC exonerated on real
  filamentary data too.
- **paratreet2 vs old-FoF-off: 0.03%.** First validation of paratreet2
  correctness on non-synthetic data - it passes.

### Round 3 - the actual cause: pruning
Old FoF prunes components below a min particle count (its `-c
min_vertices_per_component`, a standard cosmological FoF convention -
groups below ~20 particles are numerically unresolved). paratreet2's
fof3 harness reports EVERY component including singletons. On the 1M
LAMBS data, old FoF at the same b:

| old FoF pruning       | components | reduction vs unpruned |
|-----------------------|-----------|----------------------|
| -c 0 (none)           | 380,000   | 1.0x                 |
| DEFAULT (no -c flag)  | 27,554    | 13.8x                |
| -c 20                 | 2,320     | 163.8x               |

The DEFAULT-pruning reduction (13.8x) matches the student's 80M gap
(23M/2M = 11.5x) closely. The small residual is expected and in the
right direction: at 80M the halo mass function is better resolved than
at 1M, so more halos clear a fixed particle-count threshold, giving a
*smaller* reduction ratio (11.5x < 13.8x). The regime is otherwise
N-independent because b is defined in mean-interparticle-separation
units (0.2), which is below the FoF percolation threshold (~0.25-0.29)
at every N.

## Conclusion
The student's 23M is not wrong - it is the full, unpruned component
count. Old FoF's 2M is the same computation with small groups pruned
away. paratreet2 and old FoF agree to 0.03% when both report all
components. PBC and any paratreet2 bug are ruled out as the cause.

## Actionable follow-up
paratreet2 needs a **min-component-size pruning / reporting option** to
be a fair drop-in replacement for old FoF (and ChaNGa FoF), which prune
by convention. The distributed UF_2 library (src/uf2) already carries a
`prune_components(threshold)` entry; wiring it (and a matching serial
path) into the fof3 reporting is the concrete next step. Until then,
compare like-for-like: run old FoF with `-c 0`, or paratreet2 with an
equivalent post-hoc size filter on its component histogram.

Ask the student, to confirm at 80M: what `-c` value did the old-FoF
baseline use, and re-run old FoF with `-c 0` (or paratreet2's count
filtered to the same threshold) - the two should then match to <1%.
