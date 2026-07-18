#ifndef PARATREET_FOFPHASE1_H_
#define PARATREET_FOFPHASE1_H_

// FoF phase 1: intra-process friends-of-friends (see design/phase1.md).
//
// Per process, computes the connected components of the linking graph
// restricted to the process's own particles ("process-level tips"), writing
// the tip id of every particle into Particle::group_number. Tip id = global
// particle id (`order`) of the component's min-order particle, so PE-tips,
// process-tips and later UF_2 vertices share one namespace.
//
// Structure (all barriers are reduction callbacks driven by the caller):
//   (a) FoFPhase1 (group, per PE): serial union-find over the particles of
//       the subtrees resident on this PE. Neighbor discovery is a dual tree
//       walk over all pairs of this PE's subtrees (including self-pairs),
//       pruned when the box gap distance squared exceeds b^2.
//   (b) FoFPhase1 phaseB: for each subtree pair spanning two PEs of the same
//       process, the lower-PE side walks the pair over frozen data and emits
//       deduplicated (tip_i, tip_j) edges into its own buffer, then hands the
//       buffer to the process-wide FoFPhase1Node.
//   (c) FoFPhase1Node (nodegroup, per process): merge() runs a tiny serial
//       union-find over the collected boundary edges and builds the
//       PE-tip -> process-tip map.
//   (d) FoFPhase1 relabel: each PE rewrites its own particles' group_number
//       through the map (identity if absent). Owners write; no contention.
//
// No atomics anywhere: every sub-phase either writes only PE-owned data or
// reads only data frozen by the preceding barrier.
//
// Driving sequence (from a [threaded] context; see paratreet::runFoFPhase1):
//   fof_node.reset -> fof.reset -> subtrees.registerFoF -> fof.phaseA
//   -> fof.phaseB -> fof_node.merge -> fof.relabel
// Optionally after relabel (see paratreet::runFoFFragmentHistogram):
//   fof.countFragments -> fof_node.fragmentHistogram
//
// Lifetime contract: the Particle blocks registered by Subtree::registerFoF
// (Subtree::particles.data()) are stable from the end of tree build until the
// next rebuild/reset; run the whole sequence inside that window.

#include "paratreet.decl.h"
#include "common.h"
#include "Node.h"
#include "Particle.h"
#include "OrientedBox.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace paratreet {

// Component-wise gap distance squared between two axis-aligned boxes
// (0 if they overlap). Space.h has no box-box version of this, so it
// lives here.
inline Real mindist2(const OrientedBox<Real>& a, const OrientedBox<Real>& b) {
  Real d2 = 0;
  Real gx = std::max(a.lesser_corner.x - b.greater_corner.x,
                     b.lesser_corner.x - a.greater_corner.x);
  Real gy = std::max(a.lesser_corner.y - b.greater_corner.y,
                     b.lesser_corner.y - a.greater_corner.y);
  Real gz = std::max(a.lesser_corner.z - b.greater_corner.z,
                     b.lesser_corner.z - a.greater_corner.z);
  if (gx > 0) d2 += gx * gx;
  if (gy > 0) d2 += gy * gy;
  if (gz > 0) d2 += gz * gz;
  return d2;
}

// Record emitted by FoFPhase1::collect (validation/debugging helper).
// Uses Real for the position so a serial checker can reproduce the
// library's distance arithmetic bit-for-bit.
struct FoFParticleRecord {
  Real x, y, z;
  long tip;    // Particle::group_number after phase 1
  int order;   // global particle id
  int pad = 0;
};

// Result of the fragment-size histogram (design note §6.3e, giant-fragment
// detection), reduced over all processes. Fragment sizes are exact: tips are
// process-local, so each process knows its fragments' full sizes.
struct FoFFragmentHistogram {
  long bins[64];    // bins[k] = #fragments with floor(log2(size)) == k
  long n_fragments; // total fragment (process-tip) count
  long max_size;    // largest fragment size
};

} // namespace paratreet

