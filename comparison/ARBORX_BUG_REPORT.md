# ArborX distributed DBSCAN: incorrect, intermittent results on deep
# cross-rank merge graphs

**Affects**: `ArborX::Experimental::dbscan` (`src/cluster/ArborX_DistributedDBSCAN.hpp`)
at commit `51923b4`.
**Platform observed**: OLCF Frontier, Kokkos 4.6.01 with the HIP backend
(MI250X, gfx90a), ROCm 6.2.4, Cray MPICH 8.1.31, gcc 13.2.
`ARBORX_ENABLE_GPU_AWARE_MPI=OFF`.

## Symptom

`Details::verifyDBSCAN` rejects the labels that `Experimental::dbscan` just
produced, on **11 of 12 runs** at 2, 4 and 8 ranks, for a real cosmology
point set. It is intermittent: the same binary on the same input file with the
same rank count both passes and fails across repetitions.

## Reproducer

Nothing but ArborX is needed. Take any 3-D point set that is strongly
clustered and distribute it in **file order** (which is what
`benchmarks/cluster`'s own binary loader does), then:

    ArborX_Benchmark_DistributedDBSCAN.exe -f points.bin \
        --eps <b> --core-min-size 2 --verify

Our case: 100,000 particles from a cosmological N-body snapshot, in the
benchmark's own binary format (`int num_points; int dim; float xyz[]`),
`eps = 0.150205`, `minPts = 2`. Two independent friends-of-friends codes agree
the answer is 33,933 connected components with a largest component of 26,042
particles. ArborX's component count varies between runs (31,350-33,691 over
repetitions) and its largest component comes out 52,947-63,870.

**The stock generator does not expose this**, which is why CI is green:
`generateDistributedData` gives each rank its own spatially separate lattice
block, so any one cluster lives on one or two ranks and the cross-rank merge
graph is shallow. Forcing the generator to percolate by sweeping `eps` past
`spacing` (we tried 1.0 through 8.0 against `spacing = 2`, at 4 and 8 ranks)
does **not** reproduce it -- percolation is not the trigger. What matters is
the *depth* of the merge graph, and a file-order slab of clustered data is the
extreme case: every component has a fragment on every rank, so every component
needs a P-way label reconciliation.

## Cause

`Details::sortAndFilterMergePairs` is responsible for the invariant that
`Details::relabel` depends on, and establishes it with a single pass:

1. sort merge pairs by `(from, to)`;
2. for each run of equal `from`, keep only the entry with the lowest `to`, and
   re-insert `{other_to, to}` for each other `to` in the run so that dropping
   the rest cannot disconnect anything;
3. re-sort -- **without re-filtering.**

Step 2's re-inserted pairs introduce **new `from` values**, and after step 3's
sort such a value can coincide with a `from` that already carries an edge. The
output can therefore still contain two edges out of the same label. `relabel`
resolves a label with

    int next = KokkosExt::lower_bound(from.data(), from.data() + n, label)
               - from.data();

and follows only that first match, so the second edge out of that label is
silently discarded and two components that should merge do not.

Which of the two edges sorts first depends on which particle the *local*
union-find left as a component's representative. That is not deterministic, so
the failure is intermittent rather than a stable wrong answer.

The two `FIXME` comments already in this function mark exactly this spot:

    // Insert other connections that may be required on other ranks
    // FIXME: I need to convince myself that this is truly necessary, or
    // if it's a fix for something that should be addressed in a different
    // place
    ...
    // Re-sort after reinsertion
    // FIXME: I don't know whether we have duplicates here or not. If we do,
    // I don't think it matters

They are necessary, and the duplicates do matter.

## Fix we are using

Iterate steps 1-2 until no `from` value appears more than once -- which is
precisely `relabel`'s precondition. Each pass rewrites at least one label to a
strictly smaller one, so it terminates; we cap at 64 iterations and
`Kokkos::abort` if that is ever hit, which it is not in practice.

Concretely, wrap the existing sort+filter in

    for (int iteration = 0;; ++iteration) {
      Kokkos::sort(space, merge_pairs);
      int num_duplicate_from = 0;                       // parallel_reduce over
      ...                                               // from(i)==from(i-1)
      if (num_duplicate_from == 0) break;
      ... existing filter into new_merge_pairs ...
      merge_pairs = new_merge_pairs;
    }

An alternative worth considering instead: replace the keep-lowest-`to` scheme
with explicit pointer jumping on the label->label graph until it converges,
which reaches the same fixed point without materialising re-inserted edges.

## Separately: three missing fences on receive-path host staging buffers

Independent of the above, three host->device `Kokkos::deep_copy`s are issued on
the `space` execution-space instance -- hence asynchronously -- while their
host staging buffer is a **function-local mirror view** whose last reference
dies as the function returns, so the host allocation can be freed with the
copy still in flight:

* `cluster/detail/ArborX_DistributedDBSCANHelpers.hpp`, `gatherGlobalBoxes`
  (`deep_copy(space, global_boxes, global_boxes_host)`)
* `cluster/detail/ArborX_DistributedDBSCANHelpers.hpp`, `communicateMergePairs`
  (`deep_copy(space, global_merge_pairs, global_merge_pairs_host)`)
* `distributed/detail/ArborX_Distributor.hpp`, `doPostsAndWaits`
  (`deep_copy(space, imports, imports_comm)`)

The GPU-aware branches already fence the *send* buffers ("fill on device done
before MPI_Allgather"); nothing guards a *receive* buffer's lifetime. Adding
`space.fence()` after each of the three visibly stabilised one observable in
our runs (the count of points with no neighbour within eps, which had been
swinging between the correct 27,971 and 91,174 at 8 ranks), though it does not
by itself fix the component counts.

## Also: `comm_size == 1` hangs

`Experimental::dbscan` does not terminate with a single rank. `computeRanksTo`
filters out the local rank's own box, so one rank leaves the
`BoundingVolumeHierarchy` empty and the subsequent query does not return --
the case the existing `FIXME: not 100% sure what's going on`
(`ArborX_DistributedDBSCAN.hpp:309`) refers to. Easy to work around, but it
turns a degenerate configuration into a hang rather than an error.
