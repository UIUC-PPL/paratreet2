# paratreet2-specific Charm++ gotchas

Charm/Converse lessons tied to THIS codebase's internals (the cache, the
Subtree/Partition copy split, the FoF certificate walk). General, transferable
Charm lessons live in the machine-wide practitioner file
(`~/software/tutorialcharmclaude/notes/charm_best_practices.md`); this file is
only for the paratreet2-internal specifics that would be noise there. When a
lesson has both a general kernel and a project detail, the kernel goes to the
general file and the detail lands here.

## Two particle copies, two audiences

The framework keeps separate copies of the same particle data for different
roles: **Subtree** copies serve cache fetches; **Partition** copies are
traversal targets. Any post-build mutation (relabel, annotate, per-leaf) must
target the copy its consumer actually reads. Document which API touches which.
This is the shape behind the `callPerLeafFn` multi-process staleness bug: a
post-build mutation on the Partition copy was invisible to remote consumers
reading the Subtree copy under non-matching decompositions. (General kernel —
"a build-time snapshot shipped to a remote cache goes stale on later mutation"
— is in the general file.)

## Canopy-generation dedup in the cache-shipping path

`Driver::recvTC` appends canopy snapshots on tree build AND on every
`upwardPass`, so the collector accumulates multiple generations per key; the
shipper's unstable `std::sort` then ships an arbitrary generation. A "call the
shipper after the refresh" reorder does NOT fix it — the fix is keep-newest
dedup (`stable_sort` + last occurrence per key). Symptom: nondeterministically
stale cached annotations. (General kernel is in the general file.)

## Local-tree internal nodes have n_particles = -1 BY DESIGN

Only LEAF nodes carry particle counts; internal nodes of a Subtree's live
local tree keep the constructor sentinel -1 (Node.h: "non-leaves will have
this as -1"), and empty regions are EmptyLeaf children with count 0. Any
code descending a local tree by "child that has particles" must therefore
test `n_particles != 0` (skip known-empty), never `> 0` — with `> 0` a deep
dense chain (7 EmptyLeaf + 1 Internal(-1) per level, routine in LAMBS
halos) never qualifies a child and the descent spins forever. Bit us in
the phase-1 certificate work (2026-07-23, 77 CPU-minutes of spin); the
`n_particles == 0` "skip empty" guards elsewhere were accidentally correct
because -1 passes them. Note the contrast: cache-shipped SpatialNode
copies (what phase-3 visitors see) DO carry populated counts — the -1 rule
is specifically about the live local tree.

## FoF positive certificate (case 2) rarely fires — test it explicitly

In the phase-3 dual-tree walk the first encounter of a fragment pair is the
topmost (largest-box) pair, so the case-2 positive certificate (`maxdist <= b`
there) essentially never fires: by the time descent shrinks the boxes enough, a
leaf witness has already marked the pair SEEN (suppression precedes the
certificate). Its in-vivo counter stayed 0 across all data including through
percolation (design/step3.md §6c). Unit-test the path; don't infer "dead code"
from the live counter. (General kernel — "a path whose in-vivo counter can stay
0 needs a unit test" — is in the general file.)