// Per-process side of phase 1: collects the per-PE subtree registry (for
// phaseB pair enumeration), the cross-PE boundary edges, and — after
// merge() — the PE-tip -> process-tip map read by the group branches.
template <typename Data>
class FoFPhase1Node : public CBase_FoFPhase1Node<Data> {
public:
  struct SubtreeRef {
    Node<Data>* root;
    Particle* parts;
    int n;
  };

  std::mutex lock;
  // PE -> subtrees resident on that PE (this process's PEs only).
  std::map<int, std::vector<SubtreeRef>> pe_subtrees;
  std::vector<std::pair<long, long>> edges; // cross-PE (tip, tip) edges
  std::unordered_map<long, long> tip_map;   // PE-tip -> process-tip
  std::unordered_map<long, long> frag_counts; // process-tip -> exact size

  FoFPhase1Node() {}

  // Called synchronously (ckLocalBranch) by same-process group branches
  // during registration; hence the lock.
  void registerSubtree(int pe, Node<Data>* root, Particle* parts, int n) {
    std::lock_guard<std::mutex> g(lock);
    pe_subtrees[pe].push_back(SubtreeRef{root, parts, n});
  }

  // Called synchronously by group branches at the end of phaseB.
  void submitEdges(std::vector<std::pair<long, long>>&& es) {
    std::lock_guard<std::mutex> g(lock);
    edges.insert(edges.end(), es.begin(), es.end());
  }

  // Called synchronously by same-process group branches during
  // countFragments; hence the lock. Tips are process-local, so the merged
  // per-PE counts are exact fragment sizes.
  void submitFragCounts(const std::unordered_map<long, long>& counts) {
    std::lock_guard<std::mutex> g(lock);
    for (auto& kv : counts) frag_counts[kv.first] += kv.second;
  }

  void reset(const CkCallback& cb) {
    pe_subtrees.clear();
    edges.clear();
    tip_map.clear();
    frag_counts.clear();
    this->contribute(cb);
  }

  // Fragment-size histogram, step 2 of 2 (see paratreet::FoFFragmentHistogram
  // and design note §6.3e). One execution per process (nodegroup broadcast),
  // after the countFragments barrier: log2-bin the exact fragment sizes and
  // contribute a tuple reduction to cb — [0] long bins[64] (sum),
  // [1] long n_fragments (sum), [2] long max_size (max).
  void fragmentHistogram(const CkCallback& cb) {
    long bins[64] = {0};
    long n_fragments = 0;
    long max_size = 0;
    for (auto& kv : frag_counts) {
      long size = kv.second;
      int bin = 0; // floor(log2(size)); size >= 1 always
      while (bin < 63 && (1L << (bin + 1)) <= size) bin++;
      bins[bin]++;
      n_fragments++;
      if (size > max_size) max_size = size;
    }
    CkReduction::tupleElement tupleRedn[] = {
      CkReduction::tupleElement(sizeof(bins), bins, CkReduction::sum_long),
      CkReduction::tupleElement(sizeof(long), &n_fragments, CkReduction::sum_long),
      CkReduction::tupleElement(sizeof(long), &max_size, CkReduction::max_long)
    };
    CkReductionMsg* msg = CkReductionMsg::buildFromTuple(tupleRedn, 3);
    msg->setCallback(cb);
    this->contribute(msg);
  }

  // One execution per process (nodegroup broadcast): serial union-find over
  // the boundary edges; touches only tips that appear in edges.
  void merge(const CkCallback& cb) {
    std::unordered_map<long, long> parent;
    for (auto& e : edges) {
      long ra = findRoot(parent, e.first);
      long rb = findRoot(parent, e.second);
      if (ra == rb) continue;
      if (ra < rb) parent[rb] = ra; // union by min: smaller tip id wins
      else         parent[ra] = rb;
    }
    tip_map.clear();
    for (auto& kv : parent) {
      long root = findRoot(parent, kv.first);
      if (root != kv.first) tip_map.emplace(kv.first, root);
    }
    this->contribute(cb);
  }

private:
  static long findRoot(std::unordered_map<long, long>& parent, long x) {
    auto it = parent.find(x);
    if (it == parent.end()) {
      parent.emplace(x, x);
      return x;
    }
    long root = x;
    while (parent[root] != root) root = parent[root];
    while (parent[x] != root) { // path compression
      long next = parent[x];
      parent[x] = root;
      x = next;
    }
    return root;
  }
};

