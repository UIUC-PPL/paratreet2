#ifndef PARATREET_FOFPHASE3_H_
#define PARATREET_FOFPHASE3_H_

// FoF phase 3 v1: cross-process boundary walk + gather-to-one UF_2
// (design/phase3.md). After phase 1 froze process-level tips in
// Particle::group_number and Subtree::upwardPass refreshed the FragData
// annotations, this phase discovers merge-graph edges between fragments on
// different processes and applies the resulting global relabeling.
//
// Walk mechanism: the standard Partition traversal (startDown ->
// TransposedDownTraverser), i.e. every Partition's target leaves are walked
// against the full global tree; remote subtrees are fetched and resumed
// through the CacheManager/Resumer machinery exactly as in the gravity
// examples. FoFEdgeVisitor (3a, design/step3.md §3) prunes on three
// certificates: case 1 (negative, mindist(source box, target box) > b),
// case 2 (positive, both sides uniform and maxdist <= b: emit without
// descending), and case 3 (suppression: the process-level SEEN table on
// FoFPhase1Node already holds the (g, f) pair). Leaf x leaf pairs emit one
// edge per (g, f) per process, gated by the same SEEN table (first witness
// wins). Because open() now reads min_frag/max_frag on cache-shipped
// internal nodes, the app MUST sequence phase 1 + upwardPass BEFORE
// Driver::loadCache ships the starter pack (see examples/fof3); the
// annotation-validity CkEnforce in the visitor trips otherwise.
//
// Correctness of the edge predicate (design discussion, 2026-07-18): after
// phase 1, any two particles within b of each other that hold DIFFERENT tips
// are necessarily owned by different processes (phase 1 is the complete FoF
// restricted to a process), so no ownership test is needed: different-tip
// pairs within b are exactly the merge edges. Same-tip pairs are skipped.
// The same pair is discovered from both sides' walks; duplicates are
// deduplicated per PE and again at the gather root.
//
// Preconditions (asserted by runFoFPhase3):
//  - Subtree and Partition decompositions match (the FoF configuration,
//    -d oct with the oct tree): target leaves then alias the Subtree-owned
//    particle blocks that phase 1 relabeled (Partition::verifySharedLeaves
//    checks pointer identity). Source leaves fetched through the cache carry
//    relabeled copies because CacheManager::serviceRequest copies live
//    particle data at request time, after the phase-1 relabel.
//  - Call from a [threaded] context between runFoFPhase1 + upwardPass and
//    the next tree rebuild/reset.
//
// Completion detection: CkWaitQD over the walk (all walk work, including
// remote fetches and resumptions, is entry-method-driven; no external
// aggregation layer exists in v1). The edge gather uses a concat reduction
// (the reduction is the completion detection), the tip -> globalRoot map is
// broadcast via a marshalled entry, and the relabel completes through a
// reduction callback. See design/phase3.md "Deliberately deferred" for what
// step 3/4 replace here.
//
// Note: phase-1's runFoFPhase1 is templated on Data; phase 3 v1 is pinned to
// FragData because the FoF payload is FragData (and charmxi extern-entry
// instantiation of a templated visitor of a templated chare is avoidable
// complexity). Generalize when a second payload needs it.

#include "paratreet.decl.h"
#include "common.h"
#include "Configuration.h"
#include "FoFData.h"
#include "FoFPhase1.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Visitor for the phase-3 boundary walk (Partition::startDown). Carries
// state (the per-PE FoFPhase1 branch proxy and b^2); visitors are pupped
// into the startDown broadcast, so both members are pupped (same pattern as
// the old application's FoFVisitor).
struct FoFEdgeVisitor {
public:
  // Self-leaf pairs live inside one Subtree (one PE), so phase 1 already
  // unioned them: they can never produce a cross-process edge. Skip them.
  static constexpr const bool CallSelfLeaf = false;

  CProxy_FoFPhase1<FragData> fof;
  double b2 = 0.0;

  FoFEdgeVisitor() {}
  FoFEdgeVisitor(CProxy_FoFPhase1<FragData> fof_, double b2_) : fof(fof_), b2(b2_) {}

  void pup(PUP::er& p) {
    p | fof;
    p | b2;
  }

public:
  // Annotation validity (design/step3.md §2): every node whose
  // min_frag/max_frag the visitor consults must carry a post-phase-1
  // annotation. Build-time annotations are min = max = -1 (group_number is
  // initialized to -1), so min_frag >= 0 catches cache entries shipped
  // before phase 1 + upwardPass (the loadCache-ordering hazard); empty
  // nodes keep the identity values (min > max) and are exempted via
  // n_particles == 0. CkEnforce, not CkAssert: CMK_OPTIMIZE builds compile
  // CkAssert out, and this tripwire is the point.
  static void enforceValidAnnotation(const SpatialNode<FragData>& n) {
    CkEnforce(n.n_particles == 0 ||
              (n.data.min_frag <= n.data.max_frag && n.data.min_frag >= 0));
  }

