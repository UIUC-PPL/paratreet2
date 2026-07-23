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
#include "unionFindLib.h"

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

// Step 4 (distributed UF_2, design/step4.md "Tip encoding"): owner-encoded
// vertex namespace for UF_2. A process-tip is renumbered to
// (owning_process << kUF2IdxBits) | dense_index, where dense_index is a
// per-process-dense enumeration of that process's own fragments (assigned by
// FoFPhase1Node::computeTipEncoding). Because the encoding happens BEFORE
// upwardPass/loadCache/the phase-3 walk, every particle copy the walk reads
// (local or cache-shipped) already carries the encoded value, so
// FoFEdgeVisitor needs no changes: the (g, f) pairs it emits are already
// UF_2 vertex ids, and getLocationFromID(vid) below decodes them with no
// directory lookup (O(1), no communication) -- this is "Option C" from the
// design decision, chosen over a tip->owner directory (Option A, ~5 MB per
// 1e-3-density-tip at 16M scale, flagged as a scaling concern there) or a
// post-hoc dense renumbering round (Option B, unsound: the emitting process
// cannot learn a remote tip's dense id without a directory anyway).
// 40 index bits -> up to ~1.1e12 fragments per process (never binding at any
// realistic scale); the remaining 24 bits address up to ~16M processes.
constexpr int kUF2IdxBits = 40;
constexpr uint64_t kUF2IdxMask = (uint64_t(1) << kUF2IdxBits) - 1;

inline uint64_t uf2EncodeTip(int process, long dense_index) {
  return (uint64_t(uint32_t(process)) << kUF2IdxBits) | (uint64_t(dense_index) & kUF2IdxMask);
}

// Registered with UnionFindLib::registerGetLocationFromID. Must be a plain
// function (not a capturing lambda): the library stores it as a raw
// std::pair<int,int>(*)(uint64_t) function pointer.
inline std::pair<int, int> uf2LocationFromID(uint64_t vid) {
  return { int(vid >> kUF2IdxBits), int(vid & kUF2IdxMask) };
}

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

// Periodic boundary conditions (design/pbc.md). We keep a SINGLE walk /
// traversal and make the DISTANCE functions periodic (minimum-image), rather
// than the 27-image walk old paratreet uses. This is equivalent for FoF
// because the linking length always satisfies b < L/2, so each particle has
// at most one image of any other within b (the nearest). period == 0 on an
// axis means "open" (the periodic branch is a no-op) and must reproduce the
// existing open-boundary arithmetic bit-for-bit.

// Minimum-image periodic squared distance between two points. Per axis,
// wrap the coordinate difference into [-period/2, period/2] via
// d -= period*round(d/period). Origin-agnostic (works for a box centered on
// any origin, e.g. LAMBS is [-0.5, 0.5]). With period == {0,0,0} this is
// exactly Vector3D::lengthSquared of (a - b) (dx*dx + dy*dy + dz*dz in the
// same order), so open-boundary callers are bit-identical.
inline Real periodicDistSq(const Vector3D<Real>& a, const Vector3D<Real>& b,
                           const Vector3D<Real>& period) {
  Real dx = a.x - b.x;
  Real dy = a.y - b.y;
  Real dz = a.z - b.z;
  if (period.x > 0) dx -= period.x * std::round(dx / period.x);
  if (period.y > 0) dy -= period.y * std::round(dy / period.y);
  if (period.z > 0) dz -= period.z * std::round(dz / period.z);
  return dx * dx + dy * dy + dz * dz;
}

// Periodic per-axis interval gap, squared and summed over axes. For each
// axis with period p > 0 we take the smallest non-negative gap between
// intervals [a.lo, a.hi] and [b.lo, b.hi] over b shifted by {-p, 0, +p}; if
// ANY shift overlaps (gap <= 0) the axis contributes 0. For p == 0 the axis
// reduces to the existing open-boundary gap (max(a.lo-b.hi, b.lo-a.hi)),
// making mindist2(a, b, {0,0,0}) bit-identical to the 2-arg mindist2 above.
//
// Hand cases (one axis, p = 1, unit box origin-agnostic):
//  * A=[0.0,0.1], B=[0.0,0.1]: overlap at shift 0 -> gap 0. (identical boxes)
//  * A=[0.0,0.1], B=[0.85,0.95]: shift 0 gap = 0.85-0.1 = 0.75; shift -p puts
//    B at [-0.15,-0.05], gap = max(0.0-(-0.05), -0.15-0.1) = 0.05; min = 0.05
//    -> two boxes near opposite faces of an L=1 box are 0.05 apart under PBC
//    (they wrap), whereas the open-boundary gap is 0.75. So b >= 0.05 links
//    them under PBC but not without it — the effect PBC must produce.
//  * A=[0.0,0.1], B=[0.2,0.3]: shift 0 gap = 0.1; shift +p B=[1.2,1.3] gap
//    = 1.1; shift -p B=[-0.8,-0.7] gap = 0.7; min = 0.1 (no wrap benefit).
inline Real mindist2(const OrientedBox<Real>& a, const OrientedBox<Real>& b,
                     const Vector3D<Real>& period) {
  auto axisGap = [](Real alo, Real ahi, Real blo, Real bhi, Real p) -> Real {
    Real g0 = std::max(alo - bhi, blo - ahi); // open-boundary gap
    if (p <= 0) return g0 > 0 ? g0 : 0;        // exact open behavior
    Real gp = std::max(alo - (bhi + p), (blo + p) - ahi); // b shifted by +p
    Real gm = std::max(alo - (bhi - p), (blo - p) - ahi); // b shifted by -p
    if (g0 <= 0 || gp <= 0 || gm <= 0) return 0; // any shift overlaps
    return std::min(g0, std::min(gp, gm));       // smallest positive gap
  };
  Real gx = axisGap(a.lesser_corner.x, a.greater_corner.x,
                    b.lesser_corner.x, b.greater_corner.x, period.x);
  Real gy = axisGap(a.lesser_corner.y, a.greater_corner.y,
                    b.lesser_corner.y, b.greater_corner.y, period.y);
  Real gz = axisGap(a.lesser_corner.z, a.greater_corner.z,
                    b.lesser_corner.z, b.greater_corner.z, period.z);
  return gx * gx + gy * gy + gz * gz;
}

// Component-wise MAXIMUM distance squared between two axis-aligned boxes:
// the distance between the farthest pair of points, one from each box. Per
// axis the farthest pair sits at interval endpoints, so the axis term is
// max(|a.lo - b.hi|, |a.hi - b.lo|); sum the squares over axes.
// Hand checks (one axis): disjoint A=[0,1], B=[2,3] -> max(|0-3|,|1-2|) = 3
// (farthest points 0 and 3). Identical A=B=[0,1] -> max(|0-1|,|1-0|) = 1
// (opposite endpoints). Nested A=[0,4], B=[1,2] -> max(|0-2|,|4-1|) = 3
// (farthest points 4 and 1). Like mindist2, a pure function of the two
// boxes, so PBC offsets parameterize it the same way (shift one box).
inline Real maxdist2(const OrientedBox<Real>& a, const OrientedBox<Real>& b) {
  Real dx = std::max(std::fabs(a.lesser_corner.x - b.greater_corner.x),
                     std::fabs(a.greater_corner.x - b.lesser_corner.x));
  Real dy = std::max(std::fabs(a.lesser_corner.y - b.greater_corner.y),
                     std::fabs(a.greater_corner.y - b.lesser_corner.y));
  Real dz = std::max(std::fabs(a.lesser_corner.z - b.greater_corner.z),
                     std::fabs(a.greater_corner.z - b.lesser_corner.z));
  return dx * dx + dy * dy + dz * dz;
}