// Per-PE side of phase 1. See file header for the protocol.
template <typename Data>
class FoFPhase1 : public CBase_FoFPhase1<Data> {
public:
  struct SubtreeRef {
    Node<Data>* root;
    Particle* parts; // the Subtree's own particle block (stable until rebuild)
    int n;
    int offset;      // base of this block in the PE-flat index space
  };

  FoFPhase1(CProxy_FoFPhase1Node<Data> node_proxy_) : node_proxy(node_proxy_) {}

  // Synchronous, called by Subtree::registerFoF on this PE.
  void registerSubtree(Node<Data>* root, Particle* parts, int n) {
    subtrees.push_back(SubtreeRef{root, parts, n, 0});
    node_proxy.ckLocalBranch()->registerSubtree(CkMyPe(), root, parts, n);
  }

  void reset(const CkCallback& cb) {
    subtrees.clear();
    uf_parent.clear();
    flat_order.clear();
    edge_buf.clear();
    seen.clear();
    this->contribute(cb);
  }

  // (a) Per-PE union-find via dual walks over all pairs of this PE's
  // subtrees (self-pairs included), then full path compression and tip
  // assignment into Particle::group_number.
  void phaseA(double b2, const CkCallback& cb) {
    b2_ = b2;
    // Offset table: flat index space over this PE's particle blocks.
    int n_local = 0;
    for (auto& s : subtrees) {
      s.offset = n_local;
      n_local += s.n;
    }
    uf_parent.resize(n_local);
    std::iota(uf_parent.begin(), uf_parent.end(), 0);
    flat_order.resize(n_local);
    for (auto& s : subtrees)
      for (int i = 0; i < s.n; i++) flat_order[s.offset + i] = s.parts[i].order;

    for (size_t i = 0; i < subtrees.size(); i++) {
      for (size_t j = i; j < subtrees.size(); j++) {
        const SubtreeRef& sa = subtrees[i];
        const SubtreeRef& sb = subtrees[j];
        walk(sa.root, sb.root,
             [&](Node<Data>* a, Node<Data>* b) { leafLeafUnion(a, b, sa, sb); });
      }
    }

    // Freeze + compress: write tip id (order of the component's min-order
    // root particle) into every particle.
    for (auto& s : subtrees)
      for (int i = 0; i < s.n; i++)
        s.parts[i].group_number = flat_order[find(s.offset + i)];

    this->contribute(cb);
  }

  // (b) Cross-PE edge emission. For each subtree pair spanning this PE and a
  // higher PE of the same process, walk the pair over frozen data and emit
  // deduplicated (tip, tip) edges into this PE's buffer; hand the buffer to
  // the nodegroup. No-op when this process has a single PE (non-SMP or
  // one-PE-per-process runs).
  void phaseB(double b2, const CkCallback& cb) {
    b2_ = b2;
    edge_buf.clear();
    seen.clear();
    auto* nb = node_proxy.ckLocalBranch();
    int my_pe = CkMyPe();
    // pe_subtrees is frozen since the registration barrier: safe to read.
    for (auto& kv : nb->pe_subtrees) {
      if (kv.first <= my_pe) continue; // lower-PE side walks the pair
      for (auto& sa : subtrees) {
        for (auto& sb : kv.second) {
          walk(sa.root, sb.root,
               [&](Node<Data>* a, Node<Data>* b) { leafLeafEmit(a, b); });
        }
      }
    }
    if (!edge_buf.empty()) nb->submitEdges(std::move(edge_buf));
    edge_buf.clear();
    seen.clear();
    this->contribute(cb);
  }