  // The 3a state machine (design/step3.md §3). States per (g, f) fragment
  // pair: UNSEEN (absent from the process-level table) | SEEN. Both-uniform
  // node pairs consult the table; case 2 (positive certificate: every
  // point pair within b) and leaf witnesses insert-and-emit exactly once
  // (single winner under the table's mutex). Non-uniform pairs descend
  // under case-1 pruning as in v1; uniformity is hereditary, so every
  // descent reaches uniform territory or leaves.
  bool open(const SpatialNode<FragData>& source, SpatialNode<FragData>& target) {
    enforceValidAnnotation(source);
    enforceValidAnnotation(target);
    auto* branch = fof.ckLocalBranch();
    // uniform() is false for empty nodes (identity min > max), so
    // both_uniform implies both sides non-empty; the explicit n_particles
    // checks below are belt-and-braces for the case-2 certificate.
    const bool both_uniform = source.data.uniform() && target.data.uniform();
    if (both_uniform) {
      long g = source.data.min_frag; // remote/source tip
      long f = target.data.min_frag; // local/target tip
      if (g == f) {
        // Same tip on both sides: tips are process-local, so this is an
        // intra-process pair phase 1 already unioned; no edge can arise.
        branch->p3_same_frag_prunes++;
        return false;
      }
      uint64_t key = paratreet::packTipPair(g, f);
      if (branch->seenContains(key)) {         // case 3: suppression
        branch->p3_suppression_prunes++;
        return false;
      }
      if (source.n_particles != 0 && target.n_particles != 0 &&
          paratreet::maxdist2(source.data.box, target.data.box) <= b2) {
        // Case 2: positive certificate — every particle pair is within b,
        // and both sides are non-empty, so the edge (g, f) exists. Emit if
        // we won the insertion race; prune either way.
        if (branch->trySeenInsert(key)) branch->addPhase3Edge(g, f);
        branch->p3_positive_prunes++;
        return false;
      }
      // fall through to the case-1 check
    }
    if (paratreet::mindist2(source.data.box, target.data.box) > b2) {
      branch->p3_negative_prunes++;            // case 1: negative prune
      return false;
    }
    // Descend. A both-uniform descent is a (possibly redundant) searcher
    // for its unSEEN (g, f) — the §8.3 redundancy measurement for 3b.
    if (both_uniform) branch->p3_redundant_descents++;
    return true;
  }

  void node(const SpatialNode<FragData>& source, SpatialNode<FragData>& target) {}

  void leaf(const SpatialNode<FragData>& source, SpatialNode<FragData>& target) {
    // Both sides' uniform() (hence min/max) are consulted below.
    enforceValidAnnotation(source);
    enforceValidAnnotation(target);
    auto* branch = fof.ckLocalBranch();
    branch->p3_leaf_visits++;
    if (source.data.uniform() && target.data.uniform()) {
      // Uniform leaf pair: one (g, f) covers every particle pair, so
      // resolve it before the O(n*m) loop (the suppression payoff at leaf
      // level) — and a single witness suffices.
      long g = source.data.min_frag;
      long f = target.data.min_frag;
      if (g == f) {
        branch->p3_same_frag_prunes++;
        return;
      }
      uint64_t key = paratreet::packTipPair(g, f);
      if (branch->seenContains(key)) {
        branch->p3_suppression_prunes++;
        return;
      }
      for (int i = 0; i < source.n_particles; i++) {
        const Particle& sp = source.particles()[i];
        for (int j = 0; j < target.n_particles; j++) {
          const Particle& tp = target.particles()[j];
          if ((sp.position - tp.position).lengthSquared() > b2) continue;
          // Witness found: emit only if we won the race.
          if (branch->trySeenInsert(key)) branch->addPhase3Edge(g, f);
          return;
        }
      }
      return; // no witness in this leaf pair; other pairs may still find one
    }
    // Mixed fragments on at least one side: per particle-pair witness, as
    // in v1, but SEEN-gated so each (g, f) is emitted once per process.
    for (int i = 0; i < source.n_particles; i++) {
      const Particle& sp = source.particles()[i];
      for (int j = 0; j < target.n_particles; j++) {
        const Particle& tp = target.particles()[j];
        if ((sp.position - tp.position).lengthSquared() > b2) continue;
        if (sp.group_number == tp.group_number) continue; // same-tip: skip
        uint64_t key = paratreet::packTipPair(sp.group_number, tp.group_number);
        if (branch->trySeenInsert(key))
          branch->addPhase3Edge(sp.group_number, tp.group_number);
      }
    }
  }
};

