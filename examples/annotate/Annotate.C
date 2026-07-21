#include "Main.h"
#include "Paratreet.h"
#include "CheckVisitor.h"

  using namespace paratreet;

// Post-build mutation applied to the SUBTREE's own leaf copies (the copies
// traversals fetch through the cache). Partition* is nullptr in that context,
// so it must not be used here.
PARATREET_REGISTER_PER_LEAF_FN(DensityStampFn, MinMaxData, (
  [](SpatialNode<MinMaxData>& leaf, Partition<MinMaxData>* partition) {
    for (int pi = 0; pi < leaf.n_particles; pi++) {
      auto copy_part = leaf.particles()[pi];
      copy_part.density = (Real)(3.0 * copy_part.order);
      leaf.changeParticle(pi, copy_part);
    }
  }));

  void ExMain::preTraversalFn(ProxyPack<MinMaxData>& proxy_pack) {
    // Ordering (same constraint as fof3 / design step3.md §2): the mutation
    // and upwardPass must complete BEFORE loadCache ships the starter pack,
    // or multi-process traversals fetch stale (pre-mutation) cached canopy
    // data. Do them here, in preTraversalFn, ahead of loadCache.
    // (1) mutate the subtree leaf copies after tree build
    proxy_pack.subtree.callPerLeafFn(
        PARATREET_PER_LEAF_FN(DensityStampFn, MinMaxData),
        CkCallbackResumeThread());
    // (2) recompute node annotations bottom-up and refresh the TreeCanopy
    proxy_pack.subtree.upwardPass(CkCallbackResumeThread());
    // (3) push the mutated leaf particles to any remote CacheManager that took
    // a build-time flat_subtree copy of this subtree (non-matching decomps).
    // Those copies double as SOURCE leaves in the down-traversal but predate
    // the mutation above; without this they read stale (pre-mutation) data.
    // No-op under matching decomps (remote sources fetched live).
    proxy_pack.subtree.refreshCopies(CkCallbackResumeThread());
    CkWaitQD(); // let canopy propagation + copy refresh settle
    // (4) now load the cache, with the mutated/annotated data
    proxy_pack.driver.loadCache(CkCallbackResumeThread());
  }

  void ExMain::traversalFn(BoundingBox& universe, ProxyPack<MinMaxData>& proxy_pack, int iter) {
    // traverse everything, checking annotations against particles
    proxy_pack.partition.template startDown<CheckVisitor>(CheckVisitor());
  }

  void ExMain::postIterationFn(BoundingBox& universe, ProxyPack<MinMaxData>& proxy_pack, int iter) {
    void* result = nullptr;
    min_max_tracker.collect(CkCallbackResumeThread(result));
    CkReductionMsg* msg = (CkReductionMsg*)result;
    double* vals = (double*)msg->getData();
    double gmin = -vals[0];
    double gmax = vals[1];
    double expected_min = 0.0;
    double expected_max = (double)(Real)(3.0 * (universe.n_particles - 1));
    if (gmin == expected_min && gmax == expected_max) {
      CkPrintf("ANNOTATE TEST PASSED: density range [%g, %g] over %d particles\n",
               gmin, gmax, universe.n_particles);
    } else {
      CkPrintf("ANNOTATE TEST FAILED: got range [%g, %g], expected [%g, %g]\n",
               gmin, gmax, expected_min, expected_max);
    }
    delete msg;
    min_max_tracker.reset(CkCallbackResumeThread());
  }

  Real ExMain::getTimestep(BoundingBox& universe, Real max_velocity) {
    return 0.01570796326;
  }