  // (d) Rewrite this PE's particles' group_number through the merge map
  // (identity if absent).
  void relabel(const CkCallback& cb) {
    auto& tip_map = node_proxy.ckLocalBranch()->tip_map;
    for (auto& s : subtrees) {
      for (int i = 0; i < s.n; i++) {
        auto it = tip_map.find(s.parts[i].group_number);
        if (it != tip_map.end()) s.parts[i].group_number = it->second;
      }
    }
    this->contribute(cb);
  }

  // Fragment-size histogram, step 1 of 2 (run after relabel): count this
  // PE's registered particles per process-tip and hand the counts to the
  // process-wide FoFPhase1Node (synchronous local-branch call, like
  // submitEdges, so the counts are complete when the barrier fires).
  void countFragments(const CkCallback& cb) {
    std::unordered_map<long, long> counts;
    for (auto& s : subtrees)
      for (int i = 0; i < s.n; i++) counts[s.parts[i].group_number]++;
    node_proxy.ckLocalBranch()->submitFragCounts(counts);
    this->contribute(cb);
  }

  // Validation/debugging helper: concat-reduce (position, tip, order) for
  // every particle registered on this PE.
  void collect(const CkCallback& cb) {
    std::vector<paratreet::FoFParticleRecord> recs;
    for (auto& s : subtrees) {
      for (int i = 0; i < s.n; i++) {
        paratreet::FoFParticleRecord r;
        r.x = s.parts[i].position.x;
        r.y = s.parts[i].position.y;
        r.z = s.parts[i].position.z;
        r.tip = s.parts[i].group_number;
        r.order = s.parts[i].order;
        recs.push_back(r);
      }
    }
    this->contribute(recs.size() * sizeof(paratreet::FoFParticleRecord),
                     recs.data(), CkReduction::concat, cb);
  }

private:
  // Dual tree walk over a pair of local trees; prunes on box gap distance.
  // leaf_fn(a, b) is invoked on leaf x leaf pairs.
  template <typename LeafFn>
  void walk(Node<Data>* a, Node<Data>* b, const LeafFn& leaf_fn) {
    if (a == nullptr || b == nullptr) return;
    if (a->n_particles == 0 || b->n_particles == 0) return; // empty leaves
    if (paratreet::mindist2(a->data.box, b->data.box) > b2_) return;
    if (a->isLeaf() && b->isLeaf()) {
      leaf_fn(a, b);
      return;
    }
    bool open_a;
    if (a->isLeaf()) open_a = false;
    else if (b->isLeaf()) open_a = true;
    else open_a = boxMeasure(a) >= boxMeasure(b); // open the larger box
    if (open_a) {
      for (int i = 0; i < a->n_children; i++) walk(a->getChild(i), b, leaf_fn);
    } else {
      for (int i = 0; i < b->n_children; i++) walk(a, b->getChild(i), leaf_fn);
    }
  }

  static Real boxMeasure(Node<Data>* n) {
    auto sz = n->data.box.size();
    return sz.x + sz.y + sz.z;
  }

  // phaseA leaf action: pairwise distance checks -> union.
  void leafLeafUnion(Node<Data>* a, Node<Data>* b,
                     const SubtreeRef& sa, const SubtreeRef& sb) {
    const Particle* pa = a->particles();
    const Particle* pb = b->particles();
    int fa = sa.offset + int(pa - sa.parts);
    int fb = sb.offset + int(pb - sb.parts);
    if (a == b) {
      for (int i = 0; i < a->n_particles; i++)
        for (int j = i + 1; j < a->n_particles; j++)
          if ((pa[i].position - pa[j].position).lengthSquared() <= b2_)
            unite(fa + i, fa + j);
    } else {
      for (int i = 0; i < a->n_particles; i++)
        for (int j = 0; j < b->n_particles; j++)
          if ((pa[i].position - pb[j].position).lengthSquared() <= b2_)
            unite(fa + i, fb + j);
    }
  }

