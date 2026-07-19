#include "Main.h"
#include "Paratreet.h"
#include "FoFPhase1.h"
#include "FragCheckVisitor.h"

#include <unordered_map>
#include <vector>

using namespace paratreet;

  void ExMain::preTraversalFn(ProxyPack<FragData>& proxy_pack) {
    proxy_pack.driver.loadCache(CkCallbackResumeThread());
  }

  // Serial O(n^2) reference FoF over the gathered records; canonicalizes
  // both partitions by min particle order per component and compares.
  // Returns the reference component count and largest component size
  // through the out-parameters (reused by the fragment-histogram checks).
  static void verifyPhase1(const FoFParticleRecord* recs, int n, double b2,
                           long& n_components_out, long& max_size_out) {
    // Serial union-find over record indices, union by min order.
    std::vector<int> parent(n);
    for (int i = 0; i < n; i++) parent[i] = i;
    std::function<int(int)> find = [&](int x) {
      int root = x;
      while (parent[root] != root) root = parent[root];
      while (parent[x] != root) { int nx = parent[x]; parent[x] = root; x = nx; }
      return root;
    };
    auto unite = [&](int x, int y) {
      int rx = find(x), ry = find(y);
      if (rx == ry) return;
      if (recs[rx].order < recs[ry].order) parent[ry] = rx;
      else                                 parent[rx] = ry;
    };
    // Reproduce the library's distance arithmetic exactly (Vector3D<Real>).
    for (int i = 0; i < n; i++) {
      Vector3D<Real> pi(recs[i].x, recs[i].y, recs[i].z);
      for (int j = i + 1; j < n; j++) {
        Vector3D<Real> pj(recs[j].x, recs[j].y, recs[j].z);
        if ((pi - pj).lengthSquared() <= b2) unite(i, j);
      }
    }

    // Canonical serial representative per record: order of the component's
    // min-order member (guaranteed by union-by-min + full compression).
    // Also count components and the largest component size.
    std::vector<long> serial_rep(n);
    std::unordered_map<int, long> comp_size; // root index -> size
    int n_components = 0;
    long max_size = 0;
    for (int i = 0; i < n; i++) {
      int r = find(i);
      serial_rep[i] = recs[r].order;
      if (r == i) n_components++;
      long sz = ++comp_size[r];
      if (sz > max_size) max_size = sz;
    }

    // Canonical phase-1 representative per record: min order within each tip.
    std::unordered_map<long, long> tip_min_order;
    for (int i = 0; i < n; i++) {
      auto it = tip_min_order.find(recs[i].tip);
      if (it == tip_min_order.end()) tip_min_order.emplace(recs[i].tip, recs[i].order);
      else if (recs[i].order < it->second) it->second = recs[i].order;
    }

    int n_mismatch = 0;
    for (int i = 0; i < n; i++) {
      long phase_rep = tip_min_order[recs[i].tip];
      if (phase_rep != serial_rep[i]) {
        if (n_mismatch < 5) {
          CkPrintf("FOF1 MISMATCH: particle order %d at (%g, %g, %g): "
                   "serial rep %ld, phase-1 rep %ld (tip %ld)\n",
                   recs[i].order, (double)recs[i].x, (double)recs[i].y,
                   (double)recs[i].z, serial_rep[i], phase_rep, recs[i].tip);
        }
        n_mismatch++;
      }
    }

    if (n_mismatch == 0) {
      CkPrintf("FOF1 TEST PASSED: %d components\n", n_components);
    } else {
      CkPrintf("FOF1 TEST FAILED: %d of %d particles mismatched "
               "(serial: %d components, phase-1: %d tips)\n",
               n_mismatch, n, n_components, (int)tip_min_order.size());
      CkAbort("FOF1 TEST FAILED");
    }
    n_components_out = n_components;
    max_size_out = max_size;
  }

  void ExMain::traversalFn(BoundingBox& universe, ProxyPack<FragData>& proxy_pack, int iter) {
    // Linking length from the universe box: b = 0.2 * (V/N)^(1/3).
    double V = universe.box.volume();
    int N = universe.n_particles;
    double b = 0.2 * std::cbrt(V / (double)N);
    CkPrintf("FoF linking length b = %g (V = %g, N = %d)\n", b, V, N);

    // Phase-1 sequence: register -> phaseA -> phaseB -> merge -> relabel.
    paratreet::runFoFPhase1(proxy_pack.subtree, fof, fof_node, b);

    // Gather (position, tip, order) for every particle and verify against a
    // serial O(n^2) FoF with the same b.
    void* result = nullptr;
    fof.collect(CkCallbackResumeThread(result));
    CkReductionMsg* msg = (CkReductionMsg*)result;
    int n = msg->getSize() / sizeof(FoFParticleRecord);
    if (n != N) {
      CkPrintf("FOF1 TEST FAILED: gathered %d records, expected %d\n", n, N);
      CkAbort("FOF1 TEST FAILED");
    }
    verifyPhase1((const FoFParticleRecord*)msg->getData(), n, b * b,
                 serial_n_components, serial_max_size);
    delete msg;

    // Step 1 completion (design/phase1.md "After phase 1"): recompute the
    // FragData node annotations from the relabeled particles, then traverse
    // everything, asserting leaf annotations against particles (remote
    // sources ship through the CacheManager, so cached leaves are checked
    // too). CkWaitQD both lets canopy propagation settle before the
    // traversal and ensures the traversal's asserts all ran before the
    // histogram is declared valid.
    proxy_pack.subtree.upwardPass(CkCallbackResumeThread());
    CkWaitQD();
    proxy_pack.partition.template startDown<FragCheckVisitor>(FragCheckVisitor());
    CkWaitQD();

    // Fragment-size histogram: exact per-tip sizes, log2-binned.
    FoFFragmentHistogram h = paratreet::runFoFFragmentHistogram(fof, fof_node);
    CkPrintf("Fragment-size histogram:\n");
    for (int k = 0; k < 64; k++) {
      if (h.bins[k] != 0)
        CkPrintf("  size 2^%d..: %ld fragments\n", k, h.bins[k]);
    }
    CkPrintf("Max fragment size: %ld, total fragments: %ld\n",
             h.max_size, h.n_fragments);

    // On single-process runs phase 1 is the complete FoF, so the histogram
    // totals must match the serial reference exactly. (CkEnforce, not
    // CkAssert: this Charm build is CMK_OPTIMIZE, which compiles CkAssert
    // out, and these checks are the point of the test.)
    if (CkNumNodes() == 1) {
      CkEnforce(h.n_fragments == serial_n_components);
      CkEnforce(h.max_size == serial_max_size);
    }

    CkPrintf("FOF1 STEP1 PASSED\n");
  }

  void ExMain::postIterationFn(BoundingBox& universe, ProxyPack<FragData>& proxy_pack, int iter) {
  }

  Real ExMain::getTimestep(BoundingBox& universe, Real max_velocity) {
    return 0.01570796326;
  }
