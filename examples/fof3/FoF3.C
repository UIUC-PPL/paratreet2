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
    fof_b = fof_b_factor * std::cbrt(V / (double)N);
    CkPrintf("FoF linking length b = %g (factor %g, V = %g, N = %d)\n",
             fof_b, fof_b_factor, V, N);

    // Phase 1: register -> phaseA -> phaseB -> merge -> relabel. Tips are
    // process-level fragments afterwards. PBC (design/pbc.md): a cubic box
    // period of pbc_period on all axes (0 = open boundaries, the default).
    Vector3D<Real> pbc(pbc_period, pbc_period, pbc_period);
    if (pbc_period > 0) {
      // PBC validity (design/minimum-image-vs-27-images.md §8): fail fast at
      // startup rather than silently produce wrong FoF groups.
      Vector3D<Real> extent = universe.box.size();
      Real maxext = std::max(extent.x, std::max(extent.y, extent.z));
      // (1) period must be the box side, i.e. >= particle extent on every
      // axis. The universe box is slightly padded (remakeUniverse, ~2e-6), so
      // allow a small relative tolerance -- enough to accept period == true
      // box side, far tighter than any real mismatch (e.g. -P 1 on a box of
      // side 35).
      if (pbc_period < maxext * (Real)(1.0 - 1e-4)) {
        CkAbort("PBC: -P period %g is smaller than the particle box extent %g "
                "(x=%g y=%g z=%g). The period must be the box side (>= the "
                "extent on every axis); pass the true simulation box side.\n",
                (double)pbc_period, (double)maxext,
                (double)extent.x, (double)extent.y, (double)extent.z);
      }
      // (2) minimum-image requires b < L/2 (design/minimum-image §4).
      if (fof_b >= 0.5 * (double)pbc_period) {
        CkAbort("PBC: linking length b = %g is not < L/2 = %g (period L = %g). "
                "Minimum-image PBC requires b < L/2; lower -b or check -P.\n",
                fof_b, 0.5 * (double)pbc_period, (double)pbc_period);
      }
      CkPrintf("PBC enabled: period L = %g (box extent %g), b = %g < L/2 = %g\n",
               (double)pbc_period, (double)maxext, fof_b,
               0.5 * (double)pbc_period);
    }
    double t0 = CkWallTimer();
    paratreet::runFoFPhase1(proxy_pack.subtree, fof, fof_node, fof_b, pbc);
    double t1 = CkWallTimer();

    // Step 4 (-u dist; design/step4.md "Tip encoding"): renumber tips to
    // (owning_process << 40) | dense_index BEFORE upwardPass/loadCache, so
    // every particle copy the phase-3 walk later reads (including
    // cache-shipped remote copies) already carries the encoded value --
    // same class of ordering hazard as the loadCache/annotation-validity
    // fix above, and fixed the same way (do the rewrite before the ship).
    // countFragments must run first (builds frag_counts, the per-process
    // tip domain computeTipEncoding enumerates); the later fragment
    // histogram print in traversalFn reuses this same frag_counts via
    // runFoFFragmentHistogramNode (NOT runFoFFragmentHistogram, which would
    // re-invoke countFragments and double-count).
    double t_encode = 0.0;
    if (uf2_mode == UF2Mode::Dist) {
      double te0 = CkWallTimer();
      fof.countFragments(CkCallbackResumeThread());
      fof_node.computeTipEncoding(CkCallbackResumeThread());
      fof.applyTipEncoding(CkCallbackResumeThread());
      t_encode = CkWallTimer() - te0;
    }

    // Refresh the FragData annotations from the relabeled particles and let
    // canopy propagation (including the re-sends to Driver::recvTC) settle.
    proxy_pack.subtree.upwardPass(CkCallbackResumeThread());
    CkWaitQD();
    double t2 = CkWallTimer();

    // Only now ship the starter pack: every canopy annotation is valid.
    proxy_pack.driver.loadCache(CkCallbackResumeThread());
    double t3 = CkWallTimer();
    CkPrintf("FOF3STAT time_s: phase1 %.3f tip_encode %.3f upwardPass %.3f loadCache %.3f\n",
             t1 - t0, t_encode, t2 - t1, t3 - t2);
  }

  // Shared printer for the final component-count + histogram line: full and
  // stats mode MUST emit the identical format (and, for a given input, the
  // identical content — this line is the cross-run determinism observable in
  // stats mode, compared across configs/runs of the same input).
  static void printComponentsLine(long n_components, long max_size,
                                  const long* bins) {
    CkPrintf("FOF3STAT components: %ld max_size %ld log2_histogram:",
             n_components, max_size);
    for (int k = 0; k < 64; k++)
      if (bins[k] != 0) CkPrintf(" %d:%ld", k, bins[k]);
    CkPrintf("\n");
  }

  // Step 5 (design/step5-pruning.md): the min-component-size REPORTING line,
  // printed only when -m > 0. The unpruned components line (above) still
  // prints too, so both the full and the pruned counts are always visible.
  // This is a reporting filter: it does not relabel particles or touch the
  // validated partition/equality checks. Same log2 binning as the components
  // line, so the two histograms are directly comparable bin-for-bin.
  static void printSurvivingLine(long surviving_count, int min_size,
                                 long max_size, const long* bins) {
    CkPrintf("FOF3STAT surviving: %ld components with size >= %d, max_size %ld "
             "log2_histogram:", surviving_count, min_size, max_size);
    for (int k = 0; k < 64; k++)
      if (bins[k] != 0) CkPrintf(" %d:%ld", k, bins[k]);
    CkPrintf("\n");
  }

  // Serial O(n^2) reference FoF over the gathered records. Fills
  // serial_rep[i] with the canonical representative of record i's component:
  // the global order of the component's min-order member (union by min order
  // + full compression). Returns the component count.
  static long serialReference(const FoFParticleRecord* recs, int n, double b2,
                              const Vector3D<Real>& period,
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
    // Reproduce the library's distance arithmetic exactly (Vector3D<Real>);
    // periodicDistSq with period {0,0,0} is bit-identical to lengthSquared,
    // and applies minimum-image wraparound when -P is set (design/pbc.md).
    for (int i = 0; i < n; i++) {
      Vector3D<Real> pi(recs[i].x, recs[i].y, recs[i].z);
      for (int j = i + 1; j < n; j++) {
        Vector3D<Real> pj(recs[j].x, recs[j].y, recs[j].z);
        if (paratreet::periodicDistSq(pi, pj, period) <= b2) unite(i, j);
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
  // >= b: a pair within distance b differs by at most 1 cell per axis, so
  // union-find with pairwise distance tests across each cell's 27-neighborhood
  // (enumerated as self + 13 "forward" neighbor offsets, so each cell pair is
  // visited once) reproduces the exact linking graph. The distance predicate
  // replicates the library's float arithmetic (Real component subtraction,
  // Real sum of squares, promote to double for the <= b2 compare) exactly as
  // serialReference does, via paratreet::periodicDistSq.
  //
  // PBC (design/pbc.md): when period.<axis> > 0 the reference wraps that axis.
  // We use G = floor(L/b) cells of width w = L/G >= b along that axis (the
  // standard linked-cell-list-with-PBC trick: cells tile ONE period exactly,
  // so cell indices wrap modulo G and each cell has a well-defined opposite-
  // face neighbor). b < L/2 guarantees G >= 2. The distance test itself is
  // minimum-image (periodicDistSq with the period), so a pair straddling
  // opposite faces links iff its wrapped distance <= b. With period {0,0,0}
  // this is bit-identical to the open-boundary reference: inv = 1/b, no wrap,
  // and periodicDistSq == lengthSquared. Independence property: consumes only
  // the gathered position/order records. Cross-validated against the O(n^2)
  // reference at N <= 10k (see traversalFn).
  static long gridReference(const FoFParticleRecord* recs, int n, double b,
                            double b2, const Vector3D<Real>& period,
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
    auto close = [&](int i, int j) {
      Vector3D<Real> pi(recs[i].x, recs[i].y, recs[i].z);
      Vector3D<Real> pj(recs[j].x, recs[j].y, recs[j].z);
      return (double)paratreet::periodicDistSq(pi, pj, period) <= b2;
    };

    // Per-axis binning: open axes use cells of side b (inv = 1/b, no wrap,
    // exact open-boundary arithmetic); periodic axes use G = floor(L/b) cells
    // of width w = L/G (inv = 1/w) that wrap modulo G. G_[axis] == 0 flags an
    // open axis (no wrap / bounds-check only).
    const double Ls[3] = {(double)period.x, (double)period.y, (double)period.z};
    double inv[3];
    int64_t G[3];
    for (int a = 0; a < 3; a++) {
      if (Ls[a] > 0.0) {
        G[a] = (int64_t)std::floor(Ls[a] / b);
        CkEnforce(G[a] >= 2); // b < L/2 (design/pbc.md); else PBC ill-defined
        inv[a] = (double)G[a] / Ls[a]; // = 1 / (L/G)
      } else {
        G[a] = 0;
        inv[a] = 1.0 / b;
      }
    }

    // Pack (ix, iy, iz) into a u64 key, 21 bits per axis (cell count / axis
    // stays far below 2^21 at every target scale).
    double minx = 1e300, miny = 1e300, minz = 1e300;
    for (int i = 0; i < n; i++) {
      minx = std::min(minx, (double)recs[i].x);
      miny = std::min(miny, (double)recs[i].y);
      minz = std::min(minz, (double)recs[i].z);
    }
    const double mins[3] = {minx, miny, minz};
    const int64_t axis_max = (int64_t(1) << 21) - 1;
    // Cell index for value v on axis a: floor((v-min)*inv), wrapped mod G for
    // periodic axes (origin-agnostic: mod G tolerates any raw index).
    auto cellIdx = [&](double v, int a) -> int64_t {
      int64_t k = (int64_t)std::floor((v - mins[a]) * inv[a]);
      if (G[a] > 0) k = ((k % G[a]) + G[a]) % G[a];
      return k;
    };
    auto pack = [&](int64_t ix, int64_t iy, int64_t iz) {
      return (uint64_t(ix) << 42) | (uint64_t(iy) << 21) | uint64_t(iz);
    };
    std::vector<std::pair<uint64_t, int>> cells(n);
    for (int i = 0; i < n; i++) {
      int64_t ix = cellIdx((double)recs[i].x, 0);
      int64_t iy = cellIdx((double)recs[i].y, 1);
      int64_t iz = cellIdx((double)recs[i].z, 2);
      CkEnforce(ix >= 0 && ix <= axis_max && iy >= 0 && iy <= axis_max &&
                iz >= 0 && iz <= axis_max);
      cells[i] = {pack(ix, iy, iz), i};
    }
    std::sort(cells.begin(), cells.end());

    // Forward half of the 27-neighborhood: (0,0,1) and the 4 (0,1,*),
    // 9 (1,*,*) offsets — 13 total; the self cell handles (0,0,0). Offset
    // dedup is by direction, so it remains valid under wrapping (each
    // undirected cell pair is still reached once; wrapping only remaps which
    // index the offset lands on). A neighbor offset on a periodic axis wraps
    // mod G (so face cells neighbor opposite-face cells); on an open axis it
    // is bounds-checked as before.
    static const int fwd[13][3] = {
      {0,0,1}, {0,1,-1}, {0,1,0}, {0,1,1},
      {1,-1,-1}, {1,-1,0}, {1,-1,1}, {1,0,-1}, {1,0,0}, {1,0,1},
      {1,1,-1}, {1,1,0}, {1,1,1}};
    auto key_lower = [&](uint64_t key) {
      return std::lower_bound(cells.begin(), cells.end(),
                              std::make_pair(key, -1));
    };
    // Apply a neighbor offset on one axis: wrap mod G (periodic) or return
    // the raw index / -1 if out of the open-boundary range.
    auto neighborIdx = [&](int64_t idx, int off, int a) -> int64_t {
      int64_t j = idx + off;
      if (G[a] > 0) return ((j % G[a]) + G[a]) % G[a];
      if (j < 0 || j > axis_max) return -1;
      return j;
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
        int64_t jx = neighborIdx(ix, d[0], 0);
        int64_t jy = neighborIdx(iy, d[1], 1);
        int64_t jz = neighborIdx(iz, d[2], 2);
        if (jx < 0 || jy < 0 || jz < 0) continue; // open-axis out of range
        uint64_t nkey = pack(jx, jy, jz);
        if (nkey == key) continue; // self (possible only if an axis wrapped
                                   // back onto itself); within-cell handled it
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
    CkEnforce(std::fabs(b - fof_b_factor *
                            std::cbrt(universe.box.volume() / (double)N))
              <= 1e-12 * b);

    // Resolve the correctness-check mode (-c; see README "Testing on a
    // cluster"). full = gather-to-PE-0 + serial grid reference (exact
    // partition comparison); stats = no gather, no reference — distributed
    // checks (tip sentinel, annotation-validity CkEnforce) stay on, and the
    // component count + histogram line is the cross-run determinism check.
    bool do_full;
    const char* mode_name;
    switch (check_mode) {
      case CheckMode::Full:  do_full = true;  mode_name = "full";  break;
      case CheckMode::Stats: do_full = false; mode_name = "stats"; break;
      default:
        do_full = (N <= kAutoFullMaxN);
        mode_name = do_full ? "auto(full)" : "auto(stats)";
        break;
    }

    // Self-describing config line: every FOF3STAT block starts with this.
    // pbc = the cubic box period L (0 = open boundaries; design/pbc.md).
    CkPrintf("FOF3STAT config: pes %d nodes %d N %d b %.12g b_factor %g "
             "decomp %s tree %s leafsize %d mode %s pbc %g iter %d\n",
             CkNumPes(), CkNumNodes(), N, b, fof_b_factor,
             paratreet::asString(conf.decomp_type).c_str(),
             paratreet::asString(conf.tree_type).c_str(),
             conf.max_particles_per_leaf, mode_name, pbc_period, iter);
    if (check_mode == CheckMode::Auto && !do_full) {
      CkPrintf("FOF3STAT warning: N = %d > %d, full verification SKIPPED "
               "(auto mode fell back to stats); force with -c full, memory "
               "permitting (gather is ~24 bytes/particle to PE 0)\n",
               N, kAutoFullMaxN);
    }

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
    // must run before the phase-3 relabel overwrites the tips. Distributed
    // (reduction-based), so it runs in both check modes. Dist mode already
    // ran countFragments in preTraversalFn (before tip encoding), so it
    // reads frag_counts back directly rather than re-invoking countFragments
    // (which would double-count against encoded tips -- see preTraversalFn).
    {
      auto h = uf2_mode == UF2Mode::Dist
                   ? paratreet::runFoFFragmentHistogramNode(fof_node)
                   : paratreet::runFoFFragmentHistogram(fof, fof_node);
      CkPrintf("FOF3STAT fragments: %ld max_size %ld log2_histogram:",
               h.n_fragments, h.max_size);
      for (int k = 0; k < 64; k++)
        if (h.bins[k] != 0) CkPrintf(" %d:%ld", k, h.bins[k]);
      CkPrintf("\n");
    }

    // Pre-walk soundness check for the phase-3 edge predicate. Serial mode:
    // the tip sentinel (every registered particle's tip is a global particle
    // order in [0, N); phase 1 writes every registered particle, so an
    // out-of-range value means the walk would read copies phase 1 never
    // touched). Dist mode: the owner-encoded-tip invariant instead (tips are
    // no longer in [0, N) after preTraversalFn's encoding step; see
    // FoFPhase1::verifyEncodedTips). Both distributed (barrier reduction),
    // so they stay on in BOTH check modes. (Target-side sharing is asserted
    // separately by Partition::verifySharedLeaves inside runFoFPhase3[Dist].)
    double tg0 = CkWallTimer();
    if (uf2_mode == UF2Mode::Dist) paratreet::runFoFVerifyEncodedTips(fof);
    else                           paratreet::runFoFVerifyTips(fof, (long)N);
    double tg1 = CkWallTimer();
    CkPrintf("FOF3STAT time_s: fragcheck %.3f tip_sentinel %.3f\n",
             tc1 - tc0, tg1 - tg0);

    // Phase 3: cross-process boundary walk + UF_2 (dist: UnionFindLib per
    // design/step4.md; serial: v1/3a gather-to-one) + global relabel.
    Vector3D<Real> pbc(pbc_period, pbc_period, pbc_period);
    FoFPhase3Result pr = uf2_mode == UF2Mode::Dist
        ? paratreet::runFoFPhase3Dist(proxy_pack.partition, fof, fof_node, b, pbc)
        : paratreet::runFoFPhase3(proxy_pack.partition, fof, b, pbc);
    CkPrintf("FOF3STAT edges: emitted %ld sent %ld unique %ld tips_remapped %ld\n",
             pr.edges_emitted, pr.edges_sent, pr.edges_unique, pr.tips_remapped);
    // 3a counters (design/step3.md §6). Redundancy ratio = both-uniform
    // descents / unique cross-process pairs (the §8.3/8.4 go/no-go data for
    // 3b's parking). Peak active node pairs is not tracked (the traverser
    // does not expose pair activation cheaply); the edge-buffer high-water
    // mark is the reported memory figure.
    CkPrintf("FOF3STAT prunes: negative %ld positive %ld suppression %ld "
             "same_frag %ld leaf_visits %ld\n",
             pr.negative_prunes, pr.positive_prunes, pr.suppression_prunes,
             pr.same_frag_prunes, pr.leaf_visits);
    CkPrintf("FOF3STAT redundancy: both_uniform_descents %ld unique_pairs %ld "
             "ratio %.3f peak_edge_buf %ld\n",
             pr.redundant_descents, pr.edges_sent,
             pr.edges_sent > 0 ? (double)pr.redundant_descents / pr.edges_sent
                               : 0.0,
             pr.peak_edge_buf);
    CkPrintf("FOF3STAT time_s: phase3_walk %.3f edge_gather %.3f uf2 %.3f "
             "relabel %.3f\n",
             pr.t_walk, pr.t_gather, pr.t_uf2, pr.t_relabel);
    // Per-PE load-imbalance signals (min/avg/max over PEs), from the
    // phase3Stats tuple reduction: phase-1 entry-body times and the walk's
    // per-PE work distribution. max/avg >> 1 = imbalance.
    {
      double n_pes = (double)CkNumPes();
      CkPrintf("FOF3STAT balance: phaseA_s %.3f/%.3f/%.3f "
               "phaseB_s %.3f/%.3f/%.3f (min/avg/max over %d PEs)\n",
               pr.t_phaseA_min, pr.t_phaseA_avg, pr.t_phaseA_max,
               pr.t_phaseB_min, pr.t_phaseB_avg, pr.t_phaseB_max, CkNumPes());
      CkPrintf("FOF3STAT balance: leaf_visits %ld/%.1f/%ld "
               "edges_emitted %ld/%.1f/%ld (min/avg/max over %d PEs)\n",
               pr.leaf_visits_min, pr.leaf_visits / n_pes, pr.leaf_visits_max,
               pr.emitted_min, pr.edges_emitted / n_pes, pr.emitted_max,
               CkNumPes());
    }

    if (!do_full) {
      // Stats mode: no particle gather, no serial reference. The component
      // count + size histogram is still exact — computed from a distributed
      // (label, count) gather (see FoFPhase1::collectLabelCounts) — and is
      // the determinism observable: run the same input under two different
      // process/PE configs and diff the FOF3STAT components lines.
      double th0 = CkWallTimer();
      auto h = paratreet::runFoFComponentHistogram(fof, fof_min_component_size);
      double th1 = CkWallTimer();
      printComponentsLine(h.n_components, h.max_size, h.bins);
      if (fof_min_component_size > 0)
        printSurvivingLine(h.surviving_count, fof_min_component_size,
                           h.surviving_max_size, h.surviving_bins);
      CkPrintf("FOF3STAT time_s: component_histogram %.3f\n", th1 - th0);
      CkPrintf("FOF3 STATS MODE COMPLETE: %ld components "
               "(full verification not run; determinism check = compare "
               "FOF3STAT components lines across configs)\n", h.n_components);
      return;
    }

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
    long serial_components = gridReference(recs, N, b, b * b, pbc, serial_rep);
    double tv2 = CkWallTimer();
    if (N <= 10000) {
      std::vector<long> n2_rep;
      long n2_components = serialReference(recs, N, b * b, pbc, n2_rep);
      CkEnforce(n2_components == serial_components);
      for (int i = 0; i < N; i++) CkEnforce(n2_rep[i] == serial_rep[i]);
      CkPrintf("FOF3STAT crosscheck: grid reference == O(n^2) reference, "
               "%ld components\n", serial_components);
    }
    double tv3 = CkWallTimer();
    CkPrintf("FOF3STAT time_s: final_gather %.3f grid_reference %.3f "
             "n2_crosscheck %.3f\n",
             tv1 - tv0, tv2 - tv1, tv3 - tv2);

    // Label-agnostic canonicalization (design/step4.md decision 3): -u dist
    // final labels are UnionFindLib's arbitrary serial componentNumbers, not
    // the "order of the min-order member" convention -u serial's gather-to-
    // one UF_2 produces, so raw label VALUES are not comparable to
    // serial_rep across modes. Re-derive, from the gathered records
    // themselves, the canonical representative per label group (min particle
    // order among all records sharing that label) exactly as fof1's
    // verifyPhase1 already does, and compare THAT to serial_rep -- correct
    // and unchanged in behavior for -u serial (there, tip_min_order[label]
    // trivially equals label itself, since union-by-min already made the
    // label the min order of its own group).
    std::unordered_map<long, long> tip_min_order; // final label -> min order
    for (int i = 0; i < N; i++) {
      auto it = tip_min_order.find(recs[i].tip);
      if (it == tip_min_order.end()) tip_min_order.emplace(recs[i].tip, recs[i].order);
      else if (recs[i].order < it->second) it->second = recs[i].order;
    }

    int n_mismatch = 0;
    std::unordered_map<long, long> tips_seen; // final label -> count
    for (int i = 0; i < N; i++) {
      tips_seen[recs[i].tip]++;
      long canon_rep = tip_min_order[recs[i].tip];
      if (canon_rep != serial_rep[i]) {
        if (n_mismatch < 5) {
          CkPrintf("FOF3 MISMATCH: particle order %d at (%g, %g, %g): "
                   "serial rep %ld, phase-3 canonical rep %ld (label %ld)\n",
                   recs[i].order, (double)recs[i].x, (double)recs[i].y,
                   (double)recs[i].z, serial_rep[i], canon_rep, recs[i].tip);
        }
        n_mismatch++;
      }
    }
    long K = (long)tips_seen.size();
    // Final (global) component-size histogram: log2 bins over the label
    // counts. Input-determined, so it must be identical across every config
    // of the same input (the determinism cross-check; stats mode prints the
    // same line from a distributed gather); its tail is the giant-component
    // diagnostic on near-percolation inputs.
    {
      long bins[64] = {0};
      long max_size = 0;
      // Step 5 (design/step5-pruning.md): the surviving (size >= m) tallies are
      // computed from the SAME tips_seen label-count map, so no extra gather.
      long surv_bins[64] = {0};
      long surv_max_size = 0;
      long surv_count = 0;
      for (auto& kv : tips_seen) {
        long size = kv.second;
        int bin = 0;
        while (bin < 63 && (2L << bin) <= size) bin++;
        bins[bin]++;
        if (size > max_size) max_size = size;
        if (size >= (long)fof_min_component_size) {
          surv_bins[bin]++;
          surv_count++;
          if (size > surv_max_size) surv_max_size = size;
        }
      }
      printComponentsLine(K, max_size, bins);
      if (fof_min_component_size > 0)
        printSurvivingLine(surv_count, fof_min_component_size, surv_max_size,
                           surv_bins);
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
    // End-of-iteration memory footprint, min/avg/max over PEs (tuple
    // reduction on the FoFPhase1 branches; CmiMemoryUsage). Limitation: in
    // SMP builds CmiMemoryUsage reports PROCESS-wide allocation, so every PE
    // of a process contributes the same value and min/avg/max are effectively
    // over processes; it is current (not peak) usage, so the walk-time or
    // gather-time high-water mark can exceed it.
    auto ms = paratreet::runFoFMemoryStats(fof);
    CkPrintf("FOF3STAT memory_MB: %.1f/%.1f/%.1f (min/avg/max over %d PEs; "
             "CmiMemoryUsage, process-wide under SMP)\n",
             ms.min_bytes / 1e6, ms.avg_bytes / 1e6, ms.max_bytes / 1e6,
             CkNumPes());
  }

  Real ExMain::getTimestep(BoundingBox& universe, Real max_velocity) {
    return 0.01570796326;
  }
