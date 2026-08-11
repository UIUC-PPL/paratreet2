# The sequence of optimizations in this project

Written 2026-08-05 (Kale's request): every optimization in order, one to
two sentences each, plain language. Part B lists ideas that were tried,
measured, and discarded — with the reason. Dates are when each landed on
the main branch; deeper detail lives in the design note named with each
entry.

## A. Optimizations adopted, in sequence

1. **Three-level union-find structure** (project foundation, design
   established 2026-07-18). Instead of one global union-find over all
   particles, each worker thread first solves its own particles
   completely, then the threads of one process merge their results, and
   only fragment-level connections cross process boundaries — so the
   expensive distributed machinery handles millions of fragments instead
   of billions of particles.
2. **Pruning certificates and repeat-search suppression in the boundary
   walk** (2026-07-19, design/step3.md). A pair of tree nodes whose boxes
   are farther apart than the linking length is discarded without
   descending; a pair already known connected is discarded; and once one
   witness for a fragment pair is found, a process-wide table stops every
   other search for the same pair.
3. **Owner-encoded fragment identifiers for the distributed union-find**
   (2026-07-20, design/step4.md). Each fragment's identifier carries its
   owning process in the high bits, so any process can route a request to
   the owner by arithmetic instead of consulting a directory.
4. **Symmetric pair walk replacing the flat-list walk** (2026-07-23,
   design/dual-tree.md). The original walk tested every opened tree node
   against a flat list of all local leaves, which cost node-count times
   leaf-count; walking two trees against each other descends both sides
   together and prunes whole TreePiece pairs at once (measured 20x on the
   walk at one process, and it also removed an artificial superlinear
   speedup that had masked the flat walk's waste).
5. **Split only the larger side, nearest child first** (2026-07-23, same
   note). When a tree pair must descend, splitting only the shallower
   side gives a pruning test at every refinement instead of jumping to
   the 64-way product of both sides' children, and exploring the closest
   child first finds witnesses early so the suppression table covers the
   rest.
6. **Positive certificates in the process-local phase** (2026-07-23,
   design/phase1-scaling.md). If two boxes are so close that every cross
   pair must be within the linking length, all their particles are joined
   through one representative without any distance tests; a cheap size
   gate skips the test where it cannot fire.
7. **Connectivity memo in the process-local phase** (2026-07-23, same
   note). Each node remembers a representative it is already connected
   to, so later pair walks that reach proven-connected territory stop
   immediately; on clustered data this cut the dense phase about seven
   times and cut the worst-thread-to-average imbalance from 8.8 to 2.0,
   because the overloaded threads were mostly re-proving known
   connections.
8. **Slimmed remote particle copies** (2026-07-25, design/
   cached-particle-slimming.md). The boundary walk reads only position
   and fragment identifier from copied particles, so only those twenty
   bytes of the roughly hundred-twelve-byte particle are shipped.
9. **Enumeration-free identifiers and lazy vertex storage in the
   distributed union-find** (2026-07-25, design/sparse-uf2-encoding.md).
   Fragment identifiers became pure per-particle rewrites (no counting
   pass, no per-process enumeration), and union-find vertices are created
   only when an edge touches them — at 80M only about 66 thousand of
   23.7 million fragments are ever touched, so the counting wedge, the
   enumeration plateau, the full vertex array, and the full-array scans
   all disappeared.
10. **Process-local stage chaining** (2026-07-25, design/
    phase1-scaling.md). The four global barriers between the stages of
    the process-local phase were replaced by per-process counters where
    the last finishing thread triggers the next stage, so a slow process
    no longer holds every other process at each stage boundary.
11. **The shared work pool for the cross-thread phase** (2026-07-27,
    design/status-poolab-2026-07-27.md). TreePiece-pair work units are
    enumerated geometrically, split until no unit can hide a large chunk
    of work, sorted costliest-first, and claimed one at a time by all of
    a process's threads from a shared cursor — replacing a fixed
    assignment of pairs to threads that could not adapt to uneven cost.
12. **Cell-grid solver for dense TreePieces** (2026-07-28; default
    threshold 4.0 on 2026-08-04 after measurement). A TreePiece whose
    expected occupancy per linking-length cell is high is solved by
    binning particles into a grid and joining neighbors, which is linear
    in particles, instead of walking the tree; at 2B this cut the dense
    phase 19 to 29 percent.
13. **Streaming union requests during the walk** (2026-07-30, design/
    walk-uf2-overlap.md). Boundary-walk edges are submitted to the
    distributed union-find in batches while the walk is still running,
    overlapping the union traffic with the walk instead of serializing
    the two.
14. **Deeper cache shipments** (2026-07-31). Each remote fetch returns a
    deeper slice of the owner's tree, halving the number of
    request-reply round trips at the cost of larger replies.
15. **Single-distribution mode** (2026-08-03/04, design/
    single-distribution-mode.md; default since 2026-08-04). The second
    chare array that used to hold traversal targets is not created at
    all — traversals run from the tree-holding chares — which removed the
    array creation and particle assignment passes (decomposition about
    25 percent faster at 80M, results identical).
16. **Sequential union-find over gathered edges as the production mode**
    (2026-08-04/05). All cross-process edges — under a million even at
    2B — are gathered to one processor, solved sequentially, and the
    label map is broadcast back; the bracket contains no quiescence
    detection and no fine-grained message chains, which removed both the
    distributed union-find's setup cost (1.2 s at 16 nodes) and its
    exposure to the network library's idle-connection stalls (its
    bracket had reached 3.7 s under a stall; the sequential bracket
    stays at about 0.02 s at every scale measured).
17. **Correct component counting for the sequential mode** (2026-08-05).
    The distributed component counter classifies labels by sign
    (positive means the component lives entirely in one process and is
    counted locally; negative means globally summed), and the sequential
    mode now writes the negative form for every edge-touched fragment —
    fixing phantom extra components that appeared once per extra process
    sharing a positive label.
18. **Ownership pruning in the boundary walk** (2026-08-05). A pair
    whose source node is the process's own data cannot produce a
    cross-process edge (the process-local phase already found every
    local connection), so such pairs are discarded before descent; this
    removed the long busy region at the start of the walk (same-fragment
    prunes fell from 8.1 million to 20 at 16M).
19. **Symmetry pruning in the boundary walk** (2026-08-05). Every pair
    of tree pieces used to be walked from both owners, discovering every
    edge exactly twice; a balanced tie-break now keeps each pair on one
    owner only, halving the walk's pair work, the remote fetches, and
    the emitted edges.
20. **Walk termination by credit counting** (2026-08-05). Every message
    that carries walk continuation holds a credit added before sending
    and removed after its work completes; when a process's count reaches
    zero a single reduction ends the walk — replacing quiescence
    detection, whose repeated quiet-network rounds were both slow and
    exposed to the idle-connection stall.
21. **Diagnostics moved off the timed path** (2026-08-05). The cache
    memory accounting (a pointer walk over the whole cached tree) and
    the fragment-size histogram (a full pass over all particles) are now
    opt-in flags instead of running in every timing run.
22. **Cross-process work stealing for the shared pool** (2026-08-05,
    design/phaseb-offload.md stages 1-2). A pool work unit reads only
    frozen data, so an idle process can execute it: after finishing its
    own pool, each thread asks the other processes on its machine for
    units, receives the two TreePieces in flattened form, walks them, and
    returns the edges; the victim's merge waits until every shipped
    batch is home. Measurement against the 2B imbalance is in progress.

## B. Ideas tried, measured, and discarded

1. **Parking redundant searches** (design/step3.md "3b", explored
   2026-07-19 to 2026-07-23). The plan was to park a walker that reaches
   a fragment pair another walker is already probing and wake it only if
   the probe fails. Retired: at scale the redundancy per process FALLS
   (about as the inverse of process count to the 0.89 power), the
   remaining redundant work was about one percent of the walk, and most
   of it is unavoidable no-edge verification that parking cannot save.
2. **Remembering failed connectivity checks** (2026-07-23). Memoizing
   negative results ("these two are not connected") was measured worse
   both ways it was tried: exact-epoch invalidation added 140 percent
   map-maintenance cost on sparse data, and time-backoff blocked fresh
   suppressions and cost 1.5x on dense data. Only positive results are
   memoized; failed checks are cheap by construction.
3. **The positive certificate in the boundary walk** (2026-07-19
   onward). The test "every point pair within reach, emit without
   descending" is structurally sound but measured essentially never
   firing there — depth-first search with suppression always finds a
   witness first (once through a full percolation sweep). It survives in
   the code but earns nothing; the process-local phase is where positive
   certificates pay.
4. **Message aggregation for union traffic** (library integrated
   2026-07-23, default off 2026-08-05). Batching the distributed
   union-find's messages through the aggregation library was kept as
   insurance for very large scales, but at every scale measured (up to
   16 nodes) it was equal or slightly worse — the two-level work split
   had already cut the traffic to a few thousand messages per process —
   and it silently disabled the streaming overlap and cluttered traces.
   Off by default; revisit only as an explicit study.
5. **A keep-alive message ring against the network idle stall**
   (2026-08-04/05). A raw runtime-level message per process every
   hundred milliseconds was meant to keep network connections warm; it
   was verified to be sending correctly, yet the stalls persisted in the
   distributed union-find bracket (3.7 s with the ring on). Superseded
   by the sequential mode, whose bracket does not use the vulnerable
   communication pattern at all; the underlying network-library bug
   remains reported.
6. **Prediction-based work placement** (considered 2026-07-23/25 for
   the dense-phase imbalance). Placing TreePieces by a density-based cost
   model was rejected because the predictor's correlation with actual
   cost collapsed at scale (0.90 on the laptop, near zero at 1920
   processors — the suppression machinery absorbs exactly the work the
   predictor counts); work stealing reacts to observed progress instead.
   The same reasoning chose stealing over prediction-push for
   cross-process offload.
7. **Fixed assignment of cross-thread pairs** (superseded 2026-07-27).
   The predecessor of the shared pool assigned TreePiece pairs to threads
   by a symmetric hash; it could not adapt when one pair was much more
   expensive than the others and one thread's largest unit set the
   phase's finish time.
8. **The 64-way joint descent in the symmetric walk** (superseded
   2026-07-23, v2 of the dual-tree work). Splitting both sides of a
   pair at once produced the full product of children with only about
   five percent of leaf pairs surviving; splitting one side at a time
   with a pruning test at each level reached 2.3x fewer node tests than
   even the old flat walk.
9. **Edge-level parity instead of pair-level symmetry pruning**
   (analyzed and rejected 2026-08-05). Letting both sides walk but only
   one side emit would have saved only the duplicate edge transport,
   not the duplicated walk and fetches — and it cannot be combined with
   pair-level pruning, because with pair pruning active an edge may be
   discoverable on only one side while the parity rule selects the
   other.
10. **Quiescence detection as a phase terminator, generally**
    (retired stepwise 2026-07-24 through 2026-08-05). Used originally
    after decomposition steps, between process-local stages, over the
    boundary walk, and in the union bracket; replaced respectively by
    message dependencies, per-process counters, credit counting, and
    reductions. Each replacement removed rounds of quiet-network
    global agreement — the pattern most exposed to the network
    library's idle stall and the slowest thing to do on a large quiet
    machine. It survives where message counts are genuinely
    unpredictable and cheap: the distributed union-find mode's combined
    walk-plus-union bracket, and the traversal oracle paths.
11. **Vendored copy of the union-find library** (superseded
    2026-07-21). An in-tree patched copy of the library was replaced by
    linking the sibling repository once the fixes were merged upstream,
    to avoid a fork.