namespace paratreet {

// Result of runFoFPhase3 (edge statistics for design note §8.2 and the 3a
// counters of design/step3.md §6, plus the merge-map size).
struct FoFPhase3Result {
  long edges_emitted;  // SEEN wins reaching addPhase3Edge, all PEs
  long edges_sent;     // after per-PE dedup = gathered to PE 0
  long edges_unique;   // after the root's second dedup = UF_2 input
  long tips_remapped;  // tips whose global root differs (broadcast map size)
  // 3a counters, summed over PEs (see FoFPhase1 for definitions):
  long negative_prunes;
  long positive_prunes;
  long suppression_prunes;
  long same_frag_prunes;
  long leaf_visits;
  long redundant_descents; // both-uniform descents (§8.3 redundancy data)
  long peak_edge_buf;      // max over PEs of the edge-buffer high-water mark
};

// Convenience driver for the full phase-3 sequence:
//   verifySharedLeaves -> resetPhase3 -> startDown<FoFEdgeVisitor> + CkWaitQD
//   -> flushPhase3Edges (concat reduction to this thread) -> serial UF_2 on
//   PE 0 -> applyGlobalMap broadcast -> relabel barrier.
// Must be called from a [threaded] entry method on PE 0 (the driver), after
// runFoFPhase1 and Subtree::upwardPass (+ QD) and before the next tree
// rebuild/reset. The gather-to-one UF_2 is v1 scaffolding (fine to ~1e6
// edges); the distributed UF_2 replaces it in step 4.
inline FoFPhase3Result runFoFPhase3(CProxy_Partition<FragData> partitions,
                                    CProxy_FoFPhase1<FragData> fof,
                                    double linking_length) {
  auto& config = paratreet::getConfiguration();
  CkEnforce(config.decomp_type == paratreet::subtreeDecompForTree(config.tree_type));
  partitions.verifySharedLeaves(CkCallbackResumeThread());

  double b2 = linking_length * linking_length;
  fof.resetPhase3(CkCallbackResumeThread());

  // The boundary walk: all Partitions against the global tree. QD covers
  // the traversal including cache fetches and resumptions.
  partitions.startDown<FoFEdgeVisitor>(FoFEdgeVisitor(fof, b2));
  CkWaitQD();

  // Gather the per-PE deduplicated buffers to this (PE 0, driver) thread.
  void* result = nullptr;
  fof.flushPhase3Edges(CkCallbackResumeThread(result));
  CkReductionMsg* msg = (CkReductionMsg*)result;
  int n_edges = msg->getSize() / sizeof(std::pair<long, long>);
  const auto* edges = (const std::pair<long, long>*)msg->getData();

  void* stats_result = nullptr;
  fof.phase3Stats(CkCallbackResumeThread(stats_result));
  CkReductionMsg* stats_msg = (CkReductionMsg*)stats_result;
  CkReduction::tupleElement* stats_elems = nullptr;
  int n_stats_elems = 0;
  stats_msg->toTuple(&stats_elems, &n_stats_elems);
  CkEnforce(n_stats_elems == 2);
  const long* stats = (const long*)stats_elems[0].data;
  FoFPhase3Result r;
  r.edges_emitted = stats[0];
  r.edges_sent = stats[1];
  r.negative_prunes = stats[2];
  r.positive_prunes = stats[3];
  r.suppression_prunes = stats[4];
  r.same_frag_prunes = stats[5];
  r.leaf_visits = stats[6];
  r.redundant_descents = stats[7];
  r.peak_edge_buf = *(const long*)stats_elems[1].data;
  delete[] stats_elems;
  CkEnforce(r.edges_sent == (long)n_edges);

  // Serial UF_2 over the deduplicated edges; union by min tip so the global
  // root of a component is the smallest tip id = the global particle order
  // of the component's min-order member (one namespace with phase 1).
  std::unordered_set<uint64_t> unique;
  std::unordered_map<long, long> parent;
  auto find = [&](long x) -> long {
    auto it = parent.find(x);
    if (it == parent.end()) { parent.emplace(x, x); return x; }
    long root = x;
    while (parent[root] != root) root = parent[root];
    while (parent[x] != root) { long next = parent[x]; parent[x] = root; x = next; }
    return root;
  };
  long n_unique = 0;
  for (int e = 0; e < n_edges; e++) {
    long lo = edges[e].first, hi = edges[e].second;
    uint64_t key = (uint64_t(uint32_t(lo)) << 32) | uint64_t(uint32_t(hi));
    if (!unique.insert(key).second) continue;
    n_unique++;
    long ra = find(lo), rb = find(hi);
    if (ra == rb) continue;
    if (ra < rb) parent[rb] = ra;
    else         parent[ra] = rb;
  }
  delete msg;
  delete stats_msg;
  r.edges_unique = n_unique;

  // tip -> globalRoot map, remapped tips only (identity omitted); every tip
  // not appearing in any edge stays its own root.
  std::vector<std::pair<long, long>> map_vec;
  for (auto& kv : parent) {
    long root = find(kv.first);
    if (root != kv.first) map_vec.emplace_back(kv.first, root);
  }
  r.tips_remapped = (long)map_vec.size();

  // Broadcast the map; each PE relabels its own registered particles
  // (owner-writes, identity if absent).
  fof.applyGlobalMap(map_vec, CkCallbackResumeThread());
  return r;
}

} // namespace paratreet

#endif // PARATREET_FOFPHASE3_H_
