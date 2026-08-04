# Gravity: monopole Barnes-Hut on paratreet2

This folder holds the application-side code only: the node payload
(`GravityData.h`), the traversal visitor (`GravityVisitor.h`), and the
driver/verification harness (`Main.C`). The tree construction, the
parallel traversal engine, the software cache for remote tree data, and
the iteration loop all live in the framework (`src/`), so the whole
algorithm is not visible from this folder. This file explains the full
parallel flow of control and points at where each piece runs.

Design history and measured accuracy baselines: `design/barnes-hut-app.md`.

## The cast: which objects exist, and at what granularity

Everything below is a Charm++ chare collection created by the framework
(see `src/paratreet.ci`); the application never creates them directly.

- **Reader** (one per processor): loads the input file, computes
  space-filling-curve keys, and redistributes particles during
  decomposition.
- **Subtree** (an array, ~8 elements per processor by default): each
  element OWNS a contiguous block of particles — one spatial region —
  and builds a local tree over them each iteration.
- **Partition** (an array, same element count here): each element owns
  the *traversal work* for a region — its list of target leaf buckets.
  With matching decompositions (this app), a Partition's buckets are the
  very same leaf nodes its co-located Subtree built (pointer identity;
  particles are stored once).
- **TreeCanopy** (a sparse array keyed by tree-node key): the tree nodes
  ABOVE the subtree roots. Their payloads are combined from subtree
  contributions, so every process can hold the top of the global tree.
- **CacheManager** (one per process): the software cache. Holds the top
  of the global tree plus every remote subtree slice fetched so far this
  iteration. Shared, lock-free, by all processors of the process.
- **Resumer** (one per processor): bookkeeping for traversals that had
  to pause waiting for remote data — who waits on which tree node, and
  waking exactly those waiters when the data arrives.
- **Driver** (a single chare): orchestrates the whole run from one
  thread; every numbered step below is issued from it in order.
- **GravityCheck** (one per processor; this app's own, declared in
  `Main.ci`): collection plumbing for the verification harness.

## One iteration, in order

Steps run one after another, separated by completion detection
(reductions or quiescence — "quiescence" means the runtime has proven no
messages are in flight anywhere). WITHIN each step, everything is
parallel and asynchronous.

1. **Decomposition** (first iteration, and again on later rebuilds).
   Readers load particles, compute their space-filling-curve keys, and
   sort them into spatial regions using globally agreed splitter keys.
   Each region's particles are sent to its Subtree element.

2. **Tree build** (every Subtree element, independently, no messages).
   Each Subtree sorts its particles and recursively builds its local
   tree. `GravityData` is computed bottom-up DURING this build: the leaf
   constructor accumulates mass, mass-weighted position, bounding box,
   and opening radius over the leaf's particles, and `operator+=` folds
   children into parents on the way up. At the subtree root, the payload
   is contributed upward into the TreeCanopy layer, where the same
   `operator+=` combines sibling subtrees; canopy results stream to the
   Driver. So by the end of tree build, every tree node — leaf to global
   root — carries total mass, center of mass, and opening radius.

3. **Cache load** (`preTraversalFn` here calls `driver.loadCache`). The
   Driver broadcasts the collected canopy — the top of the global tree —
   to every process's CacheManager. After this, every process can start
   traversing from the global root without any communication.

4. **The force traversal** (`traversalFn` here). One broadcast —
   `partition.startDown<GravityVisitor>` — starts an independent
   top-down walk on EVERY Partition element at once. Each element walks
   the global tree once against its whole list of target buckets:

   - At each tree node, the framework calls the visitor's `open(source,
     target)` — the Barnes-Hut acceptance test. If the node is far
     enough from the target bucket's box (opening radius against
     distance, scaled by theta), the walk calls `node()`: the monopole
     contribution (treat the node as a point mass at its center of
     mass) is added to every particle in the bucket, and that branch of
     the walk ENDS. Otherwise the walk descends to the children.
   - Descent that reaches a leaf calls `leaf()`: direct particle-by-
     particle summation with softening.
   - **The remote case is where the parallelism gets interesting.** The
     walked tree is the GLOBAL tree, but a process only holds its own
     subtrees plus the canopy top. When the walk descends into a node
     whose children live on another process, the framework (not the
     visitor — the visitor never knows) does the following: the FIRST
     walker on this process to hit that node sends one request to the
     owning process's CacheManager; every walker that hits it registers
     itself with the Resumer and PARKS that branch, then continues with
     other branches of its walk — nothing blocks. The owner replies
     with a slice of its subtree (the node, a few levels below it, and
     leaf particles — the share depth is configurable). The reply is
     installed once into the process-wide cache, and the Resumer wakes
     exactly the walkers that were waiting on that node; they resume
     from where they parked. Every later traversal on this process
     finds the data already cached.
   - There is no counting of outstanding work: the framework detects
     completion of the whole traversal — all walks, all fetches, all
     resumptions — by quiescence.

5. **First half-kick** (`Partition::kick`): every bucket's particle
   velocities advance by half a step using the just-computed
   accelerations. Skipped for apps that set
   `perturb_particles = false`; gravity keeps it on.

6. **`postIterationFn`** (application hook — here, the verification
   harness at iteration 0; see below).

7. **Drift and rebuild** (`Partition::perturb` + rebuild): the second
   velocity half-kick, accelerations reset to zero, positions advance a
   full step. The moved particles are re-decomposed (back through the
   Readers on a full rebuild) and the next iteration builds a fresh tree
   over the new positions. The two half-kicks around the drift make the
   integrator the standard kick-drift-kick leapfrog.

## What the application actually provides

Three things, everything else above is framework:

- `GravityData`: the per-node payload and how it combines upward.
- `GravityVisitor`: `open()` (accept or descend), `node()` (accepted
  monopole), `leaf()` (direct summation with cubic-spline softening).
  `Visitor::CallSelfLeaf = true` tells the framework a bucket must also
  interact with its own leaf.
- `Main.C`: flags, defaults, the three hook functions, and the checker.

## The verification harness (`-c`, default on for inputs up to 10k)

After the traversal (accelerations computed, positions not yet moved),
a per-leaf callback deposits every particle's position, mass, softening,
and computed acceleration into the local GravityCheck branch; one
concatenating reduction delivers all records to processor 0. There, a
serial reference computes all pairwise forces — the same kernel
formulas, evaluated in double precision (the build's default `Real` is
single precision) — and the per-particle relative error is reported and
gated.

Two gates: at production theta the root-mean-square error must sit in
the recorded monopole error band (~4e-3 at theta 0.7); and with `-o 0`
the visitor opens EVERY node, so the walk degenerates into the same
all-pairs summation as the reference and must match it to within
single-precision accumulation noise — that mode checks the entire
walk/cache/resumption machinery exactly, independent of the physics
approximation. (No positive theta can do that: the acceptance test is
scale-relative, so some node always qualifies; the measured error just
follows the theta-squared law downward.)

## Flags

- `-f <file>` input (Tipsy format), `-d oct` decomposition, `-i <n>`
  iterations — framework flags.
- `-o <theta>` opening angle (default 0.7; `0` = exact always-open
  verification mode).
- `-T <dt>` fixed leapfrog timestep (default 0.01).
- `-c full|off|auto` verification (auto = on for N <= 10k, iteration 0).

`make test` runs seven configurations (accuracy band + exact mode,
single- and multi-process, plus a five-iteration integration run); every
run must print `GRAVITY TEST PASSED`.