// Canonical key for an unordered (tip, tip) pair, used by the phase-3a SEEN
// table (FoFPhase1Node::seen3_pairs) and FoFPhase1's per-PE SEEN3 dedup
// (addPhase3Edge). Two uint64_t fields, NOT a single packed 64-bit integer:
// step 1-3 tips fit in 32 bits (particle orders, < N), so the original
// packTipPair packed (min << 32 | max) losslessly -- but step 4's
// owner-encoded UF_2 tips (paratreet::uf2EncodeTip, up to kUF2IdxBits + node
// bits) can each need close to the full 64 bits, so packing TWO of them into
// one 64-bit integer would silently truncate and alias distinct pairs onto
// the same key -- a false-suppression correctness bug (a real edge dropped
// as "already SEEN"), not just a hash collision. TipPairKey stores both
// values in full and compares them exactly; only its HASH combines lossily
// (that's fine: unordered_set resolves same-bucket entries via operator==).
struct TipPairKey {
  uint64_t lo, hi;
  bool operator==(const TipPairKey& o) const { return lo == o.lo && hi == o.hi; }
};
struct TipPairKeyHash {
  size_t operator()(const TipPairKey& k) const {
    size_t h1 = std::hash<uint64_t>()(k.lo);
    size_t h2 = std::hash<uint64_t>()(k.hi);
    // boost::hash_combine shape; collisions only cost a bucket re-check
    // (operator== above is exact), never correctness.
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
  }
};
inline TipPairKey packTipPair(long a, long b) {
  uint64_t ua = uint64_t(a), ub = uint64_t(b);
  return ua < ub ? TipPairKey{ua, ub} : TipPairKey{ub, ua};
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

// Result of the global component-size histogram over the final (post
// phase-3 relabel) labels, computed WITHOUT gathering particles: each PE
// contributes its (label, count) pairs and the caller merges (a component
// spanning k PEs contributes k pairs). Same binning as FoFFragmentHistogram.
// Used by the fof3 harness's stats mode as the cross-run determinism check.
struct FoFComponentHistogram {
  long bins[64];      // bins[k] = #components with floor(log2(size)) == k
  long n_components;  // total component count
  long max_size;      // largest component size
  // Step 5 (design/step5-pruning.md): min-component-size REPORTING filter. When
  // the harness's -m threshold is > 0 these describe only the components with
  // size >= m (the "surviving" set); they are computed from the SAME merged
  // per-label counts as the fields above (no second gather). When m == 0 the
  // surviving_* fields mirror the totals and the harness does not print them.
  long surviving_bins[64];  // bins over components with size >= m
  long surviving_count;     // #components with size >= m
  long surviving_max_size;  // largest surviving component size (0 if none)
  int min_component_size;   // the m used (0 = no filter)
};

// Result of the per-PE memory reduction (CmiMemoryUsage). In SMP builds
// CmiMemoryUsage reports PROCESS-wide allocation, so every PE of a process
// contributes the same value; min/avg/max are then over processes in
// practice. avg = sum / CkNumPes().
struct FoFMemoryStats {
  long min_bytes;
  long max_bytes;
  double avg_bytes;
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

  // Step 4 (distributed UF_2): built by computeTipEncoding() from
  // frag_counts (so it must run after countFragments). encode_map maps this
  // process's own process-tips to their encoded UF_2 vertex ids
  // (paratreet::uf2EncodeTip); uf2_vertices is the vertex array handed to
  // UnionFindLib::initialize_vertices by index (dense_index == its position
  // here) -- UnionFindLib mutates componentNumber/parent/size IN PLACE in
  // this same storage, so applyUF2Labels reads results straight out of it.
  std::unordered_map<long, long> encode_map; // process-tip -> encoded tip
  std::vector<unionFindVertex> uf2_vertices;

  // Phase-3a SEEN table (design/step3.md §1, §3): process-level set of
  // packed (g, f) fragment pairs for which an edge has been (or is being)
  // emitted. Process level is sufficient for suppression because in the
  // transposed traversal the target fragment f is always process-local, so
  // every node pair over (g, f) is generated on f's owner process. Guarded
  // by its own mutex (single mutex is fine for 3a; stripe if contention
  // shows). Called synchronously from FoFEdgeVisitor via ckLocalBranch.
  std::mutex seen3_lock;
  std::unordered_set<paratreet::TipPairKey, paratreet::TipPairKeyHash> seen3_pairs;

  FoFPhase1Node() {}

  // Returns true iff this caller won (inserted first). SEEN is only ever
  // set at the moment an edge is emitted (winner emits), so suppression can
  // never suppress an unemitted edge.
  bool trySeenInsert(paratreet::TipPairKey key) {
    std::lock_guard<std::mutex> g(seen3_lock);
    return seen3_pairs.insert(key).second;
  }

  bool seenContains(paratreet::TipPairKey key) {
    std::lock_guard<std::mutex> g(seen3_lock);
    return seen3_pairs.count(key) != 0;
  }

  // Reset with the rest of the phase-3 state (called by every same-process
  // FoFPhase1 branch during resetPhase3; redundant clears are idempotent
  // and the reset barrier orders them before the walk).
  void clearSeen() {
    std::lock_guard<std::mutex> g(seen3_lock);
    seen3_pairs.clear();
  }

  // Per-PROCESS redundant-descent total (design/step3.md §6d): each of this
  // process's group branches deposits its per-PE p3_redundant_descents here
  // post-walk (depositNodeRedundant), so every PE can then read the same
  // process total for the per-process min/max reduction. Guarded by seen3_lock
  // (already the phase-3 mutex; deposit is one call per PE, not hot-path).
  long p3_node_redundant = 0;
  void clearNodeRedundant() {
    std::lock_guard<std::mutex> g(seen3_lock);
    p3_node_redundant = 0;
  }
  void addNodeRedundant(long v) {
    std::lock_guard<std::mutex> g(seen3_lock);
    p3_node_redundant += v;
  }

  // Per-(g,f) redundant-descent counts (design/step3.md §6e): how CONCENTRATED
  // is the pre-witness redundancy -- a few hot fragment pairs each hammered, or
  // many pairs each descended a handful of times? Keyed per process-local pair
  // (f is process-local; an edge seen from both endpoint processes appears as a
  // separate entry on each, which is the right granularity: each process's own
  // redundant work). Own mutex so it does not extend seen3_lock's critical
  // section on the hot both-uniform open() path.
  std::mutex redun_lock;
  std::unordered_map<paratreet::TipPairKey, long, paratreet::TipPairKeyHash> redun_per_pair;
  void recordRedundant(paratreet::TipPairKey key) {
    std::lock_guard<std::mutex> g(redun_lock);
    redun_per_pair[key]++;
  }
  void clearRedun() {
    std::lock_guard<std::mutex> g(redun_lock);
    redun_per_pair.clear();
  }
  // log2 histogram of descents-per-pair (same binning as fragmentHistogram):
  //   [0] long bins[64] (sum over processes) -- bins[k] = #pairs with
  //       floor(log2(descents)) == k; [1] long n_pairs (sum); [2] long
  //       max descents on any single pair (max). Pairs are process-disjoint
  //       at the counting granularity, so sum across processes is exact.
  void redundancyHistogram(const CkCallback& cb) {
    long bins[64] = {0};
    long n_pairs = 0;
    long max_per_pair = 0;
    for (auto& kv : redun_per_pair) {
      long c = kv.second;
      int bin = 0; // floor(log2(c)); c >= 1 always
      while (bin < 63 && (1L << (bin + 1)) <= c) bin++;
      bins[bin]++;
      n_pairs++;
      if (c > max_per_pair) max_per_pair = c;
    }
    CkReduction::tupleElement tupleRedn[] = {
      CkReduction::tupleElement(sizeof(bins), bins, CkReduction::sum_long),
      CkReduction::tupleElement(sizeof(long), &n_pairs, CkReduction::sum_long),
      CkReduction::tupleElement(sizeof(long), &max_per_pair, CkReduction::max_long)
    };
    CkReductionMsg* msg = CkReductionMsg::buildFromTuple(tupleRedn, 3);
    msg->setCallback(cb);
    this->contribute(msg);
  }

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
    encode_map.clear();
    uf2_vertices.clear();
    clearSeen();
    this->contribute(cb);
  }

  // Step 4 (distributed UF_2): build the owner-encoded tip namespace from
  // frag_counts (must run after countFragments has populated it for every
  // PE of this process). One execution per process (nodegroup broadcast).
  // Enumeration order (map iteration) only needs to be a bijection per
  // process -- it does not need to be deterministic across runs: UF_2's
  // resulting componentNumber values are arbitrary serial ids regardless
  // (design/step4.md; the harness canonicalizes by min order per label
  // group, not by raw label value).
  void computeTipEncoding(const CkCallback& cb) {
    encode_map.clear();
    uf2_vertices.clear();
    uf2_vertices.reserve(frag_counts.size());
    int my_node = CkMyNode();
    for (auto& kv : frag_counts) {
      long dense_index = (long)uf2_vertices.size();
      uint64_t encoded = paratreet::uf2EncodeTip(my_node, dense_index);
      encode_map.emplace(kv.first, (long)encoded);
      unionFindVertex v;
      v.vertexID = encoded;
      v.parent = -1;
      v.process_tip = -1;
      v.componentNumber = -1;
      v.componentSize = -1;
      v.size = kv.second; // fragment size (particle count), not the default 1
      uf2_vertices.push_back(std::move(v));
    }
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

  // Periodic boundary conditions (design/pbc.md): the box period, broadcast
  // by the app before phaseA. 0 per axis = open boundaries (the periodic
  // branch is a no-op; period_ defaults to {0,0,0} = exact current behavior).
  // Persistent like b2_ (not cleared by reset(); the driver re-broadcasts it
  // each run before phaseA).
  void setPeriod(Vector3D<Real> period, const CkCallback& cb) {
    period_ = period;
    this->contribute(cb);
  }

  void reset(const CkCallback& cb) {
    subtrees.clear();
    uf_parent.clear();
    flat_order.clear();
    edge_buf.clear();
    seen.clear();
    edge_buf3.clear();
    seen3.clear();
    t_phaseA = 0.0;
    t_phaseB = 0.0;
    phase3_emitted = 0;
    p3_negative_prunes = 0;
    p3_positive_prunes = 0;
    p3_suppression_prunes = 0;
    p3_same_frag_prunes = 0;
    p3_leaf_visits = 0;
    p3_redundant_descents = 0;
    p3_peak_edge_buf = 0;
    this->contribute(cb);
  }

  // (a) Per-PE union-find via dual walks over all pairs of this PE's
  // subtrees (self-pairs included), then full path compression and tip
  // assignment into Particle::group_number.
  void phaseA(double b2, const CkCallback& cb) {
    double t0 = CkWallTimer();
    b2_ = b2;
    // Offset table: flat index space over this PE's particle blocks.
    int n_local = 0;
    for (auto& s : subtrees) {
      s.offset = n_local;
      n_local += s.n;
    }
    uf_parent.resize(n_local);
    std::iota(uf_parent.begin(), uf_parent.end(), 0);
    cert_rep.clear();
    flat_order.resize(n_local);
    for (auto& s : subtrees)
      for (int i = 0; i < s.n; i++) flat_order[s.offset + i] = s.parts[i].order;

    // Self pairs FIRST, cross pairs after (merge-early ordering): local
    // assembly populates the connectivity memo, so the cross-pair walks see
    // maximal suppression (design/phase1-scaling.md, connectivity layer).
    p1_conn_suppressed = 0;
    for (int pass = 0; pass < 2; pass++) {
      for (size_t i = 0; i < subtrees.size(); i++) {
        for (size_t j = i; j < subtrees.size(); j++) {
          if ((pass == 0) != (i == j)) continue; // pass 0: self; pass 1: cross
          const SubtreeRef& sa = subtrees[i];
          const SubtreeRef& sb = subtrees[j];
          walk(sa.root, sb.root,
               [&](Node<Data>* a, Node<Data>* b) { leafLeafUnion(a, b, sa, sb); },
               [&](Node<Data>* a, Node<Data>* b) {
                 // Positive certificate: each node becomes a memoized fragment
                 // on first touch (certRep); repeat certificates are O(1).
                 int ra = certRep(a, sa);
                 if (a != b) unite(ra, certRep(b, sb));
               },
               [&](Node<Data>* a, Node<Data>* b) {
                 // Connectivity suppression (phase-3 SEEN analog, monotone):
                 // if both sides are internally connected and already share
                 // a component, no cross test can change anything — prune
                 // the pair. A connected node's SELF pair is likewise done.
                 int ra = connectedRep(a, sa);
                 if (ra < 0) return false;
                 if (a == b) { p1_conn_suppressed++; return true; }
                 int rb = connectedRep(b, sb);
                 if (rb < 0) return false;
                 if (find(ra) == find(rb)) { p1_conn_suppressed++; return true; }
                 return false;
               });
        }
      }
    }

    // Freeze + compress: write tip id (order of the component's min-order
    // root particle) into every particle.
    for (auto& s : subtrees)
      for (int i = 0; i < s.n; i++)
        s.parts[i].group_number = flat_order[find(s.offset + i)];

    t_phaseA = CkWallTimer() - t0; // per-PE load signal, reduced by phase3Stats
    this->contribute(cb);
  }

  // (b) Cross-PE edge emission. For each subtree pair spanning this PE and a
  // higher PE of the same process, walk the pair over frozen data and emit
  // deduplicated (tip, tip) edges into this PE's buffer; hand the buffer to
  // the nodegroup. No-op when this process has a single PE (non-SMP or
  // one-PE-per-process runs).
  void phaseB(double b2, const CkCallback& cb) {
    double t0 = CkWallTimer();
    b2_ = b2;
    edge_buf.clear();
    seen.clear();
    cert_tip.clear();
    auto* nb = node_proxy.ckLocalBranch();
    int my_pe = CkMyPe();
    // pe_subtrees is frozen since the registration barrier: safe to read.
    for (auto& kv : nb->pe_subtrees) {
      if (kv.first <= my_pe) continue; // lower-PE side walks the pair
      for (auto& sa : subtrees) {
        for (auto& sb : kv.second) {
          walk(sa.root, sb.root,
               [&](Node<Data>* a, Node<Data>* b) { leafLeafEmit(a, b); },
               [&](Node<Data>* a, Node<Data>* b) {
                 // Positive certificate over frozen tips, memoized: each
                 // node star-emits once; the pair contributes one edge
                 // (a != b always here — the pair spans two PEs).
                 long ta = certTipRep(a);
                 long tb = certTipRep(b);
                 if (ta != tb) {
                   long lo = std::min(ta, tb), hi = std::max(ta, tb);
                   uint64_t key =
                       (uint64_t(uint32_t(lo)) << 32) | uint64_t(uint32_t(hi));
                   if (seen.insert(key).second) edge_buf.emplace_back(lo, hi);
                 }
               },
               // No connectivity suppression in phaseB: there is no live UF
               // over frozen tips; the seen-set dedup plays that role at
               // edge granularity.
               [](Node<Data>*, Node<Data>*) { return false; });
        }
      }
    }
    if (!edge_buf.empty()) nb->submitEdges(std::move(edge_buf));
    edge_buf.clear();
    seen.clear();
    t_phaseB = CkWallTimer() - t0; // per-PE load signal, reduced by phase3Stats
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

  // --- Step 4 (distributed UF_2, -u dist; see design/step4.md). Sequence,
  // driven by paratreet::runFoFPhase3Dist: countFragments (above, builds
  // frag_counts) -> FoFPhase1Node::computeTipEncoding (builds encode_map +
  // the UF_2 vertex array) -> applyTipEncoding (below; must run and
  // complete, as a barrier, BEFORE upwardPass/loadCache/the phase-3 walk,
  // so every particle copy the walk reads already carries the encoded tip;
  // see paratreet::uf2EncodeTip's comment) -> [walk emits encoded-tip edges
  // into edge_buf3 exactly as in v1/3a, no visitor changes needed] ->
  // initUF2 -> fireUF2Edges -> CkWaitQD -> UnionFindLib::find_components ->
  // applyUF2Labels.

  // Owner-writes rewrite of this PE's particles' tips through the node's
  // encode map (identity is never valid here: every registered particle's
  // tip must appear in frag_counts, hence in encode_map, since
  // countFragments enumerated exactly these tips on every PE of this
  // process before computeTipEncoding ran -- CkEnforce catches any
  // ordering regression).
  void applyTipEncoding(const CkCallback& cb) {
    auto& encode_map = node_proxy.ckLocalBranch()->encode_map;
    for (auto& s : subtrees) {
      for (int i = 0; i < s.n; i++) {
        auto it = encode_map.find(s.parts[i].group_number);
        CkEnforce(it != encode_map.end());
        s.parts[i].group_number = (long)it->second;
      }
    }
    this->contribute(cb);
  }

  // Wire the process-local UnionFindLib element: hand off the node's UF_2
  // vertex array (by pointer -- UnionFindLib mutates it in place, which is
  // how applyUF2Labels later reads back componentNumber with no additional
  // communication) and register the O(1) location decoder. Real work runs
  // only on the node's home PE (CkNodeFirst(CkMyNode())), the PE that hosts
  // this process's UnionFindLib array element (UFNodeMap placement); other
  // PEs of the process no-op (barrier still closes via contribute).
  void initUF2(CProxy_UnionFindLib uf_proxy, const CkCallback& cb) {
    if (CkMyPe() == CkNodeFirst(CkMyNode())) {
      auto* nb = node_proxy.ckLocalBranch();
      UnionFindLib* lib = uf_proxy[CkMyNode()].ckLocal();
      CkEnforce(lib != nullptr); // must be true on the element's home PE
      lib->registerGetLocationFromID(&paratreet::uf2LocationFromID);
      lib->initialize_vertices(nb->uf2_vertices.data(),
                               (int)nb->uf2_vertices.size());
    }
    this->contribute(cb);
  }

  // Submit this PE's buffered phase-3 edges (already encoded-tip pairs --
  // the walk emitted them post-encoding) as one batched union_requests
  // message to this process's UnionFindLib element; the library internally
  // routes each edge to its actual owner (see union_request/boss_send), so
  // any element works as the entry point. No-op (no message) if empty.
  void fireUF2Edges(CProxy_UnionFindLib uf_proxy, const CkCallback& cb) {
    if (!edge_buf3.empty()) {
      std::vector<UFEdge> edges;
      edges.reserve(edge_buf3.size());
      for (auto& e : edge_buf3) edges.push_back(UFEdge{(uint64_t)e.first, (uint64_t)e.second});
      uf_proxy[CkMyNode()].union_requests(edges);
    }
    this->contribute(cb);
  }

  // CkEnforce the owner-locality invariant the whole scheme depends on:
  // every registered particle's (encoded) tip must decode to THIS process
  // (node bits == CkMyNode()) with a dense index within this process's own
  // UF_2 vertex array. A violation here means applyTipEncoding ran against
  // stale/foreign state (ordering bug), not a UF_2 library bug.
  void verifyEncodedTips(const CkCallback& cb) {
    auto* nb = node_proxy.ckLocalBranch();
    long n_vertices = (long)nb->uf2_vertices.size();
    int my_node = CkMyNode();
    for (auto& s : subtrees) {
      for (int i = 0; i < s.n; i++) {
        uint64_t enc = (uint64_t)s.parts[i].group_number;
        CkEnforce(int(enc >> paratreet::kUF2IdxBits) == my_node);
        CkEnforce(long(enc & paratreet::kUF2IdxMask) < n_vertices);
      }
    }
    this->contribute(cb);
  }

  // Owner-writes rewrite from encoded tip to UnionFindLib's componentNumber,
  // read directly out of the node's uf2_vertices array (UnionFindLib wrote
  // componentNumber in place during find_components -- same storage
  // initUF2 handed it, no gather needed). Final labels are arbitrary
  // per-run serial ids (find_components' prefix-sum boss numbering), NOT
  // the "order of the min-order member" convention -u serial produces; the
  // fof3 harness canonicalizes both by re-deriving min order per label
  // group from the gathered records, so this is fine (design/step4.md,
  // decision 3).
  void applyUF2Labels(const CkCallback& cb) {
    auto* nb = node_proxy.ckLocalBranch();
    auto& verts = nb->uf2_vertices;
    for (auto& s : subtrees) {
      for (int i = 0; i < s.n; i++) {
        long idx = s.parts[i].group_number & (long)paratreet::kUF2IdxMask;
        CkEnforce(idx >= 0 && idx < (long)verts.size());
        long comp = verts[idx].componentNumber;
        CkEnforce(comp != -1);
        s.parts[i].group_number = comp;
      }
    }
    this->contribute(cb);
  }

  // --- Phase 3 (cross-process boundary walk; see src/FoFPhase3.h and
  // design/phase3.md). The buffers below are distinct from the phaseB
  // (cross-PE, same-process) buffers above to avoid any confusion between
  // the two edge namespaces.

  // Synchronous, non-entry: called via ckLocalBranch by FoFEdgeVisitor::leaf
  // during the phase-3 traversal. Traversal work for the Partitions homed on
  // this PE executes on this PE only (Charm++ entry methods of those chares),
  // so no lock is needed. Tips may be plain particle orders (step 1-3,
  // < 32 bits) or step-4 owner-encoded UF_2 ids (up to ~64 bits); the pair
  // key (paratreet::TipPairKey) stores both endpoints in full, so it is
  // correct either way.
  void addPhase3Edge(long ti, long tj) {
    phase3_emitted++;
    long lo = std::min(ti, tj), hi = std::max(ti, tj);
    if (seen3.insert(paratreet::packTipPair(lo, hi)).second)
      edge_buf3.emplace_back(lo, hi);
    if ((long)edge_buf3.size() > p3_peak_edge_buf)
      p3_peak_edge_buf = (long)edge_buf3.size();
  }

  // --- Phase-3a SEEN suppression (design/step3.md §1, §3): synchronous
  // forwards to the process-level table on FoFPhase1Node. Called via
  // ckLocalBranch by FoFEdgeVisitor during the traversal.
  bool trySeenInsert(paratreet::TipPairKey key) {
    return node_proxy.ckLocalBranch()->trySeenInsert(key);
  }
  bool seenContains(paratreet::TipPairKey key) {
    return node_proxy.ckLocalBranch()->seenContains(key);
  }
  // Record one redundant (pre-witness both-uniform) descent over (g,f) for the
  // per-pair concentration histogram (design/step3.md §6e).
  void recordRedundant(paratreet::TipPairKey key) {
    node_proxy.ckLocalBranch()->recordRedundant(key);
  }

  // Phase-3a per-PE counters (design/step3.md §6), plain members updated
  // synchronously by FoFEdgeVisitor via ckLocalBranch; reduced by
  // phase3Stats. Peak simultaneously-active node pairs is NOT tracked: the
  // traverser does not expose pair activation/retirement cheaply, so only
  // the edge-buffer high-water mark is reported.
  long p3_negative_prunes = 0;    // open: mindist2 > b2                [case 1]
  long p3_positive_prunes = 0;    // open: maxdist2 <= b2 certificate   [case 2]
  long p3_suppression_prunes = 0; // open/leaf: (g,f) already SEEN      [case 3]
  long p3_same_frag_prunes = 0;   // open/leaf: both uniform over the SAME tip
                                  // (no cross-process edge possible; 3a
                                  // addition, not in the spec's 3-case list)
  long p3_leaf_visits = 0;        // leaf() invocations
  long p3_redundant_descents = 0; // opens that descend while both-uniform
                                  // (unSEEN, distinct tips): §8.3 data
  long p3_peak_edge_buf = 0;      // high-water mark of edge_buf3.size()

  // Per-PE wall time of the phaseA/phaseB entry bodies (load-imbalance
  // signals; set in the entries above, reset by reset(), reduced min/avg/max
  // over PEs by phase3Stats). NOT cleared by resetPhase3: phase 1 runs
  // before the phase-3 reset each iteration.
  double t_phaseA = 0.0;
  double t_phaseB = 0.0;

  void resetPhase3(const CkCallback& cb) {
    edge_buf3.clear();
    seen3.clear();
    phase3_emitted = 0;
    p3_negative_prunes = 0;
    p3_positive_prunes = 0;
    p3_suppression_prunes = 0;
    p3_same_frag_prunes = 0;
    p3_leaf_visits = 0;
    p3_redundant_descents = 0;
    p3_peak_edge_buf = 0;
    node_proxy.ckLocalBranch()->clearSeen();
    node_proxy.ckLocalBranch()->clearNodeRedundant();
    node_proxy.ckLocalBranch()->clearRedun();
    this->contribute(cb);
  }

  // Gather-to-one completion pattern: a concat reduction. Every PE
  // contributes its (already per-PE-deduplicated) edge buffer as raw bytes;
  // the reduction tree delivers one message with all edges to the driver's
  // callback on PE 0. The reduction is the completion detection -- no
  // message counting, no broadcast/point-to-point ordering hazards.
  void flushPhase3Edges(const CkCallback& cb) {
    this->contribute(edge_buf3.size() * sizeof(std::pair<long, long>),
                     edge_buf3.data(), CkReduction::concat, cb);
  }

  // Edge + 3a-counter statistics, a tuple reduction:
  //   element 0 (sum over PEs), long[8]:
  //     [0] edges emitted (SEEN wins reaching addPhase3Edge)
  //     [1] edges sent to the gather (after per-PE dedup)
  //     [2] negative prunes  [3] positive-certificate prunes
  //     [4] suppression prunes  [5] same-fragment prunes
  //     [6] leaf visits  [7] redundant (both-uniform) descents
  //   element 1 (max over PEs), long: peak edge-buffer size.
  // Load-imbalance extension (min/avg/max over PEs; avg = sum/CkNumPes at
  // the consumer):
  //   element 2 (min over PEs), long[2]: [0] leaf visits [1] edges emitted
  //   element 3 (max over PEs), long[2]: same layout
  //   element 4 (sum over PEs), double[2]: [0] phaseA s [1] phaseB s
  //   element 5 (min over PEs), double[2]: same layout
  //   element 6 (max over PEs), double[2]: same layout
  void phase3Stats(const CkCallback& cb) {
    long sums[8] = {phase3_emitted, (long)edge_buf3.size(),
                    p3_negative_prunes, p3_positive_prunes,
                    p3_suppression_prunes, p3_same_frag_prunes,
                    p3_leaf_visits, p3_redundant_descents};
    long peak = p3_peak_edge_buf;
    // per_pe[0,1] are PER-PE (leaf visits, edges emitted); per_pe[2] is the
    // PER-PROCESS redundant-descent total (design/step3.md §6d) -- every PE of
    // a process reads the same deposited value, so min/max over PEs == min/max
    // over processes (the SMP trick memoryStats relies on). Requires
    // depositNodeRedundant to have run post-walk; avg-over-processes is
    // p3_redundant_descents-sum / CkNumNodes() at the consumer.
    long node_redundant = node_proxy.ckLocalBranch()->p3_node_redundant;
    long per_pe[3] = {p3_leaf_visits, phase3_emitted, node_redundant};
    double times[2] = {t_phaseA, t_phaseB};
    CkReduction::tupleElement tupleRedn[] = {
      CkReduction::tupleElement(sizeof(sums), sums, CkReduction::sum_long),
      CkReduction::tupleElement(sizeof(long), &peak, CkReduction::max_long),
      CkReduction::tupleElement(sizeof(per_pe), per_pe, CkReduction::min_long),
      CkReduction::tupleElement(sizeof(per_pe), per_pe, CkReduction::max_long),
      CkReduction::tupleElement(sizeof(times), times, CkReduction::sum_double),
      CkReduction::tupleElement(sizeof(times), times, CkReduction::min_double),
      CkReduction::tupleElement(sizeof(times), times, CkReduction::max_double)
    };
    CkReductionMsg* msg = CkReductionMsg::buildFromTuple(tupleRedn, 7);
    msg->setCallback(cb);
    this->contribute(msg);
  }

  // Deposit this PE's redundant-descent count into the per-process total
  // (design/step3.md §6d), then barrier via cb. Called once per PE between the
  // walk's QD and phase3Stats, so the process total is complete before
  // phase3Stats reads it. Deposit is a single locked add per PE (not
  // hot-path). p3_redundant_descents is per-PE; p3_node_redundant is the sum
  // over the process's PEs.
  void depositNodeRedundant(const CkCallback& cb) {
    node_proxy.ckLocalBranch()->addNodeRedundant(p3_redundant_descents);
    this->contribute(cb);
  }

  // Distributed tip-sentinel check: every registered (Subtree-owned)
  // particle must hold a valid tip, i.e. a global particle order in
  // [0, n_total). Phase 1 writes every registered particle, so an
  // out-of-range value means some copy was never touched. Runs on each PE
  // over its own particles (no gather), so it stays affordable at any N —
  // the fof3 harness runs it in both check modes.
  void verifyTips(long n_total, const CkCallback& cb) {
    for (auto& s : subtrees) {
      for (int i = 0; i < s.n; i++) {
        long tip = s.parts[i].group_number;
        CkEnforce(tip >= 0 && tip < n_total);
      }
    }
    this->contribute(cb);
  }

  // Per-PE memory usage (CmiMemoryUsage, bytes), tuple reduction:
  //   [0] min over PEs (long), [1] sum over PEs (long; avg at the consumer),
  //   [2] max over PEs (long).
  // In SMP builds CmiMemoryUsage is process-wide (every PE of a process
  // reports the same value); see paratreet::FoFMemoryStats.
  void memoryStats(const CkCallback& cb) {
    long mem = (long)CmiMemoryUsage();
    CkReduction::tupleElement tupleRedn[] = {
      CkReduction::tupleElement(sizeof(long), &mem, CkReduction::min_long),
      CkReduction::tupleElement(sizeof(long), &mem, CkReduction::sum_long),
      CkReduction::tupleElement(sizeof(long), &mem, CkReduction::max_long)
    };
    CkReductionMsg* msg = CkReductionMsg::buildFromTuple(tupleRedn, 3);
    msg->setCallback(cb);
    this->contribute(msg);
  }

  // Per-PE (label, count) pairs over the registered particles, concat-reduced
  // to the caller, which merges them into exact global component sizes (a
  // component spanning k PEs contributes k pairs). Gather volume is one pair
  // per distinct label per PE — far below the full particle gather on
  // clustered data (worst case, all-singleton labels, it degrades toward
  // 16 B/particle; the full record gather is 24 B/particle). Used by the
  // fof3 harness's stats mode via runFoFComponentHistogram.
  void collectLabelCounts(const CkCallback& cb) {
    std::unordered_map<long, long> counts;
    for (auto& s : subtrees)
      for (int i = 0; i < s.n; i++) counts[s.parts[i].group_number]++;
    std::vector<std::pair<long, long>> v(counts.begin(), counts.end());
    this->contribute(v.size() * sizeof(std::pair<long, long>),
                     v.data(), CkReduction::concat, cb);
  }

  // Owner-writes relabel through the global tip -> root map computed by the
  // serial UF_2 (identity if absent), same pattern as relabel(). Rewrites the
  // registered Subtree-owned particle blocks; with matching decompositions
  // the Partition target leaves alias these blocks, so they see the global
  // labels too.
  void applyGlobalMap(const std::vector<std::pair<long, long>>& map_vec,
                      const CkCallback& cb) {
    std::unordered_map<long, long> tip_map(map_vec.begin(), map_vec.end());
    for (auto& s : subtrees) {
      for (int i = 0; i < s.n; i++) {
        auto it = tip_map.find(s.parts[i].group_number);
        if (it != tip_map.end()) s.parts[i].group_number = it->second;
      }
    }
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
  // Dual tree walk over a pair of local trees; prunes on box gap distance
  // (negative certificate) AND on the positive certificate: if the boxes'
  // max distance is within b, EVERY cross particle pair is a guaranteed
  // link (for a == b self pairs: every internal pair — the box diameter),
  // so cert_fn(a, b) resolves the whole pair in O(n_a + n_b) with no
  // distance tests and the descent stops. This is design-note §4 case 2
  // applied intra-process; it is what makes dense regions near-LINEAR
  // (design/phase1-scaling.md: phaseA cost is pair work ~ density, and in
  // a core at overdensity D the linking length spans many local spacings,
  // so whole subtrees certify). Skipped under PBC — maxdist2 is not
  // periodic (same exclusion as the phase-3 case-2 certificate).
  // leaf_fn(a, b) is invoked on surviving leaf x leaf pairs. prune_fn(a, b)
  // returning true means the pair is already fully resolved (connectivity
  // suppression) — skip it entirely.
  template <typename LeafFn, typename CertFn, typename PruneFn>
  void walk(Node<Data>* a, Node<Data>* b, const LeafFn& leaf_fn,
            const CertFn& cert_fn, const PruneFn& prune_fn) {
    if (a == nullptr || b == nullptr) return;
    if (a->n_particles == 0 || b->n_particles == 0) return; // empty leaves
    if (paratreet::mindist2(a->data.box, b->data.box, period_) > b2_) return;
    if (prune_fn(a, b)) return;
    const bool pbc = period_.x > 0 || period_.y > 0 || period_.z > 0;
    // Cheap conservative gate before the full maxdist2 test: per axis the
    // farthest cross pair spans at least (s_a + s_b)/2, so maxdist2 >=
    // sum(((s_a+s_b)/2)^2) >= (sum(s_a+s_b))^2 / 12 (Cauchy-Schwarz). If
    // even that lower bound exceeds b^2 the certificate cannot fire — which
    // is the common case at subcritical b, where an ungated test costs ~10%
    // of phaseA for nothing. Never skips a valid certificate.
    const Real msum = boxMeasure(a) + boxMeasure(b);
    if (!pbc && msum * msum <= Real(12) * b2_ &&
        paratreet::maxdist2(a->data.box, b->data.box) <= b2_) {
      cert_fn(a, b);
      return;
    }
    if (a->isLeaf() && b->isLeaf()) {
      leaf_fn(a, b);
      return;
    }
    bool open_a;
    if (a->isLeaf()) open_a = false;
    else if (b->isLeaf()) open_a = true;
    else open_a = boxMeasure(a) >= boxMeasure(b); // open the larger box
    if (open_a) {
      for (int i = 0; i < a->n_children; i++)
        walk(a->getChild(i), b, leaf_fn, cert_fn, prune_fn);
    } else {
      for (int i = 0; i < b->n_children; i++)
        walk(a, b->getChild(i), leaf_fn, cert_fn, prune_fn);
    }
  }

  // Flat index (phaseA union-find space) of the first particle under n.
  // Only called on non-empty nodes, so the descent always terminates at a
  // non-empty leaf. NOTE: local-tree INTERNAL nodes carry n_particles = -1
  // BY DESIGN (Node.h: "non-leaves will have this as -1"); the qualifying
  // test must be != 0 (skip only known-empty EmptyLeafs), not > 0 — with
  // > 0 a deep dense chain (7 EmptyLeaf + 1 Internal(-1) per level, common
  // in LAMBS halos) never advances and this loop spins (the 2026-07-23
  // LAMBS hang). Safe because the build never creates an all-empty
  // Internal: empty regions are EmptyLeaf(0) children.
  int firstFlat(Node<Data>* n, const SubtreeRef& s) {
    while (!n->isLeaf()) {
      Node<Data>* next = nullptr;
      for (int i = 0; i < n->n_children; i++) {
        Node<Data>* c = n->getChild(i);
        if (c != nullptr && c->n_particles != 0) { next = c; break; }
      }
      if (next == nullptr) {
        // Inconsistent tree: a non-leaf claiming particles but no non-empty
        // child. Abort loudly rather than spin (LAMBS debug 2026-07-23).
        CkPrintf("[pe %d] firstFlat STUCK: node key %" PRIx64 " type %d "
                 "n_particles %d n_children %d children:",
                 CkMyPe(), (uint64_t)n->key, (int)n->type, n->n_particles,
                 n->n_children);
        for (int i = 0; i < n->n_children; i++) {
          Node<Data>* c = n->getChild(i);
          if (c == nullptr) CkPrintf(" null");
          else CkPrintf(" (t%d,n%d)", (int)c->type, c->n_particles);
        }
        CkPrintf("\n");
        CkAbort("firstFlat: no non-empty child under a non-empty non-leaf");
      }
      n = next;
    }
    return s.offset + int(n->particles() - s.parts);
  }

  // Positive-certificate action for phaseA: unite every particle under n
  // into rep. Repeat certificates over an already-merged subtree degrade to
  // path-compressed finds (~O(1) each).
  void uniteSubtree(Node<Data>* n, const SubtreeRef& s, int rep) {
    if (n == nullptr || n->n_particles == 0) return;
    if (n->isLeaf()) {
      int f = s.offset + int(n->particles() - s.parts);
      for (int i = 0; i < n->n_particles; i++) unite(rep, f + i);
      return;
    }
    for (int i = 0; i < n->n_children; i++) uniteSubtree(n->getChild(i), s, rep);
  }

  // Hierarchical-fragment memo (Kale, 2026-07-23): the first certificate
  // involving node n star-unifies n's particles ONCE and records the
  // representative — n has become a "fragment" — so every LATER certificate
  // involving n is a single unite(rep, rep). Without this, a hot node with
  // k certified neighbors re-walks its particles k times (measured: the
  // unmemoized certificate bought only ~10% at 8M b0.8; the pair work in a
  // dense core is dominated by exactly those repeats). Valid because
  // certRep is only ever called for a node participating in a certified
  // pair: all cross pairs are genuine links, so the star through the
  // partner connects n internally even when n alone is not a clique.
  // Cleared at the start of each phaseA (node pointers are walk-scoped).
  //
  // The SAME map doubles as the connectivity memo for suppression
  // (connectedRep below): an entry means "internally connected, with this
  // representative" — monotone, so it never invalidates; find(rep) always
  // yields the CURRENT root even after later merges.
  std::unordered_map<Node<Data>*, int> cert_rep;
  int certRep(Node<Data>* n, const SubtreeRef& s) {
    auto it = cert_rep.find(n);
    if (it != cert_rep.end()) return it->second;
    int rep = firstFlat(n, s);
    uniteSubtree(n, s, rep);
    cert_rep.emplace(n, rep);
    return rep;
  }

  // Connectivity query with lazy bottom-up upgrade (the suppression layer,
  // design/phase1-scaling.md): returns a representative flat index if n is
  // CURRENTLY internally connected, else -1. Positives are memoized in
  // cert_rep (monotone — never invalidated). NON-recursive on internals:
  // it consults only the CHILDREN'S memo entries, so connectivity
  // percolates upward across successive queries as the walk revisits nodes
  // against new partners ("frequent path compression" at node granularity).
  long p1_conn_suppressed = 0; // pairs pruned by connectivity suppression
  int connectedRep(Node<Data>* n, const SubtreeRef& s) {
    auto it = cert_rep.find(n);
    if (it != cert_rep.end()) return it->second;
    // No negative memo: a FAILED check is cheap by construction — the leaf
    // path exits on the first root mismatch (~2 finds; in subcritical
    // regions leaf particles rarely share a root), and the internal path
    // exits on the first un-memoized child (~1 hash lookup). Both are
    // cheaper than the negative-memo bookkeeping they would avoid (an
    // exact-epoch memo cost +140% phaseA on 8M uniform b0.2; a backoff
    // memo blocked fresh suppressions and cost 1.5x at 8M b0.8).
    int rep = -1;
    if (n->isLeaf()) {
      if (n->n_particles <= 0) return -1;
      int f = s.offset + int(n->particles() - s.parts);
      rep = find(f);
      for (int i = 1; i < n->n_particles; i++)
        if (find(f + i) != rep) return -1;
    } else {
      for (int i = 0; i < n->n_children; i++) {
        Node<Data>* c = n->getChild(i);
        if (c == nullptr || c->n_particles == 0) continue; // empty leaf
        auto ci = cert_rep.find(c);
        if (ci == cert_rep.end()) return -1;
        int r = find(ci->second);
        if (rep < 0) rep = r;
        else if (r != rep) return -1;
      }
      if (rep < 0) return -1; // no live child
    }
    cert_rep.emplace(n, rep);
    return rep;
  }

  // phaseB memo, same idea as cert_rep: first certificate touching node n
  // star-emits n's tips once; later certificates emit one (rep, rep) edge.
  // Cleared at the start of each phaseB.
  std::unordered_map<Node<Data>*, long> cert_tip;
  long certTipRep(Node<Data>* n) {
    auto it = cert_tip.find(n);
    if (it != cert_tip.end()) return it->second;
    long rep = firstTip(n);
    emitSubtreeTips(n, rep);
    cert_tip.emplace(n, rep);
    return rep;
  }

  // Positive-certificate action for phaseB (frozen tips): emit deduplicated
  // (rep_tip, tip) edges for every particle under n. Correct without any
  // internal-connectivity assumption: all CROSS pairs are true links, so a
  // spanning star through rep connects every tip present in a and b.
  void emitSubtreeTips(Node<Data>* n, long rep_tip) {
    if (n == nullptr || n->n_particles == 0) return;
    if (n->isLeaf()) {
      const Particle* p = n->particles();
      for (int i = 0; i < n->n_particles; i++) {
        long t = p[i].group_number;
        if (t == rep_tip) continue;
        long lo = std::min(rep_tip, t), hi = std::max(rep_tip, t);
        uint64_t key = (uint64_t(uint32_t(lo)) << 32) | uint64_t(uint32_t(hi));
        if (seen.insert(key).second) edge_buf.emplace_back(lo, hi);
      }
      return;
    }
    for (int i = 0; i < n->n_children; i++) emitSubtreeTips(n->getChild(i), rep_tip);
  }

  // First (any) particle tip under n; n non-empty. Same != 0 rule as
  // firstFlat (internal n_particles is -1 by design).
  long firstTip(Node<Data>* n) {
    while (!n->isLeaf()) {
      Node<Data>* next = nullptr;
      for (int i = 0; i < n->n_children; i++) {
        Node<Data>* c = n->getChild(i);
        if (c != nullptr && c->n_particles != 0) { next = c; break; }
      }
      if (next == nullptr)
        CkAbort("firstTip: no non-empty child under a non-empty non-leaf");
      n = next;
    }
    return n->particles()[0].group_number;
  }

  static Real boxMeasure(Node<Data>* n) {
    auto sz = n->data.box.size();
    return sz.x + sz.y + sz.z;
  }

  // phaseA leaf action: pairwise distance checks -> union. When BOTH leaves
  // are already internally connected fragments (walk-level suppression has
  // ruled out same-component pairs), a single witness merges everything —
  // return after the first hit (phase 3's uniform-leaf-pair shortcut).
  void leafLeafUnion(Node<Data>* a, Node<Data>* b,
                     const SubtreeRef& sa, const SubtreeRef& sb) {
    const Particle* pa = a->particles();
    const Particle* pb = b->particles();
    int fa = sa.offset + int(pa - sa.parts);
    int fb = sb.offset + int(pb - sb.parts);
    if (a == b) {
      for (int i = 0; i < a->n_particles; i++)
        for (int j = i + 1; j < a->n_particles; j++)
          if (paratreet::periodicDistSq(pa[i].position, pa[j].position, period_) <= b2_)
            unite(fa + i, fa + j);
    } else {
      const bool one_witness =
          connectedRep(a, sa) >= 0 && connectedRep(b, sb) >= 0;
      for (int i = 0; i < a->n_particles; i++)
        for (int j = 0; j < b->n_particles; j++)
          if (paratreet::periodicDistSq(pa[i].position, pb[j].position, period_) <= b2_) {
            unite(fa + i, fb + j);
            if (one_witness) return;
          }
    }
  }

  // phaseB leaf action: pairwise distance checks -> deduplicated tip edges.
  void leafLeafEmit(Node<Data>* a, Node<Data>* b) {
    const Particle* pa = a->particles();
    const Particle* pb = b->particles();
    for (int i = 0; i < a->n_particles; i++) {
      for (int j = 0; j < b->n_particles; j++) {
        if (paratreet::periodicDistSq(pa[i].position, pb[j].position, period_) > b2_) continue;
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
  // Phase-3 cross-process buffers (kept separate from phaseB's, above).
  std::vector<std::pair<long, long>> edge_buf3;
  std::unordered_set<paratreet::TipPairKey, paratreet::TipPairKeyHash> seen3;
  long phase3_emitted = 0;
  double b2_ = 0.0;
  // Box period for PBC (design/pbc.md); {0,0,0} = open (default).
  Vector3D<Real> period_ = Vector3D<Real>(0, 0, 0);
};

namespace paratreet {

// Per-stage wall times of runFoFPhase1 (barrier-to-barrier on the driving
// thread, so each includes its reduction latency and is bounded by the
// SLOWEST PE/process — the right decomposition for the phase-1 scaling
// question, design/step3.md 6h: which stage stops speeding up with P).
struct FoFPhase1Stages {
  double reset = 0, register_s = 0, phaseA = 0, phaseB = 0, merge = 0,
         relabel = 0;
};

// Convenience driver for the full phase-1 sequence. Must be called from a
// [threaded] entry method (uses blocking callbacks), between tree build and
// the next rebuild/reset (registered particle blocks must stay alive).
// `stages`, if non-null, receives the per-stage wall times.
template <typename Data>
void runFoFPhase1(CProxy_Subtree<Data> subtrees,
                  CProxy_FoFPhase1<Data> fof,
                  CProxy_FoFPhase1Node<Data> fof_node,
                  double linking_length,
                  Vector3D<Real> period = Vector3D<Real>(0, 0, 0),
                  FoFPhase1Stages* stages = nullptr) {
  double b2 = linking_length * linking_length;
  FoFPhase1Stages local;
  double t = CkWallTimer();
  fof_node.reset(CkCallbackResumeThread());
  fof.reset(CkCallbackResumeThread());
  // PBC (design/pbc.md): broadcast the box period to every PE branch before
  // phaseA. Default {0,0,0} = open boundaries (exact current behavior).
  fof.setPeriod(period, CkCallbackResumeThread());
  local.reset = CkWallTimer() - t; t = CkWallTimer();
  subtrees.registerFoF(fof, CkCallbackResumeThread());
  local.register_s = CkWallTimer() - t; t = CkWallTimer();
  fof.phaseA(b2, CkCallbackResumeThread());
  local.phaseA = CkWallTimer() - t; t = CkWallTimer();
  fof.phaseB(b2, CkCallbackResumeThread());
  local.phaseB = CkWallTimer() - t; t = CkWallTimer();
  fof_node.merge(CkCallbackResumeThread());
  local.merge = CkWallTimer() - t; t = CkWallTimer();
  fof.relabel(CkCallbackResumeThread());
  local.relabel = CkWallTimer() - t;
  if (stages) *stages = local;
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

// Distributed tip-sentinel check (see FoFPhase1::verifyTips). Blocks until
// every PE has checked its registered particles; a bad tip trips CkEnforce
// on the owning PE. Same threaded-context requirements as runFoFPhase1.
template <typename Data>
void runFoFVerifyTips(CProxy_FoFPhase1<Data> fof, long n_total) {
  fof.verifyTips(n_total, CkCallbackResumeThread());
}

// Step 4 counterpart of runFoFVerifyTips: checks the owner-encoded UF_2
// invariant (FoFPhase1::verifyEncodedTips) instead of the [0, n_total)
// particle-order sentinel, which encoded tips do not satisfy.
template <typename Data>
void runFoFVerifyEncodedTips(CProxy_FoFPhase1<Data> fof) {
  fof.verifyEncodedTips(CkCallbackResumeThread());
}

// Fragment-size histogram, node-side only: for step 4 (-u dist), frag_counts
// is already populated by an earlier fof.countFragments() call (before
// computeTipEncoding, see design/step4.md), so re-invoking countFragments
// here (as runFoFFragmentHistogram above does) would double-count. Callers
// on the dist path use this instead.
template <typename Data>
FoFFragmentHistogram runFoFFragmentHistogramNode(CProxy_FoFPhase1Node<Data> fof_node) {
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

// Per-PE memory usage, reduced min/avg/max (see FoFPhase1::memoryStats).
// Blocks and returns the reduced stats; call from a [threaded] context.
template <typename Data>
FoFMemoryStats runFoFMemoryStats(CProxy_FoFPhase1<Data> fof) {
  void* result = nullptr;
  fof.memoryStats(CkCallbackResumeThread(result));
  CkReductionMsg* msg = (CkReductionMsg*)result;
  CkReduction::tupleElement* elems = nullptr;
  int n_elems = 0;
  msg->toTuple(&elems, &n_elems);
  CkEnforce(n_elems == 3);
  FoFMemoryStats s;
  s.min_bytes = *(const long*)elems[0].data;
  s.avg_bytes = (double)*(const long*)elems[1].data / (double)CkNumPes();
  s.max_bytes = *(const long*)elems[2].data;
  delete[] elems;
  delete msg;
  return s;
}

// Global component-size histogram over the final labels WITHOUT a particle
// gather (see FoFPhase1::collectLabelCounts): concat-gather the per-PE
// (label, count) pairs to this thread, merge to exact global sizes, and
// log2-bin them with the same binning as the fragment histogram. Run after
// the phase-3 relabel (labels must be global); call from a [threaded]
// context on PE 0. This is the stats-mode determinism observable: for a
// given input it must be bit-identical across process/PE configurations.
template <typename Data>
FoFComponentHistogram runFoFComponentHistogram(CProxy_FoFPhase1<Data> fof,
                                               int min_component_size = 0) {
  void* result = nullptr;
  fof.collectLabelCounts(CkCallbackResumeThread(result));
  CkReductionMsg* msg = (CkReductionMsg*)result;
  int n_pairs = msg->getSize() / sizeof(std::pair<long, long>);
  const auto* pairs = (const std::pair<long, long>*)msg->getData();
  std::unordered_map<long, long> counts;
  counts.reserve((size_t)n_pairs);
  for (int i = 0; i < n_pairs; i++) counts[pairs[i].first] += pairs[i].second;
  delete msg;

  FoFComponentHistogram h;
  std::memset(h.bins, 0, sizeof(h.bins));
  std::memset(h.surviving_bins, 0, sizeof(h.surviving_bins));
  h.n_components = 0;
  h.max_size = 0;
  h.surviving_count = 0;
  h.surviving_max_size = 0;
  h.min_component_size = min_component_size;
  for (auto& kv : counts) {
    long size = kv.second;
    int bin = 0; // floor(log2(size)); size >= 1 always
    while (bin < 63 && (1L << (bin + 1)) <= size) bin++;
    h.bins[bin]++;
    h.n_components++;
    if (size > h.max_size) h.max_size = size;
    // Reporting filter (step 5): survivors are components with size >= m,
    // tallied from the same merged counts (no extra gather/reduction).
    if (size >= (long)min_component_size) {
      h.surviving_bins[bin]++;
      h.surviving_count++;
      if (size > h.surviving_max_size) h.surviving_max_size = size;
    }
  }
  return h;
}

} // namespace paratreet

#endif // PARATREET_FOFPHASE1_H_
