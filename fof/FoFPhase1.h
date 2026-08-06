#ifndef PARATREET_FOFPHASE1_H_
#define PARATREET_FOFPHASE1_H_

// FoF phase 1: intra-process friends-of-friends (see design/phase1.md).
//
// Per process, computes the connected components of the linking graph
// restricted to the process's own particles ("process-level tips"), writing
// the tip id of every particle into Particle::group_number. Tip id = global
// particle id (`order`) of the component's min-order particle, so PE-tips,
// process-level tips and later UF_2 vertices share one namespace.
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
//       PE-tip -> process-level tip map.
//   (d) FoFPhase1 relabel: each PE rewrites its own particles' group_number
//       through the map (identity if absent). Owners write; no contention.
//
// No atomics anywhere: every sub-phase either writes only PE-owned data or
// reads only data frozen by the preceding barrier.
//
// Driving sequence (from a [threaded] context; see paratreet::runFoFPhase1):
//   fof_node.reset -> fof.reset -> subtrees.callPerSubtreeFn(SubtreeRegisterFn)
//   -> fof.phaseA -> fof.phaseB -> fof_node.merge -> fof.relabel
// Optionally after relabel (see paratreet::runFoFFragmentHistogram):
//   fof.countFragments -> fof_node.fragmentHistogram
//
// Lifetime contract: the Particle blocks registered through the generic
// Subtree::callPerSubtreeFn hook (fof::SubtreeRegisterFn hands each Subtree
// element's block to its PE's FoFPhase1 branch) are stable from the end of
// tree build until the next rebuild/reset; run the whole sequence inside
// that window.

#include "FoFStealTypes.h"
#include "fof.decl.h"
// Template definitions of this module's generated code (CBase_/closure/proxy
// templates from fof.def.h) must be visible before any concrete-Data use of
// the chares (e.g. FoFPhase3.h's FragData visitors) — same idiom as the
// core's Subtree.h including templates.h.
#include "fof-templates.h"
#include "CoreFunctions.h"
#include "common.h"
#include "Node.h"
#include "Particle.h"
#include "OrientedBox.h"
#include "unionFindLib.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <map>
#include <atomic>
#include <cstdio>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#include <functional>
#include <mutex>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace paratreet {

// Step 4 (distributed UF_2, design/step4.md "Tip encoding"): owner-encoded
// vertex namespace for UF_2. A process-level tip is renumbered to
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
// 43/20 split (sparse-uf2, 2026-07-25): the local field holds a RAW
// particle order (enumeration-free encoding), so kUF2IdxBits >= log2(N):
// 43 bits = 8.8e12 particles. 20 process bits = 1,048,576 processes. The
// remaining top bit is the SIGN bit of the (signed long) group_number and
// must stay clear: negative values are reserved for the -1 "never
// labeled" sentinel and for final component labels (-(comp+2)), which
// keeps the label namespace disjoint from untouched fragments' encoded
// tips without any enumeration.
constexpr int kUF2IdxBits = 43;
constexpr int kUF2ProcBits = 20;
constexpr uint64_t kUF2IdxMask = (uint64_t(1) << kUF2IdxBits) - 1;
static_assert(kUF2IdxBits + kUF2ProcBits <= 63,
              "encoded tips must leave the sign bit of long clear");

inline uint64_t uf2EncodeTip(int process, long dense_index) {
  return (uint64_t(uint32_t(process)) << kUF2IdxBits) | (uint64_t(dense_index) & kUF2IdxMask);
}

// Registered with UnionFindLib::registerGetLocationFromID. Must be a plain
// function (not a capturing lambda): the library stores it as a raw
// std::pair<int,int>(*)(uint64_t) function pointer.
inline uint64_t uf2MakeVertexID(int chare, uint64_t localId) {
  return uf2EncodeTip(chare, (long)localId);
}

inline std::pair<int, uint64_t> uf2LocationFromID(uint64_t vid) {
  return { int(vid >> kUF2IdxBits), vid & kUF2IdxMask };
}