  // phaseB leaf action: pairwise distance checks -> deduplicated tip edges.
  void leafLeafEmit(Node<Data>* a, Node<Data>* b) {
    const Particle* pa = a->particles();
    const Particle* pb = b->particles();
    for (int i = 0; i < a->n_particles; i++) {
      for (int j = 0; j < b->n_particles; j++) {
        if ((pa[i].position - pb[j].position).lengthSquared() > b2_) continue;
        long ti = pa[i].group_number;
        long tj = pb[j].group_number;
        if (ti == tj) continue;
        long lo = std::min(ti, tj), hi = std::max(ti, tj);
        // Tips are particle orders (int), so they pack into 32 bits each.
        uint64_t key = (uint64_t(uint32_t(lo)) << 32) | uint64_t(uint32_t(hi));
        if (seen.insert(key).second) edge_buf.emplace_back(lo, hi);
      }
    }
  }

  int find(int x) {
    int root = x;
    while (uf_parent[root] != root) root = uf_parent[root];
    while (uf_parent[x] != root) { // path compression
      int next = uf_parent[x];
      uf_parent[x] = root;
      x = next;
    }
    return root;
  }

  void unite(int x, int y) {
    int rx = find(x), ry = find(y);
    if (rx == ry) return;
    // Union by min: the root with the smaller global particle order wins.
    if (flat_order[rx] < flat_order[ry]) uf_parent[ry] = rx;
    else                                 uf_parent[rx] = ry;
  }

  CProxy_FoFPhase1Node<Data> node_proxy;
  std::vector<SubtreeRef> subtrees;
  std::vector<int> uf_parent;  // per-PE UF over the flat index space
  std::vector<int> flat_order; // flat index -> global particle order
  std::vector<std::pair<long, long>> edge_buf;
  std::unordered_set<uint64_t> seen;
  double b2_ = 0.0;
};

namespace paratreet {

// Convenience driver for the full phase-1 sequence. Must be called from a
// [threaded] entry method (uses blocking callbacks), between tree build and
// the next rebuild/reset (registered particle blocks must stay alive).
template <typename Data>
void runFoFPhase1(CProxy_Subtree<Data> subtrees,
                  CProxy_FoFPhase1<Data> fof,
                  CProxy_FoFPhase1Node<Data> fof_node,
                  double linking_length) {
  double b2 = linking_length * linking_length;
  fof_node.reset(CkCallbackResumeThread());
  fof.reset(CkCallbackResumeThread());
  subtrees.registerFoF(fof, CkCallbackResumeThread());
  fof.phaseA(b2, CkCallbackResumeThread());
  fof.phaseB(b2, CkCallbackResumeThread());
  fof_node.merge(CkCallbackResumeThread());
  fof.relabel(CkCallbackResumeThread());
}

// Convenience driver for the fragment-size histogram. Run after
// runFoFPhase1, under the same threaded-context and particle-lifetime
// requirements; blocks and returns the global histogram.
template <typename Data>
FoFFragmentHistogram runFoFFragmentHistogram(CProxy_FoFPhase1<Data> fof,
                                             CProxy_FoFPhase1Node<Data> fof_node) {
  fof.countFragments(CkCallbackResumeThread());
  void* result = nullptr;
  fof_node.fragmentHistogram(CkCallbackResumeThread(result));
  CkReductionMsg* msg = (CkReductionMsg*)result;
  CkReduction::tupleElement* elems = nullptr;
  int n_elems = 0;
  msg->toTuple(&elems, &n_elems);
  CkEnforce(n_elems == 3);
  FoFFragmentHistogram h;
  std::memcpy(h.bins, elems[0].data, sizeof(h.bins));
  h.n_fragments = *(const long*)elems[1].data;
  h.max_size = *(const long*)elems[2].data;
  delete[] elems;
  delete msg;
  return h;
}

} // namespace paratreet

#endif // PARATREET_FOFPHASE1_H_
