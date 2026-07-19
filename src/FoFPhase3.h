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
// examples. FoFEdgeVisitor prunes on the Minkowski test
// mindist(source box, target box) > b (case-1 pruning only, v1) and at
// leaf x leaf emits a (tip, tip) edge for every particle pair within b whose
// group_numbers differ.
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
  // Case-1 pruning only (v1): open unless the box gap exceeds b. Boxes are
  // valid on every node type reached here (built from positions, which
  // phase 1 does not modify); min_frag/max_frag are NOT read, so nodes
  // shipped into the cache before upwardPass (the loadCache starter pack)
  // are safe to test.
  bool open(const SpatialNode<FragData>& source, SpatialNode<FragData>& target) {
    return paratreet::mindist2(source.data.box, target.data.box) <= b2;
  }

  void node(const SpatialNode<FragData>& source, SpatialNode<FragData>& target) {}

  void leaf(const SpatialNode<FragData>& source, SpatialNode<FragData>& target) {
    auto* branch = fof.ckLocalBranch();
    for (int i = 0; i < source.n_particles; i++) {
      const Particle& sp = source.particles()[i];
      for (int j = 0; j < target.n_particles; j++) {
        const Particle& tp = target.particles()[j];
        if ((sp.position - tp.position).lengthSquared() > b2) continue;
        if (sp.group_number == tp.group_number) continue;
        branch->addPhase3Edge(sp.group_number, tp.group_number);
      }
    }
  }
};

namespace paratreet {

// Result of runFoFPhase3 (edge statistics for design note §8.2, plus the
// merge-map size).
struct FoFPhase3Result {
  long edges_emitted;  // leaf-pair hits (before per-PE dedup), all PEs
  long edges_sent;     // after per-PE dedup = gathered to PE 0
  long edges_unique;   // after the root's second dedup = UF_2 input
  long tips_remapped;  // tips whose global root differs (broadcast map size)
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
  CkEnforce(stats_msg->getSize() == 2 * sizeof(long));
  const long* stats = (const long*)stats_msg->getData();
  FoFPhase3Result r;
  r.edges_emitted = stats[0];
  r.edges_sent = stats[1];
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