// Fair phaseB work division (design/phase1-scaling.md, 2026-07-25): each
// unordered subtree pair spanning two PEs of a process is walked by
// exactly one of the two, chosen by one bit of a symmetric mix of the two
// subtree ROOT KEYS (stable Morton keys — identical on both sides, so
// both PEs agree without communication; pointers would vary under ASLR).
// Subtree granularity splits every PE pair's ~64 subtree pairs about in
// half with density mixing — the lower-PE-walks-everything rule gave PE i
// of an N-PE process N-1-i partner PEs (triangular; ~11x phaseB skew in
// the 80M logs). The emitted edge SET is unchanged (merge unions are
// idempotent to the cross-walker duplicates that already existed).
inline int phaseBWalker(Key ka, Key kb, int p, int q) {
  uint64_t lo = std::min<uint64_t>(ka, kb), hi = std::max<uint64_t>(ka, kb);
  uint64_t h = lo * 0x9E3779B97F4A7C15ull;
  h ^= hi + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 31;
  return (h & 1) ? std::min(p, q) : std::max(p, q);
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
  long n_fragments; // total fragment (process-level tip) count
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

// Result of the per-PE memory reduction (process RSS from the OS; see
// FoFPhase1::processRSSBytes — CmiMemoryUsage is only the fallback, it
// returns 0 on reconverse). RSS is PROCESS-wide, so every PE of a process
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
// merge() — the PE-tip -> process-level tip map read by the group branches.
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
  std::unordered_map<long, long> tip_map;   // PE-tip -> process-level tip
  std::unordered_map<long, long> frag_counts; // process-level tip -> exact size

  // Step 4 (distributed UF_2): built by computeTipEncoding() from
  // frag_counts (so it must run after countFragments). encode_map maps this
  // process's own process-level tips to their encoded UF_2 vertex ids
  // (paratreet::uf2EncodeTip); uf2_vertices is the vertex array handed to
  // UnionFindLib::initialize_vertices by index (dense_index == its position
  // here) -- UnionFindLib mutates componentNumber/parent/size IN PLACE in
  // this same storage, so applyUF2Labels reads results straight out of it.
  std::unordered_map<long, long> encode_map; // process-level tip -> encoded tip
  std::vector<unionFindVertex> uf2_vertices; // dense path only (unused by sparse-uf2)
  // Lazy-mode label readback buffer (collectUF2Labels -> applyUF2Labels):
  // localId -> componentNumber for every touched vertex of this process.
  std::unordered_map<uint64_t, long> uf2_labels;

  // Phase-3a SEEN table (design/step3.md §1, §3): process-level set of
  // packed (g, f) fragment pairs for which an edge has been (or is being)
  // emitted. Process level is sufficient for suppression because in the
  // transposed traversal the target fragment f is always process-local, so
  // every node pair over (g, f) is generated on f's owner process. Guarded
  // by its own mutex (single mutex is fine for 3a; stripe if contention
  // shows). Called synchronously from FoFEdgeVisitor via ckLocalBranch.
  // Component-histogram intra-process reduce-scatter buffers
  // (design/phase1-scaling.md "distributed component histogram",
  // 2026-07-25): PEs deposit (label, count) pairs bucketed by label hash;
  // the PE owning shard r merges bucket r. Per-shard mutexes: contention
  // is one append per depositing PE per shard.
  struct LabelShard {
    std::mutex m;
    std::vector<std::pair<long, long>> pairs;
  };
  std::vector<std::unique_ptr<LabelShard>> label_shards;
  std::mutex label_shards_init_lock;
  void ensureLabelShards(int n) {
    std::lock_guard<std::mutex> g(label_shards_init_lock);
    if ((int)label_shards.size() != n) {
      label_shards.clear();
      for (int i = 0; i < n; i++)
        label_shards.emplace_back(new LabelShard);
    }
  }

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

  // Within-process chain state (design/phase1-scaling.md, 2026-07-25):
  // phaseA..relabel are sequenced per process by deposit counters instead
  // of global reductions. chain_t0 is stamped by the first PE entering the
  // chain; stage walls are process-local (no barrier latency).
  std::atomic<int> chain_started{0};
  std::atomic<int> a_done{0};
  std::atomic<int> b_done{0};
  double chain_t0 = 0, stage_tA = 0, stage_tB = 0, stage_tM = 0;

  // --- phaseB cross-process stealing (design/phaseb-offload.md stages
  // 1-2, same-machine tier; Kale, 2026-08-05). A pool unit is a pure
  // function over frozen data (subtree pair snapshots -> tip edges), so
  // an idle process can execute it and return the edges. The victim's
  // merge must wait for both its own PEs' deposits AND every stolen
  // batch's return; tryTriggerMerge is the single gate. ORDER MATTERS on
  // the steal-server side: steal_outstanding is incremented BEFORE
  // claiming from the cursor, so the last local depositor can never see
  // a drained pool with claimed-but-unaccounted units.
  std::atomic<long> steal_outstanding{0};
  std::atomic<bool> phaseb_pool_ready{false}; // pool built (phaseA complete)
  std::atomic<bool> pool_deposits_done{false};
  std::atomic<bool> merge_fired{false};
  std::atomic<long> stolen_out_units{0};   // units shipped to helpers
  std::atomic<long> stolen_in_units{0};    // units executed for others
  std::atomic<long> steal_denials{0};      // requests answered empty
  CProxy_FoFPhase1<Data> group_proxy;      // set at chain start (for the fan)

  // Fire the merge + relabel fan exactly once, when local deposits are
  // complete and no stolen batch is outstanding. Callable from the last
  // depositor, from every returned batch, and from the deny path.
  void tryTriggerMerge() {
    if (!pool_deposits_done.load(std::memory_order_acquire)) return;
    if (steal_outstanding.load(std::memory_order_acquire) != 0) return;
    // The pool must also be fully CLAIMED: implied by local deposits in
    // normal runs (a PE deposits only after cursor exhaustion), but made
    // explicit so a pool drained partly (or, in the FOF_STEAL_TEST debug
    // mode, entirely) by helpers cannot merge before its last unit's
    // edges are home.
    if (phaseb_next.load(std::memory_order_acquire) < phaseb_pool.size())
      return;
    if (merge_fired.exchange(true)) return;
    if (stolen_out_units.load() > 0 || stolen_in_units.load() > 0) {
      CkPrintf("FOF3STAT steal: process %d out %ld in %ld denials %ld\n",
               CkMyNode(), stolen_out_units.load(), stolen_in_units.load(),
               steal_denials.load());
    }
    double tm0 = CkWallTimer();
    mergeBody();
    stage_tM = CkWallTimer() - tm0;
    int first = CkNodeFirst(CkMyNode());
    for (int pe = first; pe < first + CkNodeSize(CkMyNode()); pe++)
      group_proxy[pe].relabelChained(stage_tA, stage_tB, stage_tM);
  }
  // PhaseB dynamic pool (branch phaseb-pool): every cross-PE subtree pair
  // of this process, enumerated once by the last phaseA finisher and
  // CLAIMED IN CHUNKS by whichever PEs are free — replacing the static
  // per-pair hash assignment. Safe because a phaseB pair only reads
  // frozen data and emits edges into the executing PE's own buffer (the
  // merge is idempotent to cross-PE duplicate edges); any PE may execute
  // any pair. The pool converts phaseB stragglers into work absorbed by
  // idle sibling PEs (the trailing per-PE streaks in the 480-PE
  // timeline view, 2026-07-26).
  struct PoolUnit {
    double key; // LPT order: ascending = costliest-first (see poolPush)
    Node<Data>* a;
    Node<Data>* b;
  };
  std::vector<PoolUnit> phaseb_pool;
  std::atomic<size_t> phaseb_next{0};

  void reset(const CkCallback& cb) {
    label_shards.clear();
    phaseb_pool.clear();
    phaseb_next = 0;
    chain_started = 0;
    a_done = 0;
    b_done = 0;
    chain_t0 = stage_tA = stage_tB = stage_tM = 0;
    steal_outstanding = 0;
    phaseb_pool_ready = false;
    pool_deposits_done = false;
    merge_fired = false;
    stolen_out_units = 0;
    stolen_in_units = 0;
    steal_denials = 0;
    pe_subtrees.clear();
    edges.clear();
    tip_map.clear();
    frag_counts.clear();
    encode_map.clear();
    uf2_vertices.clear();
    uf2_labels.clear();
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

  // One execution per process: serial union-find over the boundary edges;
  // touches only tips that appear in edges. Called inline by the last PE
  // to finish phaseB (all edge buffers are in, so access is exclusive);
  // no longer a broadcast entry.
  void mergeBody() {
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

  // Synchronous, called on this PE by fof::SubtreeRegisterFn (delivered per
  // Subtree element through the generic Subtree::callPerSubtreeFn hook).
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
    t_phaseB_maxpair = 0.0;
    t_phaseB_units = 0;
    density_x = 0.0;
    phase3_emitted = 0;
    uf2_stream_batch = 0;
    p3_edges_streamed = 0;
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
  void phaseABody(double b2) {
    double t0 = CkWallTimer();
    b2_ = b2;
    // Offset table: flat index space over this PE's particle blocks.
    int n_local = 0;
    density_x = 0.0;
    for (auto& s : subtrees) {
      s.offset = n_local;
      n_local += s.n;
      double vol = (double)s.root->data.box.volume();
      if (vol > 0) density_x += (double)s.n * (double)s.n / vol;
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
          // Per-chare grid (design/phase1-scaling.md "grid phaseA",
          // 2026-07-25): a dense chare's SELF pair is solved by a cell
          // grid in ~O(n) instead of the tree walk. Gated on occupancy
          // (expected particles per cell) at the chare root — the walk
          // with its certificates stays the right tool in sparse chares
          // and for all cross-chare pairs.
          if (i == j && grid_thresh_ > 0 && sa.n >= 64) {
            double vol = (double)sa.root->data.box.volume();
            double b = std::sqrt(b2_);
            double c = b / std::sqrt(6.0);
            if (vol > 0 && (double)sa.n * c * c * c / vol >= grid_thresh_ &&
                gridSelfUnion(sa))
              continue;
          }
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
  }

  // (b) Cross-PE edge emission. For each subtree pair spanning this PE and a
  // higher PE of the same process, walk the pair over frozen data and emit
  // deduplicated (tip, tip) edges into this PE's buffer; hand the buffer to
  // the nodegroup. No-op when this process has a single PE (non-SMP or
  // one-PE-per-process runs).
  void phaseBBody(double b2) {
    double t0 = CkWallTimer();
    b2_ = b2;
    t_phaseB_maxpair = 0.0;
    t_phaseB_units = 0;
    edge_buf.clear();
    seen.clear();
    cert_tip.clear();
    auto* nb = node_proxy.ckLocalBranch();
    // Claim units from the process-wide pool until it drains (dynamic
    // self-scheduling; supersedes the static symmetric-hash assignment —
    // see the pool comment on FoFPhase1Node). Unit claims (chunk 1):
    // the pool is LPT-sorted, so consecutive units are the costliest —
    // chunked claims would stack them on one PE. Atomic traffic is one
    // fetch_add per unit, a few thousand per PE.
    // Validation mode (FOF_STEAL_TEST=1, debug only): odd processes skip
    // local claiming so their pools drain ONLY through steals — forces
    // the ship/rebuild/return path and the merge's outstanding-wait.
    static const bool steal_test = [] {
      const char* e = std::getenv("FOF_STEAL_TEST");
      return e && std::atoi(e) != 0;
    }();
    const bool skip_local = steal_test && (CkMyNode() % 2 == 1);
    const size_t CHUNK = 1;
    for (; !skip_local;) {
      size_t start = nb->phaseb_next.fetch_add(CHUNK);
      if (start >= nb->phaseb_pool.size()) break;
      size_t end = std::min(start + CHUNK, nb->phaseb_pool.size());
      for (size_t k = start; k < end; k++) {
          t_phaseB_units++;
          static const bool steal_selftest = [] {
            const char* e = std::getenv("FOF_STEAL_SELFTEST");
            return e && std::atoi(e) != 0;
          }();
          if (steal_selftest)
            selftestStolenUnit(nb->phaseb_pool[k].a, nb->phaseb_pool[k].b);
          double tp0 = CkWallTimer();
          walk(nb->phaseb_pool[k].a, nb->phaseb_pool[k].b,
               [&](Node<Data>* a, Node<Data>* b) { leafLeafEmit(a, b); },
               [&](Node<Data>* a, Node<Data>* b) {
                 // Positive certificate over frozen tips, memoized: each
                 // node star-emits once; the pair contributes one edge
                 // (a != b always here — the pair spans two PEs).
                 long ta = certTipRep(a);
                 long tb = certTipRep(b);
                 if (ta != tb) {
                   long lo = std::min(ta, tb), hi = std::max(ta, tb);
                   if (seen.insert(paratreet::packTipPair(lo, hi)).second)
                     edge_buf.emplace_back(lo, hi);
                 }
               },
               // No connectivity suppression in phaseB: there is no live UF
               // over frozen tips; the seen-set dedup plays that role at
               // edge granularity.
               [](Node<Data>*, Node<Data>*) { return false; });
          // Max single-unit wall: the divisibility diagnostic. If this
          // reduces to ~= the phaseB wall, an indivisible unit remains
          // and the split needs another level; if it collapses, the
          // leveling is complete (design/phase1-scaling.md).
          double tp = CkWallTimer() - tp0;
          if (tp > t_phaseB_maxpair) t_phaseB_maxpair = tp;
      }
    }
    if (!edge_buf.empty()) nb->submitEdges(std::move(edge_buf));
    edge_buf.clear();
    seen.clear();
    t_phaseB = CkWallTimer() - t0; // per-PE load signal, reduced by phase3Stats
  }

  // (d) Rewrite this PE's particles' group_number through the merge map
  // (identity if absent).
  void relabelBody() {
    auto& tip_map = node_proxy.ckLocalBranch()->tip_map;
    for (auto& s : subtrees) {
      for (int i = 0; i < s.n; i++) {
        auto it = tip_map.find(s.parts[i].group_number);
        if (it != tip_map.end()) s.parts[i].group_number = it->second;
      }
    }
  }

  // --- Within-process chain (design/phase1-scaling.md, 2026-07-25).
  // Every stage of phaseA -> phaseB -> merge -> relabel reads and writes
  // only process-local data (tips are global particle orders taken from
  // particle data), so the four global reductions the driver used to
  // interpose are replaced by per-process deposit counters on the node
  // branch: each PE deposits stage completion; the LAST depositor triggers
  // the next stage on the process's PEs. Cross-PE visibility is carried by
  // the deposit chain itself (the last fetch_add on the shared counter
  // synchronizes with every earlier one, so all of the process's phaseA
  // writes happen-before the phaseB trigger) plus message delivery. One
  // global reduction remains, at relabel end, carrying the process-local
  // stage walls (max-reduced) to the driver. A dense process no longer
  // holds every other process at each stage boundary.

  void startPhase1Chain(double b2, double grid_thresh, const CkCallback& done) {
    done_cb_ = done;
    grid_thresh_ = grid_thresh;
    auto* nb = node_proxy.ckLocalBranch();
    if (nb->chain_started.fetch_add(1) == 0) nb->chain_t0 = CkWallTimer();
    nb->group_proxy = this->thisProxy; // for the merge-time relabel fan
    phaseABody(b2);
    if (nb->a_done.fetch_add(1) + 1 == CkNodeSize(CkMyNode())) {
      nb->stage_tA = CkWallTimer() - nb->chain_t0;
      // Enumerate the process's phaseB pool before releasing the PEs
      // (single-threaded; pe_subtrees frozen since registration;
      // visibility: built before the trigger messages are sent).
      // Geometry-gated build (design/phase1-scaling.md): a pair enters
      // the pool only if it can interact — the walk's own mindist test,
      // run once here — and every surviving pair is split one level
      // (8x8 child cross product, itself mindist-filtered) so no single
      // claimable unit can hide a dense-boundary giant (the Anvil
      // 4-node residue: one indivisible ~0.148 s pair). No tunable
      // threshold: geometry decides. b2_/period_ were set by this PE's
      // phaseABody.
      nb->phaseb_pool.clear();
      nb->phaseb_next = 0;
      // Two split levels: the Anvil trace showed a 62 ms unit surviving
      // ONE level (design/status-poolab-2026-07-27.md) — larger than a
      // perfectly-leveled process share, so grandchild granularity is
      // warranted. Each level is mindist-filtered, so only interacting
      // pairs multiply.
      std::function<void(Node<Data>*, Node<Data>*, int)> poolPush =
          [&](Node<Data>* a, Node<Data>* b, int depth) {
        double d2 = paratreet::mindist2(a->data.box, b->data.box, period_);
        if (d2 > b2_) return;
        // Depth 2 is reserved for OVERLAPPING pairs (gap 0): only dense
        // shared boundaries can hide a giant unit, and unconditional
        // grandchild granularity measurably inflated the per-unit fixed
        // costs (phaseB avg 0.014 -> 0.020 on the laptop). Separated
        // pairs stay at depth-1 granularity. Still pure geometry.
        if (depth >= 2 || (depth == 1 && d2 > 0) || a->isLeaf() ||
            b->isLeaf()) {
          // LPT key, pure geometry (no thresholds): overlapping pairs
          // (gap 0) are the expensive ones, ordered by DESCENDING box
          // overlap volume; separated pairs follow by ascending gap.
          // Ascending sort then claims costliest-first, so the pool's
          // tail is cheap units and the last claim cannot be a giant.
          double key;
          if (d2 > 0) {
            key = d2;
          } else {
            const auto& ba = a->data.box;
            const auto& bb = b->data.box;
            double ov = 1;
            ov *= std::max(0.0,
                (double)std::min(ba.greater_corner.x, bb.greater_corner.x) -
                (double)std::max(ba.lesser_corner.x, bb.lesser_corner.x));
            ov *= std::max(0.0,
                (double)std::min(ba.greater_corner.y, bb.greater_corner.y) -
                (double)std::max(ba.lesser_corner.y, bb.lesser_corner.y));
            ov *= std::max(0.0,
                (double)std::min(ba.greater_corner.z, bb.greater_corner.z) -
                (double)std::max(ba.lesser_corner.z, bb.lesser_corner.z));
            key = -ov;
          }
          nb->phaseb_pool.push_back({key, a, b});
          return;
        }
        for (int ci = 0; ci < a->n_children; ci++) {
          Node<Data>* ca = a->getChild(ci);
          if (ca == nullptr || ca->n_particles == 0) continue;
          for (int cj = 0; cj < b->n_children; cj++) {
            Node<Data>* cb = b->getChild(cj);
            if (cb == nullptr || cb->n_particles == 0) continue;
            poolPush(ca, cb, depth + 1);
          }
        }
      };
      for (auto ita = nb->pe_subtrees.begin(); ita != nb->pe_subtrees.end();
           ++ita) {
        auto itb = ita;
        for (++itb; itb != nb->pe_subtrees.end(); ++itb)
          for (auto& sa : ita->second)
            for (auto& sb : itb->second)
              poolPush(sa.root, sb.root, 0);
      }
      std::stable_sort(nb->phaseb_pool.begin(), nb->phaseb_pool.end(),
                       [](const typename FoFPhase1Node<Data>::PoolUnit& x,
                          const typename FoFPhase1Node<Data>::PoolUnit& y) {
                         return x.key < y.key;
                       });
      nb->phaseb_pool_ready.store(true, std::memory_order_release);
      int first = CkNodeFirst(CkMyNode());
      for (int pe = first; pe < first + CkNodeSize(CkMyNode()); pe++)
        this->thisProxy[pe].phaseBChained();
    }
  }

  void phaseBChained() {
    auto* nb = node_proxy.ckLocalBranch();
    phaseBBody(b2_); // b2_ set by phaseABody on this PE
    if (nb->b_done.fetch_add(1) + 1 == CkNodeSize(CkMyNode())) {
      nb->stage_tB = CkWallTimer() - nb->chain_t0 - nb->stage_tA;
      nb->pool_deposits_done.store(true, std::memory_order_release);
      nb->tryTriggerMerge(); // fires now unless stolen batches are out
    }
    // This PE's own pool work is done and deposited; offer to help other
    // processes in the steal group (same physical machine by the
    // FOF_STEAL_GROUP convention). Self-send so this entry returns first.
    if (stealEnabled() && CkNumNodes() > 1)
      this->thisProxy[CkMyPe()].startHelping();
  }

  // --- phaseB stealing (design/phaseb-offload.md stages 1-2). Statics
  // configured by environment: FOF_STEAL=0 disables; FOF_STEAL_GROUP is
  // the number of consecutive processes per physical machine (SLURM block
  // placement; 8 on Anvil wholenode); FOF_STEAL_K is units per request.
  static bool stealEnabled() {
    // OPT-IN until the 80M-scale discrepancy is diagnosed (2026-08-06: a
    // traced 480-PE run with steals active produced two spurious merges
    // of large components — 23,707,195 instead of 23,707,197 — while
    // every laptop-scale forced-steal validation passes; the defect is
    // probabilistic in which units are stolen). FOF_STEAL=1 enables.
    static const bool on = [] {
      const char* e = std::getenv("FOF_STEAL");
      return e && std::atoi(e) == 1;
    }();
    return on;
  }
  static int stealGroup() {
    static const int g = [] {
      const char* e = std::getenv("FOF_STEAL_GROUP");
      int v = e ? std::atoi(e) : 8;
      return v > 0 ? v : 8;
    }();
    return g;
  }
  static int stealK() {
    static const int k = [] {
      const char* e = std::getenv("FOF_STEAL_K");
      int v = e ? std::atoi(e) : 4;
      return v > 0 ? v : 4;
    }();
    return k;
  }

  // Helper-side state: victims left to try in my steal group.
  std::vector<int> steal_victims;
  size_t steal_victim_i = 0;

  void startHelping() {
    steal_victims.clear();
    steal_victim_i = 0;
    int mine = CkMyNode();
    int g = stealGroup();
    int lo = (mine / g) * g;
    int hi = std::min(lo + g, CkNumNodes());
    for (int j = mine + 1; j < hi; j++) steal_victims.push_back(j);
    for (int j = lo; j < mine; j++) steal_victims.push_back(j);
    askNextVictim();
  }

  void askNextVictim() {
    if (steal_victim_i >= steal_victims.size()) return; // nothing left; idle
    int victim = steal_victims[steal_victim_i];
    this->thisProxy[CkNodeFirst(victim)].requestSteal(CkMyPe());
  }

  // Victim side: claim up to K units for the helper. outstanding++ comes
  // BEFORE the cursor claim (see the node-branch comment); the deny path
  // undoes it and re-checks the merge gate.
  void requestSteal(int helper_pe) {
    auto* nb = node_proxy.ckLocalBranch();
    if (!nb->phaseb_pool_ready.load(std::memory_order_acquire)) {
      // Not "drained" — phaseA has not finished here, so the pool does
      // not exist yet. The heavy process is typically also the slow one
      // through phaseA, so helpers must retry rather than give up (a
      // fast process can finish its whole phaseB before a slow one
      // builds its pool).
      this->thisProxy[helper_pe].stealDenied(-(CkMyNode() + 1)); // retry code
      return;
    }
    int k = stealK();
    nb->steal_outstanding.fetch_add(1, std::memory_order_acq_rel);
    size_t start = nb->phaseb_next.fetch_add((size_t)k);
    size_t pool = nb->phaseb_pool.size();
    if (start >= pool || nb->merge_fired.load()) {
      nb->steal_denials.fetch_add(1);
      nb->steal_outstanding.fetch_sub(1, std::memory_order_acq_rel);
      nb->tryTriggerMerge();
      this->thisProxy[helper_pe].stealDenied(CkMyNode());
      return;
    }
    size_t end = std::min(start + (size_t)k, pool);
    paratreet::StealShipment<Data> ship;
    ship.victim_node = CkMyNode();
    for (size_t u = start; u < end; u++) {
      ship.trees.emplace_back();
      flattenStealTree(nb->phaseb_pool[u].a, ship.trees.back());
      ship.trees.emplace_back();
      flattenStealTree(nb->phaseb_pool[u].b, ship.trees.back());
    }
    nb->stolen_out_units.fetch_add((long)(end - start));
    this->thisProxy[helper_pe].receiveSteal(ship);
  }

  void stealDenied(int victim_node) {
    if (victim_node < 0) {
      // Victim's pool is not built yet; retry the same victim after a
      // short delay (timer-paced so the retries do not spin).
      CcdCallFnAfter([](void* arg, double) {
        auto* self = (FoFPhase1<Data>*)arg;
        self->askNextVictim();
      }, this, 1.0 /* ms */);
      return;
    }
    steal_victim_i++;
    askNextVictim();
  }

  // Helper side: rebuild the shipped trees, run each unit with the same
  // walk and certificate semantics as phaseBBody but into LOCAL buffers
  // (this PE's own phaseB members are long done; locals keep it
  // re-entrant), return the edges, then ask the same victim again.
  // Execute one unit over any node pair (live or rebuilt) into a local,
  // per-call edge set — the identical walk, certificate, and dedup
  // semantics as phaseBBody, with no shared state. Used by the helper
  // executor and by the self-test.
  void walkUnitEdges(Node<Data>* a, Node<Data>* b,
                     std::vector<std::pair<long, long>>& out) {
    std::unordered_set<paratreet::TipPairKey, paratreet::TipPairKeyHash> lseen;
    std::unordered_map<Node<Data>*, long> lmemo;
    auto emitLocal = [&](long ti, long tj) {
      if (ti == tj) return;
      long lo = std::min(ti, tj), hi = std::max(ti, tj);
      if (lseen.insert(paratreet::packTipPair(lo, hi)).second)
        out.emplace_back(lo, hi);
    };
    std::function<void(Node<Data>*, long)> starLocal =
        [&](Node<Data>* n, long rep) {
      if (n == nullptr || n->n_particles == 0) return;
      if (n->isLeaf()) {
        const Particle* p = n->particles();
        for (int i = 0; i < n->n_particles; i++) emitLocal(rep, p[i].group_number);
        return;
      }
      for (int i = 0; i < n->n_children; i++) starLocal(n->getChild(i), rep);
    };
    auto certLocal = [&](Node<Data>* n) -> long {
      auto it = lmemo.find(n);
      if (it != lmemo.end()) return it->second;
      long rep = firstTip(n);
      starLocal(n, rep);
      lmemo.emplace(n, rep);
      return rep;
    };
    walk(a, b,
         [&](Node<Data>* x, Node<Data>* y) {
           const Particle* px = x->particles();
           const Particle* py = y->particles();
           for (int i = 0; i < x->n_particles; i++)
             for (int j = 0; j < y->n_particles; j++) {
               if (paratreet::periodicDistSq(px[i].position, py[j].position,
                                             period_) > b2_) continue;
               emitLocal(px[i].group_number, py[j].group_number);
             }
         },
         [&](Node<Data>* x, Node<Data>* y) {
           long tx = certLocal(x);
           long ty = certLocal(y);
           emitLocal(tx, ty);
         },
         [](Node<Data>*, Node<Data>*) { return false; });
  }

  void receiveSteal(const paratreet::StealShipment<Data>& ship) {
    std::vector<long> edges_flat;
    long units = 0;
    for (size_t t = 0; t + 1 < ship.trees.size(); t += 2) {
      std::vector<Particle> pa = ship.trees[t].particles;
      std::vector<Particle> pb = ship.trees[t + 1].particles;
      Node<Data>* a = buildStealTree(ship.trees[t], pa);
      Node<Data>* b = buildStealTree(ship.trees[t + 1], pb);
      std::vector<std::pair<long, long>> es;
      walkUnitEdges(a, b, es);
      for (auto& e : es) {
        edges_flat.push_back(e.first);
        edges_flat.push_back(e.second);
      }
      deleteStealTree(a);
      deleteStealTree(b);
      units++;
    }
    node_proxy.ckLocalBranch()->stolen_in_units.fetch_add(units);
    this->thisProxy[CkNodeFirst(ship.victim_node)].returnStolenEdges(edges_flat);
    askNextVictim(); // same victim again (steal_victim_i unchanged)
  }

  // Self-test (FOF_STEAL_SELFTEST=1, debug): for every locally claimed
  // pool unit, run the direct walk AND the full ship-path replica —
  // flatten, serialize and deserialize through memory (the same pup code
  // marshalled messages use), rebuild, walk — and abort on the first
  // difference between the two edge sets. Covers every unit of every
  // process, orders of magnitude more than real steals exercise.
  void selftestStolenUnit(Node<Data>* a, Node<Data>* b) {
    std::vector<std::pair<long, long>> direct;
    walkUnitEdges(a, b, direct);
    paratreet::StealShipment<Data> ship;
    ship.trees.emplace_back();
    flattenStealTree(a, ship.trees.back());
    ship.trees.emplace_back();
    flattenStealTree(b, ship.trees.back());
    PUP::sizer sz;
    ship.pup(sz);
    std::vector<char> buf(sz.size());
    PUP::toMem tm(buf.data());
    ship.pup(tm);
    paratreet::StealShipment<Data> ship2;
    PUP::fromMem fm(buf.data());
    ship2.pup(fm);
    std::vector<Particle> pa = ship2.trees[0].particles;
    std::vector<Particle> pb = ship2.trees[1].particles;
    Node<Data>* ra = buildStealTree(ship2.trees[0], pa);
    Node<Data>* rb = buildStealTree(ship2.trees[1], pb);
    std::vector<std::pair<long, long>> rebuilt;
    walkUnitEdges(ra, rb, rebuilt);
    deleteStealTree(ra);
    deleteStealTree(rb);
    std::sort(direct.begin(), direct.end());
    std::sort(rebuilt.begin(), rebuilt.end());
    if (direct != rebuilt) {
      CkPrintf("STEAL SELFTEST MISMATCH pe %d: direct %zu edges, rebuilt "
               "%zu edges (unit roots %llx %llx)\n",
               CkMyPe(), direct.size(), rebuilt.size(),
               (unsigned long long)a->key, (unsigned long long)b->key);
      for (size_t i = 0; i < std::max(direct.size(), rebuilt.size()); i++) {
        if (i >= direct.size() || i >= rebuilt.size() ||
            direct[i] != rebuilt[i]) {
          CkPrintf("  first difference at %zu: direct (%ld,%ld) rebuilt "
                   "(%ld,%ld)\n", i,
                   i < direct.size() ? direct[i].first : -1,
                   i < direct.size() ? direct[i].second : -1,
                   i < rebuilt.size() ? rebuilt[i].first : -1,
                   i < rebuilt.size() ? rebuilt[i].second : -1);
          break;
        }
      }
      CkAbort("steal self-test mismatch");
    }
  }

  // Victim side: merge a helper's edges and settle the batch.
  void returnStolenEdges(const std::vector<long>& edges_flat) {
    auto* nb = node_proxy.ckLocalBranch();
    std::vector<std::pair<long, long>> es;
    es.reserve(edges_flat.size() / 2);
    for (size_t i = 0; i + 1 < edges_flat.size(); i += 2)
      es.emplace_back(edges_flat[i], edges_flat[i + 1]);
    nb->submitEdges(std::move(es));
    nb->steal_outstanding.fetch_sub(1, std::memory_order_acq_rel);
    nb->tryTriggerMerge();
  }

  // Preorder flatten of a pool-unit node's subtree: (key, spatial node)
  // per node, leaf particles appended in the same preorder.
  void flattenStealTree(Node<Data>* n, paratreet::StealTree<Data>& out) {
    if (n == nullptr) {
      // Absent child slot: key 0 marks it (real keys start at 1), so the
      // preorder rebuild stays aligned without any key arithmetic.
      out.nodes.emplace_back(Key(0), SpatialNode<Data>());
      return;
    }
    out.nodes.emplace_back(n->key, SpatialNode<Data>(*n));
    if (n->isLeaf()) {
      const Particle* p = n->particles();
      for (int i = 0; i < n->n_particles; i++) out.particles.push_back(p[i]);
      return;
    }
    for (int i = 0; i < n->n_children; i++) flattenStealTree(n->getChild(i), out);
  }

  // Rebuild a shipped tree as private FullNodes over the (caller-owned)
  // particle vector. FoF is oct-only (enforced at startup), so the branch
  // factor is 8. Children are reconstructed by preorder position.
  Node<Data>* buildStealTree(const paratreet::StealTree<Data>& t,
                             std::vector<Particle>& parts) {
    size_t ni = 0, pi = 0;
    return buildStealNode(t, parts, ni, pi, nullptr);
  }
  Node<Data>* buildStealNode(const paratreet::StealTree<Data>& t,
                             std::vector<Particle>& parts, size_t& ni,
                             size_t& pi, Node<Data>* parent) {
    if (ni >= t.nodes.size()) return nullptr;
    const auto& kv = t.nodes[ni++];
    if (kv.first == Key(0)) return nullptr; // absent child slot marker
    const SpatialNode<Data>& sn = kv.second;
    bool leaf = sn.n_particles >= 0;
    auto type = leaf ? (sn.n_particles == 0 ? Node<Data>::Type::EmptyLeaf
                                            : Node<Data>::Type::Leaf)
                     : Node<Data>::Type::Internal;
    Particle* pp = nullptr;
    if (leaf && sn.n_particles > 0) {
      pp = parts.data() + pi;
      pi += sn.n_particles;
    }
    auto* node = new FullNode<Data, 8>(kv.first, type, sn, pp, parent, -1, -1);
    if (!leaf) {
      for (int i = 0; i < node->n_children; i++) {
        Node<Data>* c = buildStealNode(t, parts, ni, pi, node);
        node->exchangeChild(i, c);
      }
    }
    return node;
  }
  void deleteStealTree(Node<Data>* n) {
    if (n == nullptr) return;
    for (int i = 0; i < n->n_children; i++) deleteStealTree(n->getChild(i));
    delete n;
  }

  void relabelChained(double tA, double tB, double tM) {
    double t0 = CkWallTimer();
    relabelBody();
    // Stage walls to the driver: process-local values, max-reduced over
    // all PEs (every PE of a process contributes its process's identical
    // tA/tB/tM plus its own relabel time).
    double vals[4] = {tA, tB, tM, CkWallTimer() - t0};
    this->contribute(4 * sizeof(double), vals, CkReduction::max_double,
                     done_cb_);
  }

  // Fragment-size histogram, step 1 of 2 (run after relabel): count this
  // PE's registered particles per process-level tip and hand the counts to the
  // process-wide FoFPhase1Node (synchronous local-branch call, like
  // submitEdges, so the counts are complete when the barrier fires).
  void countFragments(const CkCallback& cb) {
    std::unordered_map<long, long> counts;
    for (auto& s : subtrees)
      for (int i = 0; i < s.n; i++) counts[s.parts[i].group_number]++;
    node_proxy.ckLocalBranch()->submitFragCounts(counts);
    this->contribute(cb);
  }

  // --- Step 4 (distributed UF_2, -u dist; see design/step4.md and
  // design/sparse-uf2-encoding.md). Sequence, driven by the app +
  // paratreet::runFoFPhase3Dist: applyTipEncoding (below; must run and
  // complete, as a barrier, BEFORE upwardPass/loadCache/the phase-3 walk,
  // so every particle copy the walk reads already carries the encoded tip;
  // see paratreet::uf2EncodeTip's comment) -> [walk emits encoded-tip edges
  // into edge_buf3 exactly as in v1/3a, no visitor changes needed] ->
  // initUF2 -> fireUF2Edges -> CkWaitQD -> UnionFindLib::find_components ->
  // collectUF2Labels -> applyUF2Labels.

  // Enumeration-free (sparse-uf2, 2026-07-25): a tip is already globally
  // unique (the min-order particle's global order) and every particle of a
  // fragment lives in this process, so the owner-decodable id is a pure
  // per-particle rewrite — no counting, no per-process enumeration, no
  // encode map. countFragments/computeTipEncoding are no longer on this
  // path (the fragments histogram, their only surviving consumer, is
  // optional reporting: fof3 -g).
  void applyTipEncoding(const CkCallback& cb) {
    tips_encoded_ = true; // arms the debug same-process-edge tripwire
    int my_node = CkMyNode();
    for (auto& s : subtrees) {
      for (int i = 0; i < s.n; i++) {
        long tip = s.parts[i].group_number;
        CkEnforce(tip >= 0 && (uint64_t)tip <= paratreet::kUF2IdxMask);
        s.parts[i].group_number = (long)paratreet::uf2EncodeTip(my_node, tip);
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
      UnionFindLib* lib = uf_proxy[CkMyNode()].ckLocal();
      CkEnforce(lib != nullptr); // must be true on the element's home PE
      lib->registerGetLocationFromID(&paratreet::uf2LocationFromID);
      // Lazy mode: no vertex array. The library creates a vertex on first
      // touch, reconstructing its full id with the registered inverse.
      lib->registerMakeVertexID(&paratreet::uf2MakeVertexID);
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

  // Enable mid-walk streaming of phase-3 edge batches into UF_2
  // (design/walk-uf2-overlap.md step 1): once armed, addPhase3Edge submits
  // edge_buf3 to the lib whenever it reaches `batch` edges. seen3 persists
  // across batches, so per-PE dedup is exactly as in the buffered path.
  // Plain-sends only (the dist driver forces batch=0 under AGGREGATION):
  // streamed cascades must stay visible to the walk's QD.
  void enableUF2Streaming(CProxy_UnionFindLib uf_proxy, long batch,
                          const CkCallback& cb) {
    uf2_stream_proxy = uf_proxy;
    uf2_stream_batch = batch;
    this->contribute(cb);
  }

  // CkEnforce the owner-locality invariant the whole scheme depends on:
  // every registered particle's (encoded) tip must decode to THIS process
  // (node bits == CkMyNode()) with a dense index within this process's own
  // UF_2 vertex array. A violation here means applyTipEncoding ran against
  // stale/foreign state (ordering bug), not a UF_2 library bug.
  void verifyEncodedTips(const CkCallback& cb) {
    int my_node = CkMyNode();
    for (auto& s : subtrees) {
      for (int i = 0; i < s.n; i++) {
        long enc = s.parts[i].group_number;
        CkEnforce(enc >= 0); // sign bit clear (43/20 split)
        CkEnforce(int(uint64_t(enc) >> paratreet::kUF2IdxBits) == my_node);
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
  // Step 1 of the lazy-mode label readback: the element's home PE copies
  // the touched-vertex labels (localId -> componentNumber) out of the
  // library's hash storage into the node branch, where every PE of the
  // process can read them after the barrier.
  void collectUF2Labels(CProxy_UnionFindLib uf_proxy, const CkCallback& cb) {
    if (CkMyPe() == CkNodeFirst(CkMyNode())) {
      auto* nb = node_proxy.ckLocalBranch();
      UnionFindLib* lib = uf_proxy[CkMyNode()].ckLocal();
      CkEnforce(lib != nullptr);
      nb->uf2_labels.clear();
      lib->collectComponentLabels(nb->uf2_labels);
    }
    this->contribute(cb);
  }

  // Step 2: owner-writes rewrite. A tip PRESENT in the touched-label map
  // becomes -(componentNumber + 2): negative, so the final label namespace
  // is disjoint from untouched fragments' (non-negative) encoded tips, and
  // distinct from the -1 sentinel. A tip ABSENT from the map was never
  // referenced by any merge edge — the fragment is its own component and
  // keeps its encoded tip as its (globally unique) label. This is the
  // identity-if-absent convention relabel/applyGlobalMap already use.
  // Labels are arbitrary per-run values either way; the fof3 harness
  // canonicalizes by min order per label group (design/step4.md).
  void applyUF2Labels(const CkCallback& cb) {
    auto* nb = node_proxy.ckLocalBranch();
    auto& labels = nb->uf2_labels;
    for (auto& s : subtrees) {
      for (int i = 0; i < s.n; i++) {
        uint64_t local_id =
            (uint64_t)s.parts[i].group_number & paratreet::kUF2IdxMask;
        auto it = labels.find(local_id);
        if (it != labels.end()) {
          CkEnforce(it->second != -1);
          s.parts[i].group_number = -(it->second + 2);
        } // else: untouched fragment keeps its encoded tip
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
#if DEBUG
    // Instrumented-build tripwire (Kale, 2026-08-04): a same-process edge
    // here means phase 1 under-merged (its completeness invariant says
    // different tips within b are on different processes) OR a cached
    // copy carried stale tips. UF_2 would absorb such an edge silently
    // and CORRECTLY (union_request's local_union fast path), masking the
    // upstream bug — so debug builds abort loudly instead. Armed only
    // once tips are owner-encoded (-u dist); serial-mode raw tips carry
    // no owner bits to compare.
    if (tips_encoded_ &&
        (uint64_t(ti) >> paratreet::kUF2IdxBits) ==
        (uint64_t(tj) >> paratreet::kUF2IdxBits)) {
      CkAbort("FoF phase-3: same-process edge emitted (tips %ld, %ld on "
              "process %ld) — phase 1 incomplete or stale cached tips",
              ti, tj, (long)(uint64_t(ti) >> paratreet::kUF2IdxBits));
    }
#endif
    phase3_emitted++;
    long lo = std::min(ti, tj), hi = std::max(ti, tj);
    if (seen3.insert(paratreet::packTipPair(lo, hi)).second)
      edge_buf3.emplace_back(lo, hi);
    if ((long)edge_buf3.size() > p3_peak_edge_buf)
      p3_peak_edge_buf = (long)edge_buf3.size();
    if (uf2_stream_batch > 0 && (long)edge_buf3.size() >= uf2_stream_batch)
      flushUF2Batch();
  }

  // Submit the current edge buffer to this process's UnionFindLib element
  // (the library routes each edge to its owner). Non-entry; runs on the
  // walking PE. The buffer clears but seen3 does NOT: dedup spans batches.
  void flushUF2Batch() {
    std::vector<UFEdge> edges;
    edges.reserve(edge_buf3.size());
    for (auto& e : edge_buf3)
      edges.push_back(UFEdge{(uint64_t)e.first, (uint64_t)e.second});
    uf2_stream_proxy[CkMyNode()].union_requests(edges);
    p3_edges_streamed += (long)edge_buf3.size();
    edge_buf3.clear();
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
                                  // (with streaming on, <= the batch size)
  // Mid-walk UF_2 streaming state (enableUF2Streaming / flushUF2Batch).
  CProxy_UnionFindLib uf2_stream_proxy;
  long uf2_stream_batch = 0;      // 0 = off (classic post-walk injection)
  long p3_edges_streamed = 0;     // edges already submitted mid-walk

  // Per-PE wall time of the phaseA/phaseB entry bodies (load-imbalance
  // signals; set in the entries above, reset by reset(), reduced min/avg/max
  // over PEs by phase3Stats). NOT cleared by resetPhase3: phase 1 runs
  // before the phase-3 reset each iteration.
  double t_phaseA = 0.0;
  double t_phaseB = 0.0;
  double t_phaseB_maxpair = 0.0; // longest single pool-unit walk this PE ran
  long t_phaseB_units = 0;       // pool units this PE claimed (pool balance)
  // Predicted phaseA pair work from geometry alone: sum over this PE's
  // subtrees of n^2/V (pair count within a chare ~ n * local density).
  // Correlated against t_phaseA across PEs by the stats reduction — the
  // density-drives-phase1-work quantifier (Kale, 2026-07-29).
  double density_x = 0.0;

  void resetPhase3(const CkCallback& cb) {
    tips_encoded_ = false;
    edge_buf3.clear();
    seen3.clear();
    phase3_emitted = 0;
    uf2_stream_batch = 0;
    p3_edges_streamed = 0;
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
    // Gather-integrity diagnostic (FOF_EDGE_CHECK=1): per-contribution
    // checksums to compare against what the root receives.
    if (std::getenv("FOF_EDGE_CHECK")) {
      long n = (long)edge_buf3.size(), slo = 0, shi = 0, zeros = 0;
      for (auto& e : edge_buf3) {
        slo += e.first; shi += e.second;
        if (e.first == 0 && e.second == 0) zeros++;
      }
      CkPrintf("edge-check contrib: pe %d n %ld sum_lo %ld sum_hi %ld zeros %ld\n",
               CkMyPe(), n, slo, shi, zeros);
    }
    this->contribute(edge_buf3.size() * sizeof(std::pair<long, long>),
                     edge_buf3.data(), CkReduction::concat, cb);
  }

  // Edge + 3a-counter statistics, a tuple reduction:
  //   element 0 (sum over PEs), long[9]:
  //     [0] edges emitted (SEEN wins reaching addPhase3Edge)
  //     [1] edges sent to the gather (after per-PE dedup)
  //     [2] negative prunes  [3] positive-certificate prunes
  //     [4] suppression prunes  [5] same-fragment prunes
  //     [6] leaf visits  [7] redundant (both-uniform) descents
  //     [8] phaseB pool units claimed (total = process pool sizes summed)
  //   element 1 (max over PEs), long: peak edge-buffer size.
  // Load-imbalance extension (min/avg/max over PEs; avg = sum/CkNumPes at
  // the consumer):
  //   element 2 (min over PEs), long[4]: [0] leaf visits [1] edges emitted
  //     [2] per-process redundant total [3] phaseB pool units claimed
  //   element 3 (max over PEs), long[4]: same layout
  //   element 4 (sum over PEs), double[4]: [0] phaseA s [1] phaseB s
  //     [2] phaseB maxpair s [3] density-work proxy X (sum n^2/V)
  //   element 5 (min over PEs), double[4]: same layout
  //   element 6 (max over PEs), double[4]: same layout
  //   element 7 (sum over PEs), double[3]: X*t_phaseA, X^2, t_phaseA^2 —
  //     with element 4's X and phaseA sums these give the Pearson r of
  //     predicted density work vs measured phaseA time across PEs.
  void phase3Stats(const CkCallback& cb) {
    // "edges sent" counts streamed batches plus what remains buffered (the
    // two are disjoint: flushUF2Batch clears the buffer as it submits).
    long sums[9] = {phase3_emitted,
                    p3_edges_streamed + (long)edge_buf3.size(),
                    p3_negative_prunes, p3_positive_prunes,
                    p3_suppression_prunes, p3_same_frag_prunes,
                    p3_leaf_visits, p3_redundant_descents, t_phaseB_units};
    long peak = p3_peak_edge_buf;
    // per_pe[0,1] are PER-PE (leaf visits, edges emitted); per_pe[2] is the
    // PER-PROCESS redundant-descent total (design/step3.md §6d) -- every PE of
    // a process reads the same deposited value, so min/max over PEs == min/max
    // over processes (the SMP trick memoryStats relies on). Requires
    // depositNodeRedundant to have run post-walk; avg-over-processes is
    // p3_redundant_descents-sum / CkNumNodes() at the consumer.
    long node_redundant = node_proxy.ckLocalBranch()->p3_node_redundant;
    long per_pe[4] = {p3_leaf_visits, phase3_emitted, node_redundant,
                      t_phaseB_units};
    double times[4] = {t_phaseA, t_phaseB, t_phaseB_maxpair, density_x};
    double corr[3] = {density_x * t_phaseA, density_x * density_x,
                      t_phaseA * t_phaseA};
    CkReduction::tupleElement tupleRedn[] = {
      CkReduction::tupleElement(sizeof(sums), sums, CkReduction::sum_long),
      CkReduction::tupleElement(sizeof(long), &peak, CkReduction::max_long),
      CkReduction::tupleElement(sizeof(per_pe), per_pe, CkReduction::min_long),
      CkReduction::tupleElement(sizeof(per_pe), per_pe, CkReduction::max_long),
      CkReduction::tupleElement(sizeof(times), times, CkReduction::sum_double),
      CkReduction::tupleElement(sizeof(times), times, CkReduction::min_double),
      CkReduction::tupleElement(sizeof(times), times, CkReduction::max_double),
      CkReduction::tupleElement(sizeof(corr), corr, CkReduction::sum_double)
    };
    CkReductionMsg* msg = CkReductionMsg::buildFromTuple(tupleRedn, 8);
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
  // Resident-set size of this process, from the OS accounting: works on
  // runtimes where CmiMemoryUsage returns 0 (reconverse does not wrap the
  // allocator). One pseudo-file read per call — negligible.
  static long processRSSBytes() {
    long rss = 0;
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS)
      rss = (long)info.resident_size;
#else
    FILE* f = fopen("/proc/self/statm", "r");
    if (f) {
      long tot = 0, res = 0;
      if (fscanf(f, "%ld %ld", &tot, &res) == 2)
        rss = res * sysconf(_SC_PAGESIZE);
      fclose(f);
    }
#endif
    if (rss <= 0) rss = (long)CmiMemoryUsage(); // fallback
    return rss;
  }

  void memoryStats(const CkCallback& cb) {
    long mem = processRSSBytes();
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
  //
  // Distributed component histogram (Kale's scheme, 2026-07-25; replaces
  // the 240-way concat gather whose spanning-tree copying was ~400 MB x
  // tree depth of runtime overhead at 80M — the black blob in the
  // Projections trace). Key facts it exploits: after applyUF2Labels the
  // label SIGN classifies every component — POSITIVE labels are untouched
  // single-fragment components, entirely process-local (fragments are
  // process-level), so their exact sizes are computable inside the
  // process; NEGATIVE labels mark components reached by cross-process
  // merge edges — few (7,029 at 80M P=8) — and only those need global
  // per-label summing. Three stages, driven by runFoFComponentHistogram:
  //  1. depositLabelCounts: per-PE label counts, bucketed by label hash
  //     into the node branch's per-shard buffers (intra-process
  //     reduce-scatter — NOT a locked whole-map merge, which would be the
  //     countFragments wedge reborn).
  //  2. histogramShard: the PE owning shard r merges bucket r; positive
  //     labels are binned locally and tuple-SUM-reduced (a fixed 64-bin
  //     vector fits the standard reduction mold); negative totals are
  //     kept aside per PE.
  //  3. collectTouchedCounts: tiny concat of the per-process negative
  //     (label, total) pairs to PE 0, which sums across processes and
  //     folds them into the histogram.

  static uint64_t labelShardMix(long label) {
    uint64_t h = (uint64_t)label;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
  }

  void depositLabelCounts(const CkCallback& cb) {
    int n_shards = CkNodeSize(CkMyNode());
    auto* nb = node_proxy.ckLocalBranch();
    nb->ensureLabelShards(n_shards);
    std::unordered_map<long, long> counts;
    for (auto& s : subtrees)
      for (int i = 0; i < s.n; i++) counts[s.parts[i].group_number]++;
    std::vector<std::vector<std::pair<long, long>>> buckets(n_shards);
    for (auto& kv : counts)
      buckets[labelShardMix(kv.first) % n_shards].emplace_back(kv.first,
                                                               kv.second);
    for (int r = 0; r < n_shards; r++) {
      if (buckets[r].empty()) continue;
      auto& shard = *nb->label_shards[r];
      std::lock_guard<std::mutex> g(shard.m);
      shard.pairs.insert(shard.pairs.end(), buckets[r].begin(),
                         buckets[r].end());
    }
    this->contribute(cb);
  }

  void histogramShard(int min_component_size, const CkCallback& cb) {
    int my_shard = CkMyRank();
    auto* nb = node_proxy.ckLocalBranch();
    touched_totals_.clear();
    // 2 x (64 bins + count + max): totals then survivors.
    long bins[64] = {0}, sbins[64] = {0};
    long n = 0, maxs = 0, sn = 0, smaxs = 0;
    if (my_shard < (int)nb->label_shards.size()) {
      auto& shard = *nb->label_shards[my_shard];
      std::unordered_map<long, long> totals;
      totals.reserve(shard.pairs.size());
      for (auto& p : shard.pairs) totals[p.first] += p.second;
      shard.pairs.clear();
      shard.pairs.shrink_to_fit();
      for (auto& kv : totals) {
        if (kv.first < 0) { // touched: global summing needed (stage 3)
          touched_totals_.emplace_back(kv.first, kv.second);
          continue;
        }
        long size = kv.second; // untouched fragment: complete local total
        int bin = 0;
        while (bin < 63 && (1L << (bin + 1)) <= size) bin++;
        bins[bin]++; n++;
        if (size > maxs) maxs = size;
        if (size >= (long)min_component_size) {
          sbins[bin]++; sn++;
          if (size > smaxs) smaxs = size;
        }
      }
    }
    CkReduction::tupleElement tuple[] = {
      CkReduction::tupleElement(sizeof(bins), bins, CkReduction::sum_long),
      CkReduction::tupleElement(sizeof(long), &n, CkReduction::sum_long),
      CkReduction::tupleElement(sizeof(long), &maxs, CkReduction::max_long),
      CkReduction::tupleElement(sizeof(sbins), sbins, CkReduction::sum_long),
      CkReduction::tupleElement(sizeof(long), &sn, CkReduction::sum_long),
      CkReduction::tupleElement(sizeof(long), &smaxs, CkReduction::max_long)
    };
    CkReductionMsg* msg = CkReductionMsg::buildFromTuple(tuple, 6);
    msg->setCallback(cb);
    this->contribute(msg);
  }

  void collectTouchedCounts(const CkCallback& cb) {
    this->contribute(touched_totals_.size() * sizeof(std::pair<long, long>),
                     touched_totals_.data(), CkReduction::concat, cb);
    touched_totals_.clear();
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

  // Residual stencil for the b/sqrt(6) grid: forward-half offsets whose
  // cells can hold a pair within b. Minimum gap between cells offset by d
  // is (|d|-1)+ cells per axis, so the reachability condition is
  // sum(((|d|-1)+)^2) <= 6 (equality included: gap exactly b). Face
  // offsets (single +1) are in the list too but recognized by the caller
  // for the test-free union.
  static const std::vector<std::array<int, 3>>& gridOffsets() {
    static const std::vector<std::array<int, 3>> offs = [] {
      std::vector<std::array<int, 3>> v;
      for (int dz = -3; dz <= 3; dz++)
        for (int dy = -3; dy <= 3; dy++)
          for (int dx = -3; dx <= 3; dx++) {
            if (dz < 0 || (dz == 0 && dy < 0) ||
                (dz == 0 && dy == 0 && dx <= 0))
              continue; // forward half-space: each unordered pair once
            auto g = [](int d) { int a = std::abs(d) - 1; return a > 0 ? a : 0; };
            if (g(dx) * g(dx) + g(dy) * g(dy) + g(dz) * g(dz) <= 6)
              v.push_back({dx, dy, dz});
          }
      return v;
    }();
    return offs;
  }

  // Grid solve of one dense chare's self pair (Kale's cell idea,
  // 2026-07-25; design/phase1-scaling.md). Cell side c = b/sqrt(6) gives
  // two test-free guarantees: any same-cell pair is within b (diagonal
  // c*sqrt(3) = b/sqrt(2)) and any pair in FACE-adjacent cells is within
  // b (max separation c*sqrt(6) = b). So one pass unions each cell into a
  // clique through its first-seen representative, a neighbor pass unions
  // face-adjacent occupied cells rep-to-rep, and distance tests survive
  // only across the residual stencil between cells not already in the
  // same component (first witness merges, same as leafLeafUnion).
  // Euclidean bounds only shrink under minimum-image PBC, so the free
  // unions stay valid with a period; residual tests use periodicDistSq.
  // Returns false when the grid would be degenerate (caller falls back to
  // the walk): key-packing overflow, or a PBC chare spanning half the box.
  bool gridSelfUnion(const SubtreeRef& s) {
    const double b = std::sqrt(b2_);
    const double c = b / std::sqrt(6.0);
    const auto& box = s.root->data.box;
    const double ox = (double)box.lesser_corner.x;
    const double oy = (double)box.lesser_corner.y;
    const double oz = (double)box.lesser_corner.z;
    const double exx = (double)box.greater_corner.x - ox;
    const double exy = (double)box.greater_corner.y - oy;
    const double exz = (double)box.greater_corner.z - oz;
    const int64_t nx = (int64_t)(exx / c) + 1;
    const int64_t ny = (int64_t)(exy / c) + 1;
    const int64_t nz = (int64_t)(exz / c) + 1;
    if (nx > (1 << 20) || ny > (1 << 20) || nz > (1 << 20)) return false;
    if (period_.x > 0 && (exx > period_.x / 2 || exy > period_.y / 2 ||
                          exz > period_.z / 2))
      return false; // minimum-image identity with plain distance broken

    auto cellKey = [&](int64_t ix, int64_t iy, int64_t iz) -> uint64_t {
      return ((uint64_t)ix << 40) | ((uint64_t)iy << 20) | (uint64_t)iz;
    };
    std::vector<std::pair<uint64_t, int>> cells(s.n);
    for (int i = 0; i < s.n; i++) {
      int64_t ix = (int64_t)(((double)s.parts[i].position.x - ox) / c);
      int64_t iy = (int64_t)(((double)s.parts[i].position.y - oy) / c);
      int64_t iz = (int64_t)(((double)s.parts[i].position.z - oz) / c);
      if (ix < 0) ix = 0; if (ix >= nx) ix = nx - 1;
      if (iy < 0) iy = 0; if (iy >= ny) iy = ny - 1;
      if (iz < 0) iz = 0; if (iz >= nz) iz = nz - 1;
      cells[i] = {cellKey(ix, iy, iz), i};
    }
    std::sort(cells.begin(), cells.end());

    // Occupied-cell ranges over the sorted array; same-cell cliques union
    // through the first particle as representative.
    struct OccCell { uint64_t key; int begin, end, rep; };
    std::vector<OccCell> occ;
    for (int k = 0; k < (int)cells.size();) {
      int e = k + 1;
      int rep = s.offset + cells[k].second;
      while (e < (int)cells.size() && cells[e].first == cells[k].first) {
        unite(rep, s.offset + cells[e].second);
        e++;
      }
      occ.push_back({cells[k].first, k, e, rep});
      k = e;
    }

    auto findOcc = [&](uint64_t key) -> const OccCell* {
      int lo = 0, hi = (int)occ.size() - 1;
      while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (occ[mid].key == key) return &occ[mid];
        if (occ[mid].key < key) lo = mid + 1; else hi = mid - 1;
      }
      return nullptr;
    };

    const auto& offs = gridOffsets();
    for (auto& oc : occ) {
      int64_t ix = (int64_t)(oc.key >> 40);
      int64_t iy = (int64_t)((oc.key >> 20) & 0xFFFFF);
      int64_t iz = (int64_t)(oc.key & 0xFFFFF);
      for (auto& d : offs) {
        int64_t jx = ix + d[0], jy = iy + d[1], jz = iz + d[2];
        if (jx < 0 || jx >= nx || jy < 0 || jy >= ny || jz < 0 || jz >= nz)
          continue;
        const OccCell* nb = findOcc(cellKey(jx, jy, jz));
        if (nb == nullptr) continue;
        if (std::abs(d[0]) + std::abs(d[1]) + std::abs(d[2]) == 1) {
          unite(oc.rep, nb->rep); // face-adjacent: test-free
          continue;
        }
        if (find(oc.rep) == find(nb->rep)) continue; // already connected
        bool merged = false;
        for (int a = oc.begin; a < oc.end && !merged; a++) {
          const Particle& pa = s.parts[cells[a].second];
          for (int q = nb->begin; q < nb->end; q++) {
            const Particle& pb = s.parts[cells[q].second];
            if (paratreet::periodicDistSq(pa.position, pb.position, period_) <=
                b2_) {
              unite(s.offset + cells[a].second, s.offset + cells[q].second);
              merged = true; // one witness merges the components
              break;
            }
          }
        }
      }
    }
    return true;
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
        if (seen.insert(paratreet::packTipPair(lo, hi)).second)
          edge_buf.emplace_back(lo, hi);
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
        // Exact key (64-bit audit): the old 32|32 packing collides above
        // 4.29e9 particles, silently suppressing real edges.
        if (seen.insert(paratreet::packTipPair(lo, hi)).second)
          edge_buf.emplace_back(lo, hi);
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
  // uf_parent stays int: PE-LOCAL flat indices (a single PE never holds
  // 2^31 particles). flat_order holds GLOBAL orders: 64-bit.
  std::vector<int> uf_parent;  // per-PE UF over the flat index space
  std::vector<long> flat_order; // flat index -> global particle order
  std::vector<std::pair<long, long>> edge_buf;
  std::unordered_set<paratreet::TipPairKey, paratreet::TipPairKeyHash> seen;
  // Phase-3 cross-process buffers (kept separate from phaseB's, above).
  std::vector<std::pair<long, long>> edge_buf3;
  std::unordered_set<paratreet::TipPairKey, paratreet::TipPairKeyHash> seen3;
  // Set by applyTipEncoding, cleared by resetPhase3: gates the debug
  // same-process-edge check above (meaningless on raw serial-mode tips).
  bool tips_encoded_ = false;
  long phase3_emitted = 0;
  double b2_ = 0.0;
  // Occupancy gate for the per-chare grid (expected particles per cell of
  // side b/sqrt(6)); <= 0 disables the grid entirely (walk-only default —
  // laptop A/B showed parity-to-slightly-worse at reachable densities;
  // the deep-overdensity payoff regime needs the Anvil A/B).
  double grid_thresh_ = 0.0;
  // Final-reduction callback of the within-process chain (startPhase1Chain).
  CkCallback done_cb_;
  // Touched-component (negative-label) per-process totals held between
  // histogramShard and collectTouchedCounts.
  std::vector<std::pair<long, long>> touched_totals_;
  // Box period for PBC (design/pbc.md); {0,0,0} = open (default).
  Vector3D<Real> period_ = Vector3D<Real>(0, 0, 0);
};

namespace fof {

// Subtree-registration functor, delivered to every Subtree element through
// the core's generic Subtree::callPerSubtreeFn hook: hands the element's
// local tree root and contiguous particle block to the FoFPhase1 branch on
// the element's PE. This is the inversion that keeps the core FoF-free —
// the core exposes subtree views to any PerSubtreeAble, and this module
// consumes them; the core never names an FoF type.
template <typename Data>
class SubtreeRegisterFn : public paratreet::PerSubtreeAble<Data> {
  CProxy_FoFPhase1<Data> fof_;

 public:
  SubtreeRegisterFn(void) = default;
  explicit SubtreeRegisterFn(CProxy_FoFPhase1<Data> fof) : fof_(fof) {}
  SubtreeRegisterFn(CkMigrateMessage* m)
      : paratreet::PerSubtreeAble<Data>(m) {}

  // Hand-expanded PUPable_decl_template: the macro's register_PUP_ID body
  // calls register_constructor unqualified, which fails here because the
  // PUP::able base is reached through a DEPENDENT base class
  // (PerSubtreeAble<Data>) — unqualified lookup does not search dependent
  // bases. Same contents, with the call qualified.
 private:
  static PUP::able* call_PUP_constructor(void) {
    return new SubtreeRegisterFn<Data>((CkMigrateMessage*)0);
  }
  static PUP::able::PUP_ID my_PUP_ID;

 public:
  virtual const PUP::able::PUP_ID& get_PUP_ID(void) const override {
    return my_PUP_ID;
  }
  static void register_PUP_ID(const char* name) {
    my_PUP_ID = PUP::able::register_constructor(name, call_PUP_constructor);
  }

  virtual void pup(PUP::er& p) override {
    paratreet::PerSubtreeAble<Data>::pup(p);
    p | fof_;
  }

  virtual void operator()(Node<Data>* local_root, Particle* particles,
                          int n_particles) override {
    fof_.ckLocalBranch()->registerSubtree(local_root, particles, n_particles);
  }
};

template <typename Data>
PUP::able::PUP_ID SubtreeRegisterFn<Data>::my_PUP_ID = 0;

// Per-Data registration of this module's templated chares and PUPables.
// Applications call this from an overridden __register() in their Main
// subclass, after the base paratreet::Main<T>::__register() (see
// examples/fof3/Main.h). Mirrors the core's registration idiom.
template <typename Data>
void registerChares(void) {
  // Intentionally leaked: Charm++ registration keeps the pointer for the
  // lifetime of the program, and this runs once per type.
  auto makeName = [](const char* ty) {
    auto* name =
        new std::string(std::string(ty) + "<" + typeid(Data).name() + ">");
    return name->c_str();
  };
  CkIndex_FoFPhase1<Data>::__register(makeName("FoFPhase1"),
                                      sizeof(FoFPhase1<Data>));
  CkIndex_FoFPhase1Node<Data>::__register(makeName("FoFPhase1Node"),
                                          sizeof(FoFPhase1Node<Data>));
  PUPable_reg2(SubtreeRegisterFn<Data>, makeName("fof::SubtreeRegisterFn"));
}

}  // namespace fof

namespace paratreet {

// Per-stage wall times of runFoFPhase1. reset/register are still
// barrier-to-barrier on the driving thread. phaseA/phaseB/merge are
// PROCESS-LOCAL walls, max-reduced over processes (relabel: max over
// PEs) — no global barrier separates the stages anymore (the
// within-process chain, design/phase1-scaling.md 2026-07-25), so the
// stage values no longer include barrier latency and their SUM can
// exceed the phase-1 wall (stages of different processes overlap; the
// wall is the max over processes of each process's own sum).
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
                  FoFPhase1Stages* stages = nullptr,
                  // Occupancy gate for the per-chare grid in phaseA
                  // (expected particles per b/sqrt(6) cell at the chare
                  // root); <= 0 (default) disables the grid — enable via
                  // fof3 -G for the density-regime A/B (see
                  // design/phase1-scaling.md).
                  double grid_occupancy_threshold = 0.0) {
  double b2 = linking_length * linking_length;
  FoFPhase1Stages local;
  double t = CkWallTimer();
  fof_node.reset(CkCallbackResumeThread());
  fof.reset(CkCallbackResumeThread());
  // PBC (design/pbc.md): broadcast the box period to every PE branch before
  // phaseA. Default {0,0,0} = open boundaries (exact current behavior).
  fof.setPeriod(period, CkCallbackResumeThread());
  local.reset = CkWallTimer() - t; t = CkWallTimer();
  {
    fof::SubtreeRegisterFn<Data> reg_fn(fof);
    subtrees.callPerSubtreeFn(
        CkReference<paratreet::PerSubtreeAble<Data>>(reg_fn),
        CkCallbackResumeThread());
  }
  local.register_s = CkWallTimer() - t;
  // One broadcast starts the within-process chain; the single global
  // reduction at its end delivers the max-reduced stage walls.
  void* result = nullptr;
  fof.startPhase1Chain(b2, grid_occupancy_threshold,
                       CkCallbackResumeThread(result));
  {
    CkReductionMsg* m = (CkReductionMsg*)result;
    const double* v = (const double*)m->getData();
    local.phaseA = v[0];
    local.phaseB = v[1];
    local.merge = v[2];
    local.relabel = v[3];
    delete m;
  }
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

// Global component-size histogram over the final labels, computed FULLY
// DISTRIBUTED (see FoFPhase1::depositLabelCounts and the scheme comment
// there): untouched (positive-label) components are process-local, so
// their sizes are binned inside each process and SUM-reduced as a fixed
// 64-bin vector; only the few edge-touched (negative-label) components
// need cross-process per-label summing, gathered as a tiny concat. Run
// after the phase-3 relabel (labels must be global); call from a
// [threaded] context on PE 0. This is the stats-mode determinism
// observable: for a given input the resulting line must be bit-identical
// across process/PE configurations (and it computes the same multiset of
// sizes as the old 240-way concat gather it replaces).
template <typename Data>
FoFComponentHistogram runFoFComponentHistogram(CProxy_FoFPhase1<Data> fof,
                                               int min_component_size = 0) {
  // Stage 1: per-PE counts into the intra-process shard buffers.
  fof.depositLabelCounts(CkCallbackResumeThread());

  // Stage 2: shard merge + local binning of process-local components.
  FoFComponentHistogram h;
  std::memset(h.bins, 0, sizeof(h.bins));
  std::memset(h.surviving_bins, 0, sizeof(h.surviving_bins));
  h.min_component_size = min_component_size;
  {
    void* result = nullptr;
    fof.histogramShard(min_component_size, CkCallbackResumeThread(result));
    CkReductionMsg* msg = (CkReductionMsg*)result;
    CkReduction::tupleElement* elems = nullptr;
    int n_elems = 0;
    msg->toTuple(&elems, &n_elems);
    CkEnforce(n_elems == 6);
    std::memcpy(h.bins, elems[0].data, sizeof(h.bins));
    h.n_components = *(const long*)elems[1].data;
    h.max_size = *(const long*)elems[2].data;
    std::memcpy(h.surviving_bins, elems[3].data, sizeof(h.surviving_bins));
    h.surviving_count = *(const long*)elems[4].data;
    h.surviving_max_size = *(const long*)elems[5].data;
    delete[] elems;
    delete msg;
  }

  // Stage 3: cross-process summing of the touched components only
  // (~per-process #touched pairs; 7,029 labels total at 80M P=8).
  {
    void* result = nullptr;
    fof.collectTouchedCounts(CkCallbackResumeThread(result));
    CkReductionMsg* msg = (CkReductionMsg*)result;
    int n_pairs = msg->getSize() / sizeof(std::pair<long, long>);
    const auto* pairs = (const std::pair<long, long>*)msg->getData();
    std::unordered_map<long, long> totals;
    totals.reserve((size_t)n_pairs);
    for (int i = 0; i < n_pairs; i++) totals[pairs[i].first] += pairs[i].second;
    delete msg;
    for (auto& kv : totals) {
      long size = kv.second;
      int bin = 0; // floor(log2(size)); size >= 1 always
      while (bin < 63 && (1L << (bin + 1)) <= size) bin++;
      h.bins[bin]++;
      h.n_components++;
      if (size > h.max_size) h.max_size = size;
      if (size >= (long)min_component_size) {
        h.surviving_bins[bin]++;
        h.surviving_count++;
        if (size > h.surviving_max_size) h.surviving_max_size = size;
      }
    }
  }
  return h;
}

} // namespace paratreet

#endif // PARATREET_FOFPHASE1_H_
