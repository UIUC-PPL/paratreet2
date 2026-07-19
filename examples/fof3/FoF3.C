#include "Main.h"
#include "Paratreet.h"
#include "FoFPhase1.h"
#include "FoFPhase3.h"
#include "FragCheckVisitor.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <vector>

using namespace paratreet;

  // THE ORDERING FIX (design/step3.md §2): phase 1 + upwardPass must
  // complete BEFORE Driver::loadCache ships the starter pack, because the
  // 3a visitor reads min_frag/max_frag on cache-shipped internal nodes
  // (CachedBoundary canopies). Driver::run calls preTraversalFn inline on
  // its threaded entry after the tree build QD, so the whole sequence can
  // run here; loadCache then ships post-phase-1 annotations (Driver's
  // sortStorage keeps the newest canopy generation per key). The
  // annotation-validity CkEnforce in FoFEdgeVisitor trips if this ordering
  // regresses.
  void ExMain::preTraversalFn(ProxyPack<FragData>& proxy_pack) {
    // Linking length from the universe box: b = 0.2 * (V/N)^(1/3). The
    // universe is not passed to preTraversalFn; read it from the
    // ThreadStateHolder branch on this PE (set during decomposition, same
    // value Driver::run later passes to traversalFn).
    const BoundingBox& universe = thread_state_holder.ckLocalBranch()->universe;
    double V = universe.box.volume();
    int N = universe.n_particles;
    fof_b = 0.2 * std::cbrt(V / (double)N);
    CkPrintf("FoF linking length b = %g (V = %g, N = %d)\n", fof_b, V, N);

    // Phase 1: register -> phaseA -> phaseB -> merge -> relabel. Tips are
    // process-level fragments afterwards.
    double t0 = CkWallTimer();
    paratreet::runFoFPhase1(proxy_pack.subtree, fof, fof_node, fof_b);
    double t1 = CkWallTimer();

    // Refresh the FragData annotations from the relabeled particles and let
    // canopy propagation (including the re-sends to Driver::recvTC) settle.
    proxy_pack.subtree.upwardPass(CkCallbackResumeThread());
    CkWaitQD();
    double t2 = CkWallTimer();

    // Only now ship the starter pack: every canopy annotation is valid.
    proxy_pack.driver.loadCache(CkCallbackResumeThread());
    double t3 = CkWallTimer();
    CkPrintf("FOF3 TIME phase1 %.3f s, upwardPass %.3f s, loadCache %.3f s\n",
             t1 - t0, t2 - t1, t3 - t2);
  }

  // Serial O(n^2) reference FoF over the gathered records. Fills
  // serial_rep[i] with the canonical representative of record i's component:
  // the global order of the component's min-order member (union by min order
  // + full compression). Returns the component count.
  static long serialReference(const FoFParticleRecord* recs, int n, double b2,
                              std::vector<long>& serial_rep) {
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
    serial_rep.resize(n);
    long n_components = 0;
    for (int i = 0; i < n; i++) {
      int r = find(i);
      serial_rep[i] = recs[r].order;
      if (r == i) n_components++;
    }
    return n_components;
  }

  // Exact grid-hash serial reference FoF, O(n) at fixed density (the O(n^2)
  // reference above is quadratic and unusable past ~10k). Cells of side
  // exactly b: a pair within distance b differs by at most 1 cell per axis,
  // so union-find with pairwise distance tests across each cell's
  // 27-neighborhood (enumerated as self + 13 "forward" neighbor offsets, so
  // each cell pair is visited once) reproduces the exact linking graph. The
  // distance predicate replicates the library's float arithmetic (Real
  // component subtraction, Real sum of squares, promote to double for the
  // <= b2 compare) exactly as serialReference does via Vector3D<Real>.
  // Independence property: consumes only the gathered position/order
  // records — no library geometry, tree, or hash code. Cross-validated
  // against the O(n^2) reference at N <= 10k (see traversalFn).
  static long gridReference(const FoFParticleRecord* recs, int n, double b,
                            double b2, std::vector<long>& serial_rep) {
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
    auto close = [&](int i, int j) {
      Real dx = recs[i].x - recs[j].x;
      Real dy = recs[i].y - recs[j].y;
      Real dz = recs[i].z - recs[j].z;
      Real d2 = dx * dx + dy * dy + dz * dz;
      return (double)d2 <= b2;
    };

    // Bin into cells of side b; pack (ix, iy, iz) into a u64 key, 21 bits
    // per axis (box extent / b stays far below 2^21 at every target scale).
    double minx = 1e300, miny = 1e300, minz = 1e300;
    for (int i = 0; i < n; i++) {
      minx = std::min(minx, (double)recs[i].x);
      miny = std::min(miny, (double)recs[i].y);
      minz = std::min(minz, (double)recs[i].z);
    }
    const double inv = 1.0 / b;
    const int64_t axis_max = (int64_t(1) << 21) - 1;
    auto pack = [&](int64_t ix, int64_t iy, int64_t iz) {
      return (uint64_t(ix) << 42) | (uint64_t(iy) << 21) | uint64_t(iz);
    };
    std::vector<std::pair<uint64_t, int>> cells(n);
    for (int i = 0; i < n; i++) {
      int64_t ix = (int64_t)std::floor(((double)recs[i].x - minx) * inv);
      int64_t iy = (int64_t)std::floor(((double)recs[i].y - miny) * inv);
      int64_t iz = (int64_t)std::floor(((double)recs[i].z - minz) * inv);
      CkEnforce(ix >= 0 && ix <= axis_max && iy >= 0 && iy <= axis_max &&
                iz >= 0 && iz <= axis_max);
      cells[i] = {pack(ix, iy, iz), i};
    }
    std::sort(cells.begin(), cells.end());

    // Forward half of the 27-neighborhood: (0,0,1) and the 4 (0,1,*),
    // 9 (1,*,*) offsets — 13 total; the self cell handles (0,0,0).
    static const int fwd[13][3] = {
      {0,0,1}, {0,1,-1}, {0,1,0}, {0,1,1},
      {1,-1,-1}, {1,-1,0}, {1,-1,1}, {1,0,-1}, {1,0,0}, {1,0,1},
      {1,1,-1}, {1,1,0}, {1,1,1}};
    auto key_lower = [&](uint64_t key) {
      return std::lower_bound(cells.begin(), cells.end(),
                              std::make_pair(key, -1));
    };
    for (size_t s = 0; s < cells.size();) {
      uint64_t key = cells[s].first;
      size_t e = s + 1;
      while (e < cells.size() && cells[e].first == key) e++;
      // Within-cell pairs.
      for (size_t a = s; a < e; a++)
        for (size_t c = a + 1; c < e; c++)
          if (close(cells[a].second, cells[c].second))
            unite(cells[a].second, cells[c].second);
      // Cross pairs against each forward neighbor cell.
      int64_t ix = int64_t(key >> 42);
      int64_t iy = int64_t((key >> 21) & axis_max);
      int64_t iz = int64_t(key & axis_max);
      for (auto& d : fwd) {
        int64_t jx = ix + d[0], jy = iy + d[1], jz = iz + d[2];
        if (jx < 0 || jy < 0 || jz < 0 ||
            jx > axis_max || jy > axis_max || jz > axis_max) continue;
        uint64_t nkey = pack(jx, jy, jz);
        for (auto it = key_lower(nkey);
             it != cells.end() && it->first == nkey; ++it)
          for (size_t a = s; a < e; a++)
            if (close(cells[a].second, it->second))
              unite(cells[a].second, it->second);
      }
      s = e;
    }

    serial_rep.resize(n);
    long n_components = 0;
    for (int i = 0; i < n; i++) {
      int r = find(i);
      serial_rep[i] = recs[r].order;
      if (r == i) n_components++;
    }
    return n_components;
  }

  // Gather (position, tip, order) for every registered particle; caller owns
  // the returned message (records = msg->getData()).
  static CkReductionMsg* gatherRecords(CProxy_FoFPhase1<FragData> fof, int N,
                                       const char* what) {
    void* result = nullptr;
    fof.collect(CkCallbackResumeThread(result));
    CkReductionMsg* msg = (CkReductionMsg*)result;
    int n = msg->getSize() / sizeof(FoFParticleRecord);
    if (n != N) {
      CkPrintf("FOF3 TEST FAILED: %s gathered %d records, expected %d\n",
               what, n, N);
      CkAbort("FOF3 TEST FAILED");
    }
    return msg;
  }

  void ExMain::traversalFn(BoundingBox& universe, ProxyPack<FragData>& proxy_pack, int iter) {
    // Phase 1 + upwardPass already ran in preTraversalFn (before loadCache;
    // see the ordering note there). Sanity-check that the linking length
    // computed there matches this universe.
    int N = universe.n_particles;
    double b = fof_b;
    CkEnforce(b > 0.0);
    CkEnforce(std::fabs(b - 0.2 * std::cbrt(universe.box.volume() / (double)N))
              <= 1e-12 * b);

    // Leaf-annotation assertions over the full tree (remote sources ship
    // through the CacheManager: first true multi-process exercise of
    // upwardPass canopy propagation + FragData cache shipping). The QD also
    // guarantees all asserts ran.
    // The check traversal opens every node for every target leaf, so it is
    // quadratic scaffolding (measured: 262 s at 1M +p1 vs 2 s for the real
    // walk; hours at 8M). Gate it at 100k: the full assertion sweep stays on
    // for the whole established matrix plus the 100k runs, and above that
    // the annotation-validity CkEnforce inside FoFEdgeVisitor (on every node
    // the phase-3 walk actually consults) remains the tripwire. It has also
    // passed ungated at 1M (+p1, +p2, 2026-07-19).
    double tc0 = CkWallTimer();
    if (N <= 100000) {
      proxy_pack.partition.template startDown<FragCheckVisitor>(FragCheckVisitor());
      CkWaitQD();
    }
    double tc1 = CkWallTimer();

    // Phase-1 fragment (process-tip) histogram — design note §6.3e data;
    // must run before the phase-3 relabel overwrites the tips.
    {
      auto h = paratreet::runFoFFragmentHistogram(fof, fof_node);
      CkPrintf("Phase 1 fragments: %ld, max size %ld; log2 histogram:",
               h.n_fragments, h.max_size);
      for (int k = 0; k < 64; k++)
        if (h.bins[k] != 0) CkPrintf(" %d:%ld", k, h.bins[k]);
      CkPrintf("\n");
    }

    // Pre-walk soundness check for the phase-3 edge predicate: every
    // registered (Subtree-owned) particle must hold a valid tip, i.e. a
    // global particle order in [0, N). Phase 1 writes every registered
    // particle, so an out-of-range value would mean the walk reads copies
    // phase 1 never touched. (Target-side sharing is asserted separately by
    // Partition::verifySharedLeaves inside runFoFPhase3.)
    double tg0 = CkWallTimer();
    {
      CkReductionMsg* msg = gatherRecords(fof, N, "pre-walk");
      const auto* recs = (const FoFParticleRecord*)msg->getData();
      for (int i = 0; i < N; i++) {
        CkEnforce(recs[i].tip >= 0 && recs[i].tip < N);
      }
      delete msg;
    }
    double tg1 = CkWallTimer();
    CkPrintf("FOF3 TIME fragcheck %.3f s, pre-walk gather %.3f s\n",
             tc1 - tc0, tg1 - tg0);

    // Phase 3: cross-process boundary walk + gather-to-one UF_2 + global
    // relabel (see src/FoFPhase3.h).
    FoFPhase3Result pr = paratreet::runFoFPhase3(proxy_pack.partition, fof, b);
    CkPrintf("Phase 3 edges: emitted %ld, sent %ld (per-PE dedup), "
             "unique %ld (root dedup), tips remapped %ld\n",
             pr.edges_emitted, pr.edges_sent, pr.edges_unique, pr.tips_remapped);
    // 3a counters (design/step3.md §6). Redundancy ratio = both-uniform
    // descents / unique cross-process pairs (the §8.3/8.4 go/no-go data for
    // 3b's parking). Peak active node pairs is not tracked (the traverser
    // does not expose pair activation cheaply); the edge-buffer high-water
    // mark is the reported memory figure.
    CkPrintf("Phase 3a prunes: negative %ld, positive %ld, suppression %ld, "
             "same-frag %ld; leaf visits %ld\n",
             pr.negative_prunes, pr.positive_prunes, pr.suppression_prunes,
             pr.same_frag_prunes, pr.leaf_visits);
    CkPrintf("Phase 3a redundancy: %ld both-uniform descents / %ld unique "
             "pairs = %.3f; peak edge buffer per PE = %ld\n",
             pr.redundant_descents, pr.edges_unique,
             pr.edges_unique > 0 ? (double)pr.redundant_descents / pr.edges_unique
                                 : 0.0,
             pr.peak_edge_buf);
    CkPrintf("FOF3 TIME phase3 walk %.3f s, edge gather %.3f s, UF_2 %.3f s, "
             "relabel %.3f s\n",
             pr.t_walk, pr.t_gather, pr.t_uf2, pr.t_relabel);

    // End-to-end check: after UF_2 relabel the labels are global, so the
    // comparison against the serial O(n^2) reference is valid on any number
    // of processes. Union-by-min everywhere makes the final group_number the
    // global order of the component's min-order member -- the same canonical
    // representative the serial reference computes -- so compare directly.
    double tv0 = CkWallTimer();
    CkReductionMsg* msg = gatherRecords(fof, N, "final");
    double tv1 = CkWallTimer();
    const auto* recs = (const FoFParticleRecord*)msg->getData();
    // Reference partition: exact grid-hash FoF (O(n) at fixed density). At
    // N <= 10k the O(n^2) reference also runs and the two partitions must
    // be identical (cross-validation of the grid reference; above 10k only
    // the grid runs — the O(n^2) path stays available behind this size
    // threshold).
    std::vector<long> serial_rep;
    long serial_components = gridReference(recs, N, b, b * b, serial_rep);
    double tv2 = CkWallTimer();
    if (N <= 10000) {
      std::vector<long> n2_rep;
      long n2_components = serialReference(recs, N, b * b, n2_rep);
      CkEnforce(n2_components == serial_components);
      for (int i = 0; i < N; i++) CkEnforce(n2_rep[i] == serial_rep[i]);
      CkPrintf("FOF3 grid reference cross-validated against O(n^2): "
               "identical partition, %ld components\n", serial_components);
    }
    double tv3 = CkWallTimer();
    CkPrintf("FOF3 TIME final gather %.3f s, grid reference %.3f s"
             " (O(n^2) cross-check %.3f s)\n",
             tv1 - tv0, tv2 - tv1, tv3 - tv2);

    int n_mismatch = 0;
    std::unordered_map<long, long> tips_seen; // final label -> count
    for (int i = 0; i < N; i++) {
      tips_seen[recs[i].tip]++;
      if (recs[i].tip != serial_rep[i]) {
        if (n_mismatch < 5) {
          CkPrintf("FOF3 MISMATCH: particle order %d at (%g, %g, %g): "
                   "serial rep %ld, phase-3 label %ld\n",
                   recs[i].order, (double)recs[i].x, (double)recs[i].y,
                   (double)recs[i].z, serial_rep[i], recs[i].tip);
        }
        n_mismatch++;
      }
    }
    long K = (long)tips_seen.size();
    // Final (global) component-size histogram: log2 bins over the label
    // counts. Input-determined, so it must be identical across every config
    // of the same input (the determinism cross-check); its tail is the
    // giant-component diagnostic on near-percolation inputs.
    {
      long bins[64] = {0};
      long max_size = 0;
      for (auto& kv : tips_seen) {
        long size = kv.second;
        int bin = 0;
        while (bin < 63 && (2L << bin) <= size) bin++;
        bins[bin]++;
        if (size > max_size) max_size = size;
      }
      CkPrintf("Final components: %ld, max size %ld; log2 histogram:",
               K, max_size);
      for (int k = 0; k < 64; k++)
        if (bins[k] != 0) CkPrintf(" %d:%ld", k, bins[k]);
      CkPrintf("\n");
    }
    delete msg;

    if (n_mismatch != 0 || K != serial_components) {
      CkPrintf("FOF3 TEST FAILED: %d of %d particles mismatched "
               "(serial: %ld components, phase-3: %ld labels)\n",
               n_mismatch, N, serial_components, K);
      CkAbort("FOF3 TEST FAILED");
    }
    CkPrintf("FOF3 TEST PASSED: %ld components\n", K);
  }

  void ExMain::postIterationFn(BoundingBox& universe, ProxyPack<FragData>& proxy_pack, int iter) {
  }

  Real ExMain::getTimestep(BoundingBox& universe, Real max_velocity) {
    return 0.01570796326;
  }
