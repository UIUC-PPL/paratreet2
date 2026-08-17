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
//       the TreePieces resident on this PE. Neighbor discovery is a dual tree
//       walk over all pairs of this PE's TreePieces (including self-pairs),
//       pruned when the box gap distance squared exceeds b^2.
//   (b) FoFPhase1 phaseB: for each TreePiece pair spanning two PEs of the same
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
//   fof_node.reset -> fof.reset -> treepieces.callPerTreePieceFn(TreePieceRegisterFn)
//   -> fof.phaseA -> fof.phaseB -> fof_node.merge -> fof.relabel
// Optionally after relabel (see paratreet::runFoFFragmentHistogram):
//   fof.countFragments -> fof_node.fragmentHistogram
//
// Lifetime contract: the Particle blocks registered through the generic
// TreePiece::callPerTreePieceFn hook (fof::TreePieceRegisterFn hands each TreePiece
// element's block to its PE's FoFPhase1 branch) are stable from the end of
// tree build until the next rebuild/reset; run the whole sequence inside
// that window.

#include "fof.decl.h"
// Template definitions of this module's generated code (CBase_/closure/proxy
// templates from fof.def.h) must be visible before any concrete-Data use of
// the chares (e.g. FoFPhase3.h's FragData visitors) — same idiom as the
// core's TreePiece.h including templates.h.
#include "fof-templates.h"
#include "CoreFunctions.h"
#include "common.h"
#include "Node.h"
#include "Particle.h"
#include "OrientedBox.h"
#include "unionFindLib.h"

#ifdef FOF_GPU
// design/phase1-gpu.md section 6.2: the device library's header names
// neither Kokkos nor HIP, so this translation unit stays a charmc TU.
// hapi.h is the exception — it needs the vendor's stream typedef, and
// hapi_portable.h does not pull the vendor header in itself. Build with
// -D__HIP_PLATFORM_AMD__ or hip_runtime_api.h declares nothing (trap 4).
#include "gpu/FoFDevice.h"
#if defined(CMK_HIP)
#include <hip/hip_runtime_api.h>
#endif
#if defined(CMK_HIP) || defined(CMK_CUDA)
#define FOF_GPU_HAS_HAPI 1
#include <hapi.h>
#endif
#endif

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

// Defined in FoF.C next to the keep-alive ring: one line per process of
// ring-message gap counters (the no-tracing stall monitor). Called from
// phase3Stats, once per process.
void fofKeepAliveGapsPrint(void);

// Fair phaseB work division (design/phase1-scaling.md, 2026-07-25): each
// unordered TreePiece pair spanning two PEs of a process is walked by
// exactly one of the two, chosen by one bit of a symmetric mix of the two
// TreePiece ROOT KEYS (stable Morton keys — identical on both sides, so
// both PEs agree without communication; pointers would vary under ASLR).
// TreePiece granularity splits every PE pair's ~64 TreePiece pairs about in
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

// Stage 4 (design/phase1-gpu.md section 18): what the device path is FOR.
//
//   Off      the CPU chain computes phase 1. No device work. Default.
//   Replace  the device computes phase 1 and the CPU chain does not run.
//            phaseA, phaseB, merge and relabel are skipped outright; the
//            device pass produces the four things section 4 names as the
//            contract (uf_parent, roots, rep_label, root_counts) plus
//            every particle's group_number, and everything downstream is
//            unchanged.
//   Verify   BOTH run: the CPU chain first, then the device pass, then an
//            exact per-particle comparison. This is the oracle mode of
//            section 6.4 and it is what every gate run uses. It is also
//            what FOF_GPU_STAGE0=1 meant through stages 0-3, so that
//            spelling still selects it.
//
// Replace is not the default and should not become one silently: it is
// selected per run by FOF_GPU_PHASE1=1, and a build without FOF_GPU
// refuses it rather than quietly falling back to the CPU (a silent
// fallback would turn "the GPU path regressed" into "the GPU path was
// never on", which is the failure this project has already paid for
// twice — traps 16 and 17).
enum class FoFGpuMode { Off, Replace, Verify };

inline FoFGpuMode fofGpuMode() {
  static const FoFGpuMode m = [] {
    auto on = [](const char* e) {
      const char* v = std::getenv(e);
      return v != nullptr && std::atoi(v) != 0;
    };
    if (on("FOF_GPU_VERIFY") || on("FOF_GPU_STAGE0")) return FoFGpuMode::Verify;
    if (on("FOF_GPU_PHASE1")) return FoFGpuMode::Replace;
    return FoFGpuMode::Off;
  }();
  return m;
}

} // namespace paratreet

// Per-process side of phase 1: collects the per-PE TreePiece registry (for
// phaseB pair enumeration), the cross-PE boundary edges, and — after
// merge() — the PE-tip -> process-level tip map read by the group branches.
template <typename Data>
class FoFPhase1Node : public CBase_FoFPhase1Node<Data> {
public:
  struct TreePieceRef {
    Node<Data>* root;
    Particle* parts;
    int n;
    const paratreet::DNode* dnodes = nullptr;  // src/DeviceTree.h; may be null
    int n_dnodes = 0;
  };

  std::mutex lock;
  // PE -> TreePieces resident on that PE (this process's PEs only).
  std::map<int, std::vector<TreePieceRef>> pe_treepieces;
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

#ifdef FOF_GPU
  // --- GPU staging (design/phase1-gpu.md stage 0, second half).
  // The process's single device, owned here because FoFPhase1Node is the
  // per-PROCESS branch and one process maps to one GPU by invariant.
  // Everything in this block is measurement scaffolding: it stages the
  // process's particles onto the device, round-trips them, and verifies
  // them, while the CPU chain below still computes the answer. Stage 2
  // replaces the round trip with the union-find; the staging halves
  // (K0/K5) are what survive.
  static_assert(sizeof(paratreet::DNode) == sizeof(fofgpu::DDNode),
                "DNode and the device library's DDNode must stay identical");
  fofgpu::Device device;
  void* dev_stream = nullptr;
  bool dev_inited = false;
  std::atomic<long> dev_total{0};   // process-flat particle count
  std::map<int, long> dev_pe_base;  // PE -> its base in that space (under lock)
  // Flat-tree layout plan, computed once on the home PE and then written
  // against concurrently: PE -> per-piece node offset (-1 = no tree).
  std::map<int, std::vector<int> > dev_piece_node_off;
  long dev_total_nodes = 0;
  double dev_t_tree_upload = 0, dev_t_tree_check = 0;
  long dev_tree_pieces = 0;
  // Per-iteration deposit counters. resetDevice() below MUST clear them:
  // they are compared against CkNodeSize(), so a counter left at its
  // end-of-iteration value never fires the trigger again and the second
  // iteration hangs with no error. Every run of this path so far has been
  // -i 1 (the default), which is the only reason it was never seen.
  std::atomic<int> dev_count_done{0};
  std::atomic<int> dev_pack_done{0};
  std::atomic<int> dev_scatter_done{0};
  double dev_t_launch = 0;          // enqueue -> completion callback
  double dev_t_block = 0;           // the part of the pass that BLOCKS a PE
  fofgpu::WalkStats dev_walk;
  int dev_launch_pe = -1;           // whichever PE packed last (stage 4)

  void resetDevice() {
    dev_total.store(0);
    dev_pe_base.clear();
    dev_piece_node_off.clear();
    dev_total_nodes = 0;
    dev_tree_pieces = 0;
    dev_t_tree_upload = dev_t_tree_check = 0;
    dev_count_done.store(0);
    dev_pack_done.store(0);
    dev_scatter_done.store(0);
    dev_t_launch = dev_t_block = 0;
    dev_launch_pe = -1;
    dev_walk = fofgpu::WalkStats();
  }
#endif

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

  // Per-process phaseA wall-clock deposit (design/phasea-reassignment.md
  // section 3, the skew-split instrument; INDEPENDENT of the union-find
  // mode — phaseA precedes either union-find). Each PE deposits its
  // t_phaseA once; phase3Stats reads sum and max to report the
  // within-process skew (proc max / proc avg) separately from the
  // cross-process skew (proc avg spread). One locked add per PE per
  // iteration, not hot-path.
  double a_time_sum = 0.0, a_time_max = 0.0;
  void depositPhaseATime(double t) {
    std::lock_guard<std::mutex> g(lock);
    a_time_sum += t;
    if (t > a_time_max) a_time_max = t;
  }
  // Stage split (see FoFPhase1::t_pa_self): per-process sum and max of
  // each stage, printed by printLoadModel's caller. max/(sum/nPEs) per
  // stage is the stage's within-process skew.
  double pa_self_sum = 0.0, pa_self_max = 0.0;
  double pa_cross_sum = 0.0, pa_cross_max = 0.0;
  void depositPhaseAStages(double self_t, double cross_t) {
    std::lock_guard<std::mutex> g(lock);
    pa_self_sum += self_t;
    if (self_t > pa_self_max) pa_self_max = self_t;
    pa_cross_sum += cross_t;
    if (cross_t > pa_cross_max) pa_cross_max = cross_t;
  }
  void printPhaseAStages() {
    CkPrintf("FOF3STAT phaseA_stages: node %d self_sum %.3f self_max %.3f "
             "cross_sum %.3f cross_max %.3f\n",
             CkMyNode(), pa_self_sum, pa_self_max, pa_cross_sum,
             pa_cross_max);
  }

  // ---- phaseA claim pool (campaign S1, FOF_STEALA=1;
  // design/phaseab-balancing.md section 14 and
  // design/phasea-reassignment.md). The frozen registry flattened into a
  // claimable list: any PE of the process may claim any piece (exclusive
  // ownership by CAS), own pieces first, then unclaimed siblings' —
  // nearest-centroid first when FOF_STEALA_GEO=1 (default; =0 is the
  // scan-order comparison arm, Kale's 1b). Built once per iteration
  // under the narrow lock by the first arriver (~hundreds of entries).
  struct StealEntry {
    Node<Data>* root;
    Particle* parts;
    int n;
    int home_pe;
    double c[3]; // box center, for the geometry claim priority
  };
  std::vector<StealEntry> steal_pool;
  std::unique_ptr<std::atomic<int>[]> steal_claimed;
  bool steal_built = false;
  // Direct steal evidence (Kale, 2026-08-12): per-process counts of
  // claims that landed on the piece's home PE vs a sibling ("foreign" =
  // an actual steal). Printed once per process at a_done completion.
  std::atomic<long> s1_own{0}, s1_foreign{0}, s1_pes_foreign{0};

  // Per-unit walltime histogram (log2 buckets of microseconds; Frontier
  // Opus ask 2026-08-12): with a ~1950x unit-cost spread, max and mean
  // cannot distinguish "a few giants over dust" from a smooth heavy
  // tail — and those imply different fixes. Accumulated per process,
  // printed once at the merge.
  static constexpr int kUnitHistBuckets = 28;
  std::atomic<long> pb_unit_hist[kUnitHistBuckets] = {};
  void printUnitHist() {
    printOneHist("pb_unit_hist", pb_unit_hist, "log2us");
  }
  void printOneHist(const char* name, std::atomic<long>* hist,
                    const char* unit) {
    long total = 0;
    for (int i = 0; i < kUnitHistBuckets; i++) total += hist[i].load();
    if (total == 0) return;
    char buf[512];
    int off = snprintf(buf, sizeof(buf), "FOF3STAT %s: node %d %s",
                       name, CkMyNode(), unit);
    for (int i = 0; i < kUnitHistBuckets; i++) {
      long c = hist[i].load();
      if (c > 0 && off < (int)sizeof(buf) - 24)
        off += snprintf(buf + off, sizeof(buf) - off, " %d:%ld", i, c);
    }
    CkPrintf("%s\n", buf);
  }

  void ensureStealPool() {
    std::lock_guard<std::mutex> g(lock);
    if (steal_built) return;
    for (auto& kv : pe_treepieces)
      for (auto& s : kv.second) {
        StealEntry e;
        e.root = s.root; e.parts = s.parts; e.n = s.n; e.home_pe = kv.first;
        for (int ax = 0; ax < 3; ax++)
          e.c[ax] = 0.5 * ((double)s.root->data.box.lesser_corner[ax] +
                           (double)s.root->data.box.greater_corner[ax]);
        steal_pool.push_back(e);
      }
    steal_claimed.reset(new std::atomic<int>[steal_pool.size()]);
    for (size_t i = 0; i < steal_pool.size(); i++) steal_claimed[i] = 0;
    steal_built = true;
  }

  // ---- Responsiveness + transfer probe (campaign P0, FOF_PROBE=1;
  // design/phaseab-balancing.md section 14). The coordinator process of
  // each physical-node domain (lowest process of a FOF_PROCS_PER_PNODE
  // block, default 8) pings every domain sibling every FOF_PROBE_MS
  // (default 25) while armed; RTTs answer the campaign's central
  // question -- does a process attend to messages while its PEs are
  // inside phaseA/B? At report time a one-shot transfer ladder to the
  // next process prices shipment sizes on the real transport (reconverse
  // has no cross-process shared memory, so this is NIC loopback by
  // construction; the latency signature confirms it). Charm "node" =
  // process; the physical-node domain comes from the env, not the
  // runtime.
  static int probeProcsPerPnode() {
    static const int v = [] {
      const char* e = std::getenv("FOF_PROCS_PER_PNODE");
      return e ? std::max(1, std::atoi(e)) : 8;
    }();
    return v;
  }
  static int probePeriodMs() {
    static const int v = [] {
      const char* e = std::getenv("FOF_PROBE_MS");
      return e ? std::max(1, std::atoi(e)) : 25;
    }();
    return v;
  }
  bool probe_armed = false;
  std::map<int, std::vector<double>> probe_rtts; // target proc -> rtt seconds
  CkCallback probe_report_cb;
  size_t probe_xfer_idx = 0;
  std::vector<std::pair<size_t, double>> probe_xfer_rtts;

  static void probeTickThunk(void* branch, double) {
    ((FoFPhase1Node<Data>*)branch)->probeTick();
  }

  void probeTick() {
    if (!probe_armed) return;
    int ppn = probeProcsPerPnode();
    int base = (CkMyNode() / ppn) * ppn;
    int hi = std::min(base + ppn, CkNumNodes());
    double now = CkWallTimer();
    for (int p = base; p < hi; p++)
      if (p != CkMyNode()) this->thisProxy[p].probePing(CkMyNode(), now);
    CcdCallFnAfter(probeTickThunk, this, probePeriodMs());
  }

  void probeStart(const CkCallback& cb) {
    if (std::getenv("FOF_PROBE")) {
      probe_rtts.clear();
      int ppn = probeProcsPerPnode();
      if (CkMyNode() % ppn == 0 && CkNumNodes() > 1) {
        probe_armed = true;
        CcdCallFnAfter(probeTickThunk, this, probePeriodMs());
      }
    }
    this->contribute(cb);
  }

  void probePing(int from, double t0) {
    this->thisProxy[from].probeAck(CkMyNode(), t0);
  }

  void probeAck(int target, double t0) {
    std::lock_guard<std::mutex> g(lock);
    probe_rtts[target].push_back(CkWallTimer() - t0);
  }

  // Transfer ladder: one size at a time to the next process; the ack
  // carries no payload, so RTT ~ one-way transfer + small.
  void probeXfer(int from, double t0, const std::vector<char>& payload) {
    this->thisProxy[from].probeXferAck(t0, (long)payload.size());
  }

  void probeXferAck(double t0, long bytes) {
    probe_xfer_rtts.emplace_back((size_t)bytes, CkWallTimer() - t0);
    probeXferNext();
  }

  void probeXferNext() {
    static const size_t sizes[] = {4096, 65536, 1048576, 8388608};
    if (probe_xfer_idx < 4 && CkNumNodes() > 1) {
      size_t sz = sizes[probe_xfer_idx++];
      std::vector<char> payload(sz, 1);
      this->thisProxy[CkMyNode() + 1].probeXfer(CkMyNode(), CkWallTimer(),
                                                payload);
      return;
    }
    // Ladder done: print everything and close the report reduction.
    for (auto& kv : probe_rtts) {
      auto v = kv.second;
      if (v.empty()) continue;
      std::sort(v.begin(), v.end());
      size_t n = v.size();
      CkPrintf("FOF3STAT probe: proc %d->%d rtt_us min %.0f med %.0f "
               "p99 %.0f max %.0f n %zu\n",
               CkMyNode(), kv.first, v[0] * 1e6, v[n / 2] * 1e6,
               v[std::min(n - 1, n * 99 / 100)] * 1e6, v[n - 1] * 1e6, n);
    }
    for (auto& pr : probe_xfer_rtts)
      CkPrintf("FOF3STAT probe_xfer: proc %d->%d bytes %zu rtt_us %.0f\n",
               CkMyNode(), CkMyNode() + 1, pr.first, pr.second * 1e6);
    probe_rtts.clear();
    probe_xfer_rtts.clear();
    this->contribute(probe_report_cb);
  }

  void probeReport(const CkCallback& cb) {
    probe_report_cb = cb;
    bool was_armed = probe_armed;
    probe_armed = false; // stop the tick chain
    if (std::getenv("FOF_PROBE") && was_armed) {
      probe_xfer_idx = 0;
      probeXferNext(); // ladder, then print + contribute
    } else {
      this->contribute(cb);
    }
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
  void registerTreePiece(int pe, Node<Data>* root, Particle* parts, int n,
                         const paratreet::DNode* dnodes = nullptr,
                         int n_dnodes = 0) {
    std::lock_guard<std::mutex> g(lock);
    // Field assignment, not brace init: TreePieceRef has default member
    // initializers, which under charmc's -std=gnu++11 makes it a
    // non-aggregate.
    TreePieceRef ref;
    ref.root = root; ref.parts = parts; ref.n = n;
    ref.dnodes = dnodes; ref.n_dnodes = n_dnodes;
    pe_treepieces[pe].push_back(ref);
  }

  // Per-process phaseB wall deposit, the phaseA analogue above. Needed
  // because the load model predicts a PROCESS's phase-1 cost, while the
  // balance line only reports min/avg/max over PEs globally.
  double b_time_sum = 0.0, b_time_max = 0.0;
  void depositPhaseBTime(double t) {
    std::lock_guard<std::mutex> g(lock);
    b_time_sum += t;
    if (t > b_time_max) b_time_max = t;
  }

  // PREDICTED vs ACTUAL, one line per process (design/piece-load-model.md;
  // this is the cost model the campaign validated and kept — the migration
  // mechanisms it once fed are retired, tag campaign-2026-08-stealing):
  //   self = sum n^1.2 over this process's pieces          (phaseA model)
  //   pair = sum n^2 / V^(4/3) over the same pieces        (phaseB model,
  //          mean-field neighbours; the constant b^4 is absorbed)
  // Fit sum(actual phaseA) ~ a*self and sum(actual phaseB) ~ c*pair across
  // processes: the R^2 of those two regressions is the go/no-go for acting
  // on the model, and the ratio c/a calibrates the two terms' weights.
  // m2 between two pieces, the SAME expected-pairs estimate the pool uses
  // per unit (FoFPhase1.h ~2489, validated R^2 0.87 at 2B), evaluated at
  // piece-root granularity: rho_a * rho_b * V_int(a grown by b, b) * V_ball.
  // With a == b it degenerates to the piece's own self-pair estimate, so
  // ONE formula covers self work and pair work.
  static double pieceM2(const TreePieceRef& a, const TreePieceRef& b,
                        double b2) {
    double va = (double)a.root->data.box.volume();
    double vb = (double)b.root->data.box.volume();
    if (va <= 0 || vb <= 0) return 0.0;
    const double bb = std::sqrt(b2);
    double vint = 1.0;
    for (int ax = 0; ax < 3 && vint > 0; ax++) {
      double lo = std::max((double)a.root->data.box.lesser_corner[ax] - bb,
                           (double)b.root->data.box.lesser_corner[ax]);
      double hi = std::min((double)a.root->data.box.greater_corner[ax] + bb,
                           (double)b.root->data.box.greater_corner[ax]);
      vint = hi > lo ? vint * (hi - lo) : 0.0;
    }
    if (vint <= 0) return 0.0;
    const double vball = 4.18879 * bb * bb * bb;
    return ((double)a.root->data.n_below / va) *
           ((double)b.root->data.n_below / vb) * vint * vball;
  }

  // Predicted phase-1 work of THIS process UNDER THE CURRENT ASSIGNMENT
  // (Kale, 2026-08-15). The point of the estimate is only to say what this
  // process will do if nothing moves — it deliberately does NOT try to
  // predict what shedding a piece would do to pair work, which is
  // unknowable cheaply and unnecessary for choosing a victim.
  //
  // So it sums m2 over the pairs the assignment ACTUALLY schedules:
  //   m2_self  = sum_i  m2(i,i)          -> the phaseA self-pair term
  //   m2_intra = sum over pairs on the SAME PE   -> rest of phaseA
  //   m2_cross = sum over pairs on DIFFERENT PEs -> phaseB (the pool)
  // The earlier version summed a per-piece MEAN-FIELD proxy
  // (n^1.2 and n^2/V^(4/3)) that never looked at a neighbour; it reached
  // r=+0.871 against phaseB on both machines, but its self term was
  // anti-correlated (-0.19 Anvil, -0.26 Frontier) and unusable. Both proxies
  // are still printed so the two can be compared on one run.
  // Cost: O(pieces^2) per process, ~141k evaluations at 531 pieces — but it
  // runs once, off the critical path, at phase3Stats.
  void printLoadModel(double b2) {
    static const double self_exp = [] {
      const char* e = std::getenv("PARATREET_LB_SELF_EXP");
      return e ? std::atof(e) : 1.2;
    }();
    std::vector<const TreePieceRef*> all;
    std::vector<int> pe_of;
    double self_proxy = 0, pair_proxy = 0, np = 0;
    for (auto& kv : pe_treepieces)
      for (auto& s : kv.second) {
        if (s.n <= 0 || s.root == nullptr) continue;
        all.push_back(&s);
        pe_of.push_back(kv.first);
        double n = (double)s.n, v = (double)s.root->data.box.volume();
        self_proxy += std::pow(n, self_exp);
        if (v > 0) pair_proxy += n * n / std::pow(v, 4.0 / 3.0);
        np += n;
      }
    double m2_self = 0, m2_intra = 0, m2_cross = 0;
    for (size_t i = 0; i < all.size(); i++) {
      m2_self += pieceM2(*all[i], *all[i], b2);
      for (size_t j = i + 1; j < all.size(); j++) {
        double m = pieceM2(*all[i], *all[j], b2);
        if (m <= 0) continue;
        if (pe_of[i] == pe_of[j]) m2_intra += m; else m2_cross += m;
      }
    }
    CkPrintf("FOF3STAT load_model: node %d pieces %zu n %.0f "
             "m2_self %.6g m2_intra %.6g m2_cross %.6g "
             "self %.6g pair %.6g "
             "pa_sum_s %.3f pa_max_s %.3f pb_sum_s %.3f pb_max_s %.3f\n",
             CkMyNode(), all.size(), np, m2_self, m2_intra, m2_cross,
             self_proxy, pair_proxy, a_time_sum, a_time_max,
             b_time_sum, b_time_max);
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
  // PhaseB dynamic pool (branch phaseb-pool): every cross-PE TreePiece pair
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
    double m2;  // expected-pairs cost estimate (campaign S2; 0.87 R2 at 2B)
  };
  std::vector<PoolUnit> phaseb_pool;
  std::atomic<size_t> phaseb_next{0};
  // Campaign S2: KD partitioning of the pool (FOF_PB_PARTS > 0). The
  // pool is reordered partition-contiguous; each partition has a range,
  // an m2 cost, a claim flag (a PE claims a whole partition, costliest
  // unclaimed first — optimistic pick + CAS, same shape as the S1 piece
  // claims), and a unit cursor (fetch_add within the claimed partition).
  std::vector<std::pair<uint32_t, uint32_t>> pb_part_range;
  std::vector<double> pb_part_cost;
  std::unique_ptr<std::atomic<int>[]> pb_part_claimed;
  std::unique_ptr<std::atomic<uint32_t>[]> pb_part_next;
  // Campaign S2b (FOF_PB_MERGE=1, requires FOF_PB_PARTS): piece-REGION
  // formulation with a process-level barrier. Units are classified
  // intra-region (B1, partitioned by region) vs cross-region (B2, flat
  // tail from pb_b2_begin); after B1 drains, the last depositor runs the
  // mid-phase union-find over the B1 edges (the L2 path compression),
  // every PE relabels + re-annotates (three-part rule; B1 edges are
  // RETAINED for the final merge), then B2 runs over compressed tips.
  int pb_stage = 0;             // 0 flat/unit-KD; 1 = B1; 2 = B2
  size_t pb_b2_begin = 0;
  std::atomic<size_t> pb_b2_next{0};
  std::atomic<int> b1_done{0};
  std::atomic<int> mid_done{0};
  double stage_tM1 = 0.0;       // mid-merge wall (diagnostic)
  // Parallel pool build (2026-08-07): each of the process's threads
  // enumerates a stripe of the TreePiece-pair space into its own slice —
  // no lock, no sharing — and the last one to finish concatenates and
  // sorts. The build used to run entirely on the thread that finished
  // phaseA last while the other fourteen waited, which is what made
  // finer unit splitting cost more than it saved.
  std::vector<std::vector<PoolUnit>> pool_slices;
  std::atomic<int> slice_done{0};

  void reset(const CkCallback& cb) {
    label_shards.clear();
    phaseb_pool.clear();
    phaseb_next = 0;
    pool_slices.clear();
    slice_done = 0;
    chain_started = 0;
    a_done = 0;
    b_done = 0;
    chain_t0 = stage_tA = stage_tB = stage_tM = 0;
    pe_treepieces.clear();
    edges.clear();
    tip_map.clear();
    frag_counts.clear();
    encode_map.clear();
    uf2_vertices.clear();
    uf2_labels.clear();
    global_slice.clear();
    steal_pool.clear();
    steal_claimed.reset();
    steal_built = false;
    s1_own = 0;
    s1_foreign = 0;
    s1_pes_foreign = 0;
    for (int i = 0; i < kUnitHistBuckets; i++) pb_unit_hist[i] = 0;
    pb_part_range.clear();
    pb_part_cost.clear();
    pb_part_claimed.reset();
    pb_part_next.reset();
    pb_stage = 0;
    pb_b2_begin = 0;
    pb_b2_next = 0;
    b1_done = 0;
    mid_done = 0;
    stage_tM1 = 0.0;
    a_time_sum = 0.0;
    a_time_max = 0.0;
    pa_self_sum = pa_self_max = pa_cross_sum = pa_cross_max = 0.0;
    b_time_sum = 0.0;
    b_time_max = 0.0;
    clearSeen();
#ifdef FOF_GPU
    resetDevice();
#endif
    this->contribute(cb);
  }

  // Stage 3 (design/relabel-representative.md): this process's slice of
  // the phase-3 label map, sharded by owner at processor 0. Stored ONCE
  // per process; every PE then probes it read-only at representative
  // granularity (FoFPhase1::applySliceOnPE). The store completes before
  // the per-PE trigger messages are sent, and message delivery orders the
  // reads after it — same publication pattern as the phase-1 deposit
  // chain.
  std::unordered_map<long, long> global_slice;
  void applyGlobalMapSlice(const std::vector<std::pair<long, long>>& slice,
                           const CkGroupID& fof_gid, const CkCallback& cb) {
    global_slice.clear();
    global_slice.insert(slice.begin(), slice.end());
    CProxy_FoFPhase1<Data> fof_proxy(fof_gid);
    int first = CkNodeFirst(CkMyNode());
    for (int pe = first; pe < first + CkNodeSize(CkMyNode()); pe++)
      fof_proxy[pe].applySliceOnPE(cb);
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
  struct TreePieceRef {
    Node<Data>* root;
    Particle* parts; // the TreePiece's own particle block (stable until rebuild)
    int n;
    int offset;      // base of this block in the PE-flat index space
    // Flat device tree for this TreePiece (src/DeviceTree.h), or null
    // when PARATREET_DEVICE_TREE is off. Owned by the TreePiece and
    // stable for the same window as `parts`.
    const paratreet::DNode* dnodes = nullptr;
    int n_dnodes = 0;
  };

  FoFPhase1(CProxy_FoFPhase1Node<Data> node_proxy_) : node_proxy(node_proxy_) {}

  // Synchronous, called on this PE by fof::TreePieceRegisterFn (delivered per
  // TreePiece element through the generic TreePiece::callPerTreePieceFn hook).
  void registerTreePiece(Node<Data>* root, Particle* parts, int n,
                         const paratreet::DNode* dnodes = nullptr,
                         int n_dnodes = 0) {
    TreePieceRef ref;
    ref.root = root; ref.parts = parts; ref.n = n; ref.offset = 0;
    ref.dnodes = dnodes; ref.n_dnodes = n_dnodes;
    treepieces.push_back(ref);
    node_proxy.ckLocalBranch()->registerTreePiece(CkMyPe(), root, parts, n,
                                                  dnodes, n_dnodes);
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
    treepieces.clear();
    uf_parent.clear();
    flat_order.clear();
    roots.clear();
    rep_label.clear();
    root_counts.clear();
    frozen_ = false;
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
  // TreePieces (self-pairs included), then full path compression and tip
  // assignment into Particle::group_number.
  // Campaign S1 arms.
  static bool stealA() {
    static const bool on = [] {
      const char* e = std::getenv("FOF_STEALA");
      return e && std::atoi(e) != 0;
    }();
    return on;
  }
  static bool stealGeo() {
    static const bool on = [] {
      const char* e = std::getenv("FOF_STEALA_GEO");
      return !e || std::atoi(e) != 0; // geometry priority is the default arm
    }();
    return on;
  }

  // Accounting + flat-index bookkeeping for one piece entering this PE's
  // phaseA work set (shared by the static and claim paths).
  void phaseAAdmit(TreePieceRef& s) {
    s.offset = (int)uf_parent.size();
    uf_parent.resize(s.offset + s.n);
    std::iota(uf_parent.begin() + s.offset, uf_parent.end(), s.offset);
    flat_order.resize(s.offset + s.n);
    for (int i = 0; i < s.n; i++) flat_order[s.offset + i] = s.parts[i].order;
    double vol = (double)s.root->data.box.volume();
    if (vol > 0) density_x += (double)s.n * (double)s.n / vol;
    size_x += std::pow((double)s.n, 1.28);
    if ((long)s.n > max_piece_n) max_piece_n = s.n;
  }

  // One SELF pair (grid gate + walk), shared by both paths.
  void phaseASelfPair(const TreePieceRef& sa) {
    if (grid_thresh_ > 0 && sa.n >= 64) {
      double vol = (double)sa.root->data.box.volume();
      double b = std::sqrt(b2_);
      double c = b / std::sqrt(6.0);
      if (vol > 0 && (double)sa.n * c * c * c / vol >= grid_thresh_ &&
          gridSelfUnion(sa))
        return;
    }
    grid_ctx_ = &sa;
    phaseAWalkPair(sa, sa);
    grid_ctx_ = nullptr;
  }

  // One pair walk with the standard phaseA callbacks (self or cross).
  void phaseAWalkPair(const TreePieceRef& sa, const TreePieceRef& sb) {
    walk(sa.root, sb.root,
         [&](Node<Data>* a, Node<Data>* b) { leafLeafUnion(a, b, sa, sb); },
         [&](Node<Data>* a, Node<Data>* b) {
           int ra = certRep(a, sa);
           if (a != b) unite(ra, certRep(b, sb));
         },
         [&](Node<Data>* a, Node<Data>* b) {
           int ra = connectedRep(a, sa);
           if (ra < 0) return false;
           if (a == b) { p1_conn_suppressed++; return true; }
           int rb = connectedRep(b, sb);
           if (rb < 0) return false;
           if (find(ra) == find(rb)) { p1_conn_suppressed++; return true; }
           return false;
         });
  }

  // The claim path (FOF_STEALA=1): rebuild this PE's work set by claiming
  // pieces from the process-wide pool — own pieces first, then unclaimed
  // siblings' (nearest-centroid first under FOF_STEALA_GEO=1). Self pair
  // runs at claim time (self-first ordering preserved piece by piece);
  // the cross pass runs over the realized set afterwards. Ends by
  // RE-KEYING this PE's registry bucket to the realized assignment — the
  // constraint that keeps buildPoolSlice enumerating cross-ASSIGNMENT
  // pairs (the silent-under-merge trap; fof1 phase-1-exact is the guard).
  void phaseAClaims() {
    const double t_claims0 = CkWallTimer();
    auto* nb = node_proxy.ckLocalBranch();
    nb->ensureStealPool();
    auto& pool = nb->steal_pool;
    auto* claimed = nb->steal_claimed.get();
    treepieces.clear();
    uf_parent.clear();
    flat_order.clear();
    // My claim-priority reference point: the centroid of my HOME pieces.
    double myc[3] = {0, 0, 0};
    int nown = 0;
    for (auto& e : pool)
      if (e.home_pe == CkMyPe()) {
        for (int ax = 0; ax < 3; ax++) myc[ax] += e.c[ax];
        nown++;
      }
    if (nown > 0)
      for (int ax = 0; ax < 3; ax++) myc[ax] /= nown;
    long own_claims = 0, foreign_claims = 0;
    auto claim = [&](size_t i) {
      int expect = 0;
      if (!claimed[i].compare_exchange_strong(expect, 1)) return false;
      (pool[i].home_pe == CkMyPe()) ? own_claims++ : foreign_claims++;
      TreePieceRef r{pool[i].root, pool[i].parts, pool[i].n, 0};
      treepieces.push_back(r);
      phaseAAdmit(treepieces.back());
      phaseASelfPair(treepieces.back());
      return true;
    };
    // Own pieces first (a faster sibling may already hold some — that IS
    // the stealing).
    for (size_t i = 0; i < pool.size(); i++)
      if (pool[i].home_pe == CkMyPe()) claim(i);
    // Then unclaimed work, geometry-preferring or scan-order.
    for (;;) {
      size_t pick = pool.size();
      if (stealGeo()) {
        double best = 0;
        for (size_t i = 0; i < pool.size(); i++) {
          if (claimed[i].load(std::memory_order_relaxed) != 0) continue;
          double d = 0;
          for (int ax = 0; ax < 3; ax++) {
            double dx = pool[i].c[ax] - myc[ax];
            d += dx * dx;
          }
          if (pick == pool.size() || d < best) { pick = i; best = d; }
        }
      } else {
        for (size_t i = 0; i < pool.size(); i++)
          if (claimed[i].load(std::memory_order_relaxed) == 0) { pick = i; break; }
      }
      if (pick == pool.size()) break; // pool drained
      claim(pick); // CAS may lose a race; loop re-scans either way
    }
    t_pa_self = CkWallTimer() - t_claims0;
    // Cross pairs over the realized set (self pairs already done).
    const double t_cross0 = CkWallTimer();
    for (size_t i = 0; i < treepieces.size(); i++)
      for (size_t j = i + 1; j < treepieces.size(); j++)
        phaseAWalkPair(treepieces[i], treepieces[j]);
    t_pa_cross = CkWallTimer() - t_cross0;
    // RE-KEY: my registry bucket = my realized assignment, before the
    // a_done deposit publishes it to the pool build.
    {
      std::lock_guard<std::mutex> g(nb->lock);
      auto& bucket = nb->pe_treepieces[CkMyPe()];
      bucket.clear();
      for (auto& s : treepieces)
        bucket.push_back({s.root, s.parts, s.n});
    }
    nb->s1_own += own_claims;
    nb->s1_foreign += foreign_claims;
    if (foreign_claims > 0) nb->s1_pes_foreign++;
  }

  // Per-PE phaseA stage split (instrument only, 2026-08-17): how much of
  // this PE's phaseA is the SELF-walk stage (stealable under Kale's
  // barrier scheme — any PE can self-walk any piece) vs the
  // cross-piece-within-PE stage (owner-bound there). The within-process
  // skew of each stage, read across a process's PEs, is what decides
  // whether the barrier scheme captures the 1.28 within-skew or only the
  // self share of it. Set by BOTH the static and the claim paths.
  double t_pa_self = 0.0, t_pa_cross = 0.0;
  void phaseABody(double b2) {
    double t0 = CkWallTimer();
    frozen_ = false;
    b2_ = b2;
    density_x = 0.0;
    size_x = 0.0;
    max_piece_n = 0;
    cert_rep.clear();
    p1_conn_suppressed = 0;
    if (stealA()) {
      phaseAClaims();
      phaseAFreeze(t0);
      return;
    }
    // Static path (default): offset table over the registered pieces.
    int n_local = 0;
    for (auto& s : treepieces) {
      s.offset = n_local;
      n_local += s.n;
      double vol = (double)s.root->data.box.volume();
      if (vol > 0) density_x += (double)s.n * (double)s.n / vol;
      // Size predictor for phaseA SELF-pair cost (cost-model probe: cost
      // ~ n^1.2-1.3, R2 ~0.9; the density predictor above collapses at
      // scale because suppression linearizes dense regions). Independent
      // of the union-find mode.
      size_x += std::pow((double)s.n, 1.28);
      if ((long)s.n > max_piece_n) max_piece_n = s.n;
    }
    uf_parent.resize(n_local);
    std::iota(uf_parent.begin(), uf_parent.end(), 0);
    flat_order.resize(n_local);
    for (auto& s : treepieces)
      for (int i = 0; i < s.n; i++) flat_order[s.offset + i] = s.parts[i].order;

    // Self pairs FIRST, cross pairs after (merge-early ordering): local
    // assembly populates the connectivity memo, so the cross-pair walks see
    // maximal suppression (design/phase1-scaling.md, connectivity layer).
    p1_conn_suppressed = 0;
    double t_pass0 = CkWallTimer();
    for (int pass = 0; pass < 2; pass++) {
      if (pass == 1) {
        t_pa_self = CkWallTimer() - t_pass0;
        t_pass0 = CkWallTimer();   // reused as the cross-stage start
      }
      for (size_t i = 0; i < treepieces.size(); i++) {
        for (size_t j = i; j < treepieces.size(); j++) {
          if ((pass == 0) != (i == j)) continue; // pass 0: self; pass 1: cross
          const TreePieceRef& sa = treepieces[i];
          const TreePieceRef& sb = treepieces[j];
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
          // Per-level grid context: set only for SELF pairs (a == b can
          // never occur inside a cross-piece walk — the two sides descend
          // different trees), consumed by walk's self-node gate.
          grid_ctx_ = (i == j) ? &sa : nullptr;
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
    grid_ctx_ = nullptr;
    t_pa_cross = CkWallTimer() - t_pass0;
    phaseAFreeze(t0);
  }

  // Shared phaseA tail (static and claim paths): freeze + compress +
  // annotate + representative structures + component counts.
  void phaseAFreeze(double t0) {
    // Freeze + compress: write tip id (order of the component's min-order
    // root particle) into every particle. Component counting rides this
    // pass (Kale's design, 2026-08-06): one dense-array increment per
    // particle beside the find() that already runs, replacing the
    // component counter's separate full-particle hashing pass. The counts
    // stay keyed by ROOT INDEX (root_counts); labels live in rep_label and
    // every later rewrite (relabelBody, applyTipEncoding, applyGlobalMap,
    // applyUF2Labels) touches only rep_label[roots], so depositLabelCounts
    // deposits (rep_label[r], root_counts[r]) pairs directly
    // (design/relabel-representative.md).
    root_counts.assign(uf_parent.size(), 0);
    if (tipAnnotate()) {
      // Fused: the freeze and the node annotation both touch every
      // particle, and the annotation reads back the group_number the
      // freeze has just written, so they are one walk. Iterating leaves
      // rather than the flat block is the only difference - a leaf's
      // particles are a contiguous slice of the block, so the flat index
      // the union-find needs is offset + (slice base) + j.
      for (auto& s : treepieces) {
        long covered = freezeAndAnnotate(s.root, s, root_counts);
        // Every particle of the block must belong to exactly one leaf,
        // or the tips and the component counts that ride this loop would
        // both be short. Cheap to check, and it protects Kale's
        // freeze-pass component counting.
        CkEnforce(covered == (long)s.n);
      }
    } else {
      for (auto& s : treepieces)
        for (int i = 0; i < s.n; i++) {
          int r = find(s.offset + i);
          s.parts[i].group_number = flat_order[r];
          root_counts[r]++;
        }
    }
    // Freeze the representative structure: from here on, label maps touch
    // only rep_label[roots] and particles are rewritten by
    // materializeLabels()'s indexed load.
    roots.clear();
    rep_label.assign(uf_parent.size(), -1);
    for (size_t r = 0; r < root_counts.size(); r++)
      if (root_counts[r] > 0) {
        roots.push_back((int)r);
        rep_label[r] = flat_order[r];
      }
    frozen_ = true;

    // Write the frozen tips into the node annotation, bottom up.
    //
    // phaseA cannot use min_frag/max_frag itself: its tips are LIVE, the
    // union-find merges fragments as the walk proceeds, so a value stored
    // in a node is stale at the next union. That is why phaseA answers
    // "is this node internally connected" through cert_rep, a memo over
    // union-find roots whose entries stay valid across merges.
    //
    // phaseB has no such constraint. The freeze pass above wrote a final
    // group_number into every particle and nothing changes it until the
    // merge, so from here the annotation is simply data: constant time to
    // test, hereditary, and carried in the node rather than in a
    // per-processor map keyed by node address.
    //
    // Superseded by TreePiece::upwardPass after relabel, which is what
    // phase 3 reads. Uniformity is monotone across the merge, so a node
    // uniform here is still uniform there.
    // (The annotation is written by the fused freeze walk above; it used
    // to be a second pass over every particle, which cost about 0.10 s
    // at 8M and made the change a net loss at small scale.)
    t_phaseA = CkWallTimer() - t0; // per-PE load signal, reduced by phase3Stats
  }

  static bool tipAnnotate() {
    static const bool on = [] {
      const char* e = std::getenv("FOF_TIP_ANNOTATE");
      return !e || std::atoi(e) != 0;   // FOF_TIP_ANNOTATE=0 for the A/B
    }();
    return on;
  }
  double t_tip_annotate = 0.0;
  // Empty nodes keep the identity range, so uniform() stays false for
  // them, exactly as FragData's own constructor intends.
  // Freeze and annotate in one walk. Returns how many particles it
  // covered so the caller can confirm the leaves account for the whole
  // block. Writes each particle's final tip, feeds root_counts for the
  // component counting that rides this pass, and leaves every node
  // carrying the min and max tip beneath it.
  long freezeAndAnnotate(Node<Data>* n, const TreePieceRef& s,
                         std::vector<long>& root_counts) {
    const long kMin = std::numeric_limits<long>::max();
    const long kMax = std::numeric_limits<long>::min();
    if (n == nullptr) return 0;
    long mn = kMin, mx = kMax, covered = 0;
    if (n->isLeaf()) {
      const int base = (int)(n->particles() - s.parts);
      for (int j = 0; j < n->n_particles; j++) {
        int r = find(s.offset + base + j);
        long g = flat_order[r];
        s.parts[base + j].group_number = g;
        root_counts[r]++;
        if (g < mn) mn = g;
        if (g > mx) mx = g;
      }
      covered = n->n_particles;
    } else {
      for (int i = 0; i < n->n_children; i++) {
        Node<Data>* c = n->getChild(i);
        if (c == nullptr) continue;
        covered += freezeAndAnnotate(c, s, root_counts);
        if (c->data.min_frag < mn) mn = c->data.min_frag;
        if (c->data.max_frag > mx) mx = c->data.max_frag;
      }
    }
    n->data.min_frag = mn;
    n->data.max_frag = mx;
    return covered;
  }

  // Kept for reference and for any caller that needs the annotation
  // without the freeze; the production path uses the fused walk above.
  std::pair<long, long> annotateFrozenTips(Node<Data>* n) {
    const long kMin = std::numeric_limits<long>::max();
    const long kMax = std::numeric_limits<long>::min();
    if (n == nullptr) return {kMin, kMax};
    long mn = kMin, mx = kMax;
    if (n->isLeaf()) {
      const Particle* p = n->particles();
      for (int i = 0; i < n->n_particles; i++) {
        long g = p[i].group_number;
        if (g < mn) mn = g;
        if (g > mx) mx = g;
      }
    } else {
      for (int i = 0; i < n->n_children; i++) {
        auto r = annotateFrozenTips(n->getChild(i));
        if (r.first < mn) mn = r.first;
        if (r.second > mx) mx = r.second;
      }
    }
    n->data.min_frag = mn;
    n->data.max_frag = mx;
    return {mn, mx};
  }

  // (b) Cross-PE edge emission. For each TreePiece pair spanning this PE and a
  // higher PE of the same process, walk the pair over frozen data and emit
  // deduplicated (tip, tip) edges into this PE's buffer; hand the buffer to
  // the nodegroup. No-op when this process has a single PE (non-SMP or
  // one-PE-per-process runs).
  // Sliced drain (campaign step P1, design/phaseab-balancing.md sections
  // 9 and 14): with FOF_PHASEB_SLICE_MS > 0 the claim loop returns false
  // at the deadline and the caller re-enters by SELF-SEND, so the
  // scheduler runs between slices and this PE stays responsive to
  // messages (probe pings, network progress). Nothing is
  // stashed: all intermediates are per-PE members and the claim cursor
  // is process-shared. Default 0 = drain in one call, byte-identical to
  // the pre-campaign behavior.
  static double sliceSeconds() {
    static const double s = [] {
      const char* e = std::getenv("FOF_PHASEB_SLICE_MS");
      return e ? std::atof(e) / 1e3 : 0.0;
    }();
    return s;
  }

  bool phaseBBody(double b2) {
    double t0 = CkWallTimer();
    if (!pb_active_) {   // start of stage (not a slice re-entry)
      pb_active_ = true;
      pb_t_accum_ = 0.0;
      pb_cur_part_ = -1;
      b2_ = b2;
      t_phaseB_maxpair = 0.0;
      t_phaseB_units = 0;
      edge_buf.clear();
      seen.clear();
      cert_tip.clear();
    }
    const double slice = sliceSeconds();
    auto* nb = node_proxy.ckLocalBranch();
    // Claim units from the process-wide pool until it drains (dynamic
    // self-scheduling; supersedes the static symmetric-hash assignment —
    // see the pool comment on FoFPhase1Node). Unit claims (chunk 1):
    // the pool is LPT-sorted, so consecutive units are the costliest —
    // chunked claims would stack them on one PE. Atomic traffic is one
    // fetch_add per unit, a few thousand per PE.
    // Claim granularity is the UNIT everywhere (measured 2026-08-11:
    // exclusive whole-partition claims regressed phaseB 0.031 -> 0.114 at
    // 80M — one PE drained the giant partition alone, un-doing the flat
    // pool's LPT balance). Partitions remain the natural GPU batch unit;
    // intra-process draining uses global cursors over the
    // partition-ordered pool: [0, pb_b2_begin) in the B1 stage,
    // [pb_b2_begin, n) in B2, the whole pool otherwise.
    const int stage = nb->pb_stage; // 0 flat/unit-KD, 1 = B1, 2 = B2
    const size_t CHUNK = 1;
    for (;;) {
      size_t start, end;
      if (stage == 2) {
        size_t u = nb->pb_b2_next.fetch_add(1);
        if (u >= nb->phaseb_pool.size()) break;
        start = u;
        end = u + 1;
      } else {
        size_t limit = stage == 1 ? nb->pb_b2_begin : nb->phaseb_pool.size();
        start = nb->phaseb_next.fetch_add(CHUNK);
        if (start >= limit) break;
        end = std::min(start + CHUNK, limit);
      }
      for (size_t k = start; k < end; k++) {
          t_phaseB_units++;
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
          {
            long us = (long)(tp * 1e6);
            int b = 0;
            while (us > 1 && b < FoFPhase1Node<Data>::kUnitHistBuckets - 1) {
              us >>= 1;
              b++;
            }
            nb->pb_unit_hist[b].fetch_add(1, std::memory_order_relaxed);
          }
      }
      if (slice > 0 && CkWallTimer() - t0 > slice) {
        pb_t_accum_ += CkWallTimer() - t0;
        return false;   // deadline: caller re-enters by self-send
      }
    }
    if (!edge_buf.empty()) nb->submitEdges(std::move(edge_buf));
    edge_buf.clear();
    seen.clear();
    if (stage == 1) {
      // End of B1 only: edges are in; the mid-phase merge and the B2
      // round follow (phaseBChained routes on the stage). Stay active so
      // B2 keeps accumulating into the same t_phaseB.
      pb_t_accum_ += CkWallTimer() - t0;
      return true;
    }
    pb_active_ = false;
    t_phaseB = pb_t_accum_ + (CkWallTimer() - t0); // summed over slices
    return true;
  }

  // (d) Rewrite this PE's labels through the merge map (identity if
  // absent): representative-indirect (design/relabel-representative.md)
  // — the map is probed once per frozen root, then one indexed-load pass
  // writes the result into the particles.
  void relabelBody() {
    auto& tip_map = node_proxy.ckLocalBranch()->tip_map;
    for (int r : roots) {
      auto it = tip_map.find(rep_label[r]);
      if (it != tip_map.end()) rep_label[r] = it->second;
    }
    materializeLabels();
  }

  // One indexed-load pass writing each representative's current label into
  // every particle. No hashing: uf_parent[i] is the root index directly
  // (full path compression at the freeze). Morton-sorted particle blocks
  // mean consecutive particles nearly always share a root, so this touches
  // about one distinct rep_label cache line per fragment.
  void materializeLabels() {
    for (auto& s : treepieces)
      for (int i = 0; i < s.n; i++)
        s.parts[i].group_number = rep_label[uf_parent[s.offset + i]];
  }

#ifdef FOF_GPU
  // --- The device phase-1 chain (design/phase1-gpu.md sections 11-18).
  //
  // Five rounds inside the process, driven by atomic deposit counters on
  // the node branch rather than by reductions — the same shape as the CPU
  // chain above, and for the same reason: no process should hold every
  // other process at a stage boundary.
  //
  //   1. deviceStage0   every PE   reserve a slice of the flat index space
  //   2. deviceInit     one PE     bind the device, size buffers, plan the tree
  //   3. devicePack     every PE   K0 pack + this PE's slice of the flat tree
  //      ...the LAST PE to finish packing enqueues the pass and arms the
  //      completion callback, then RETURNS TO THE SCHEDULER.
  //   4. deviceComplete that PE    the callback: fence, read stats, fan out
  //   5. deviceScatter  every PE   K5: adopt (Replace) or compare (Verify)
  //
  // Stage 4 changed round 3/4. Through stage 3 the device call was
  // synchronous and pinned to the process's HOME PE, so one scheduler
  // thread sat inside an entry method for the whole pass — 445 ms at 2B.
  // Neither constraint is real: HAPI gives every PE of the process the
  // same device (device_count = visible/procs_per_node = 1 here, so
  // pes_per_device = ppn and every PE's my_device is identical), and
  // hapiAddCallback queues the completion event on the CALLING PE, so the
  // launcher is also the natural completion target. The pass is therefore
  // enqueued by whichever PE happens to finish packing last and completed
  // by the same PE, with no scheduler thread blocked in between.
  //
  // A nodegroup entry does NOT run on the home PE (measured,
  // design/phase1-gpu.md section 11.3 trap 2), so the hops are explicit
  // and go through this PE-indexed GROUP proxy.

  // Completion form. SYNCHRONOUS BY DEFAULT, on measurement.
  //
  // Stage 4 built the asynchronous path (enqueue + hapiAddCallback) on
  // the reasoning that only the leaf-count readback forces a host
  // synchronization, so the walk/freeze/download tail could run with the
  // scheduler free. The tail is real -- at 100k the fenced form reports
  // walk 15.3 ms + freeze 15.6 ms and the async form reports the whole
  // thing as already finished by the time the callback is serviced. But
  // measured at 2B on 16 nodes, the enqueue ITSELF does not return until
  // the pass is essentially done: `blocking` is 307.9 ms of a 308.2 ms
  // round trip, and the synchronous arm is if anything slightly faster
  // (5.620 vs 5.724 s per iteration). The async form therefore adds a
  // HAPI event, a callback dispatch and QD bookkeeping to defer 0.3 ms.
  //
  // Why the enqueue blocks is NOT established, and the prediction above
  // being wrong is the reason to keep both paths rather than delete one.
  // The suspects are the per-call Kokkos View allocations in the prepare
  // (hipFree is a synchronizing call) and the HIP launch queue backing up
  // behind a long kernel. Hoisting those allocations to persist across
  // iterations would test it and is worth doing on its own merits; if it
  // makes the enqueue return promptly, FOF_GPU_ASYNC becomes the default.
  //
  // Launching from ANY PE is kept in both forms and is not what this
  // selects: the pass is enqueued by whichever PE packed last either way.
  static bool gpuAsyncLaunch() {
    static const bool on = [] {
      const char* e = std::getenv("FOF_GPU_ASYNC");
      return e && std::atoi(e) != 0;
    }();
    return on;
  }

  // Release the pinned staging once the scatter has read the labels
  // (section 17.3: the memory the device path leaves resident is the
  // surviving candidate for the phase-3 regression).
  //
  // OFF by default, deliberately. Releasing means the next iteration
  // re-allocates ~600 MB of PINNED host memory, and hipHostMalloc at that
  // size is not a cheap call — it is hundreds of milliseconds, paid per
  // iteration, against a hypothesis that is still unmeasured at the scale
  // where it matters (at 80M on one node the A/B moved phase 3 by 12 ms).
  // Making a certain per-iteration cost the default to chase an uncertain
  // one is the wrong way round. FOF_GPU_RELEASE=1 is the other arm.
  static bool gpuReleaseStaging() {
    static const bool on = [] {
      const char* e = std::getenv("FOF_GPU_RELEASE");
      return e && std::atoi(e) != 0;
    }();
    return on;
  }

  // The stage-3 cell-grid occupancy gate, read once. Both the launch and
  // the completion report need it and they are now separate methods, so it
  // cannot stay a local of the launch.
  static double gpuGridThresh() {
    static const double g = [] {
      const char* e = std::getenv("FOF_GPU_GRID");
      return e ? std::atof(e) : 0.0;
    }();
    return g;
  }

  // Round 1 (broadcast, every PE): reserve this PE's slice of the
  // process-flat index space. fetch_add gives disjoint bases with no
  // ordering requirement — the PE that gets base 0 is not special.
  void deviceStage0(double b2, const CkCallback& cb) {
    b2_ = b2;
    dev_done_cb_ = cb;
    dev_t_pack_ = 0;
    dev_t_scatter_ = 0;
    auto* nb = node_proxy.ckLocalBranch();
    dev_n_ = 0;
    // In Replace mode nothing has run before this, so the flat index
    // space this PE's TreePieces occupy has to be laid out here — phaseA
    // is what used to do it, and everything downstream (uf_parent
    // indexing, materializeLabels) reads s.offset.
    for (auto& s : treepieces) {
      s.offset = dev_n_;
      dev_n_ += s.n;
    }
    dev_base_ = nb->dev_total.fetch_add(dev_n_);
    {
      // Published so the home PE can rebase the flat trees without
      // reaching into another PE's group branch.
      std::lock_guard<std::mutex> g(nb->lock);
      nb->dev_pe_base[CkMyPe()] = dev_base_;
    }
    if (nb->dev_count_done.fetch_add(1) + 1 == CkNodeSize(CkMyNode()))
      this->thisProxy[CkNodeFirst(CkMyNode())].deviceInitOnHome();
  }

  // Round 2 (home PE only): bind the device and size the staging buffers.
  void deviceInitOnHome() {
    auto* nb = node_proxy.ckLocalBranch();
    int device_id = -1;
#ifdef FOF_GPU_HAS_HAPI
    hapiCheck(hapiGetDevice(&device_id));
#endif
    if (!nb->dev_inited) {
      nb->device.init(device_id);
      const int procs_per_node = CmiNumNodes() / CmiNumPhysicalNodes();
      // One process per GPU is an invariant, not a preference
      // (design/phase1-gpu.md section 2): sharing a GCD silently halves
      // every later measurement, so refuse rather than run.
      if (!nb->device.checkMapping(procs_per_node))
        CkAbort("FoF GPU: processes would SHARE a GPU (%d processes on "
                "this physical node, only %d devices visible). Launch at "
                "most one process per GCD.",
                procs_per_node, nb->device.info().n_visible);
#ifdef FOF_GPU_HAS_HAPI
      hapiCreateStreams();
      nb->dev_stream = (void*)hapiGetStream();
#endif
      nb->device.setStream(nb->dev_stream);
      nb->dev_inited = true;
    }
    nb->device.resize((size_t)nb->dev_total.load());
    devicePlanTree(nb);
    const int first = CkNodeFirst(CkMyNode());
    for (int pe = first; pe < first + CkNodeSize(CkMyNode()); pe++)
      this->thisProxy[pe].devicePack();
  }

  // Stage 1 (design/phase1-gpu.md): concatenate the process's per-TreePiece
  // flat trees into one array and upload it. The trees are built at TREE
  // BUILD time (src/DeviceTree.h, gated by PARATREET_DEVICE_TREE), so
  // nothing here walks a tree — this is a memcpy per piece plus an index
  // rebase, and it is the only part of the flat tree that phase 1 pays for.
  //
  // Rebasing: each piece's node indices are piece-local, so a piece placed
  // at `node_off` has child_begin shifted by node_off; part_begin stays
  // piece-local and is paired with the piece's particle base, which is
  // what piece_part_base carries. Keeping part_begin piece-local means the
  // flat tree is independent of how the PEs happened to lay the particles
  // out, so it can be built once and reused across phase-1 calls.
  // Stage 1 (design/phase1-gpu.md): plan the concatenated flat tree.
  //
  // The per-TreePiece trees are built at TREE BUILD time
  // (src/DeviceTree.h, gated by PARATREET_DEVICE_TREE), so nothing here
  // walks a tree. What is left is a memcpy per piece plus a rebase of the
  // child links — and at 80M that is 3.2M nodes / 123 MB per process,
  // which measured 236 ms when one PE did it. So the home PE only
  // computes the LAYOUT here; the copying is handed to the PEs alongside
  // the particle pack, each writing its own disjoint slice.
  //
  // pe_treepieces is process-shared and holds the pieces in the same
  // registration order each PE's own `treepieces` does, so the two sides
  // agree on which slice belongs to whom without any extra bookkeeping.
  void devicePlanTree(FoFPhase1Node<Data>* nb) {
    nb->dev_piece_node_off.clear();
    nb->dev_total_nodes = 0;
    nb->dev_tree_pieces = 0;
    std::vector<int> piece_root, piece_base;
    long nodes = 0;
    for (auto& kv : nb->pe_treepieces) {
      auto bit = nb->dev_pe_base.find(kv.first);
      if (bit == nb->dev_pe_base.end()) continue;
      long at = bit->second;  // this PE's base in the process-flat space
      auto& offs = nb->dev_piece_node_off[kv.first];
      offs.reserve(kv.second.size());
      for (auto& s : kv.second) {
        if (s.dnodes != nullptr && s.n_dnodes > 0) {
          offs.push_back((int)nodes);
          piece_root.push_back((int)nodes);
          // part_begin stays PIECE-local in the flat tree, so it is
          // paired with the piece's particle base here. That keeps the
          // tree independent of how the PEs laid the particles out, so it
          // survives being built once and reused.
          piece_base.push_back((int)at);
          nodes += s.n_dnodes;
        } else {
          offs.push_back(-1);
        }
        at += s.n;
      }
    }
    // No flat trees registered (PARATREET_DEVICE_TREE unset?) means no tree
    // on the device. Left to the guard in deviceLaunch, which aborts: a
    // warning is not enough once the device is the only arm.
    if (nodes == 0) return;
    nb->dev_total_nodes = nodes;
    nb->dev_tree_pieces = (long)piece_root.size();
    nb->device.resizeTree(nodes, (int)piece_root.size());
    std::memcpy(nb->device.hostPieceRoot(), piece_root.data(),
                piece_root.size() * sizeof(int));
    std::memcpy(nb->device.hostPieceBase(), piece_base.data(),
                piece_base.size() * sizeof(int));
  }

  // Home PE, after every PE has filled its slice.
  void deviceFinishTree(FoFPhase1Node<Data>* nb) {
    if (nb->dev_total_nodes == 0) return;
    fofgpu::TreeStats ts;
    nb->device.uploadTree(&ts);
    nb->dev_t_tree_upload = ts.t_upload;
    nb->dev_t_tree_check = ts.t_check;
    const long staged = nb->dev_total.load();
    // Both numbers were recomputed BY THE DEVICE from what actually
    // landed in its memory, so they check the copy and the rebase rather
    // than echoing the host.
    if (ts.bad_nodes != 0)
      CkAbort("FoF GPU: %ld structurally invalid nodes after tree upload",
              ts.bad_nodes);
    if (ts.particles_under_roots != staged)
      CkAbort("FoF GPU: flat trees cover %ld particles but %ld were staged",
              ts.particles_under_roots, staged);
  }

  // Round 3 (every PE): the K0 pack. Reads this PE's own Particle blocks
  // (~112 B stride) and writes the 20 B device form into its own slice of
  // the pinned buffer. Owner-writes, disjoint slices, no locking — and
  // parallel across the process's PEs, which is the whole point: the same
  // gather on one thread is ~0.1 s at 2B chare sizes.
  void devicePack() {
    double t0 = CkWallTimer();
    auto* nb = node_proxy.ckLocalBranch();
    float* pos = nb->device.hostPositions();
    long* order = nb->device.hostOrders();
    long at = dev_base_;
    for (auto& s : treepieces) {
      for (int i = 0; i < s.n; i++, at++) {
        pos[3 * at] = (float)s.parts[i].position.x;
        pos[3 * at + 1] = (float)s.parts[i].position.y;
        pos[3 * at + 2] = (float)s.parts[i].position.z;
        order[at] = s.parts[i].order;
      }
    }
    dev_t_pack_ = CkWallTimer() - t0;

    // Same round, same owner-writes shape: this PE's slices of the
    // concatenated flat tree. Rebasing child_begin into the concatenated
    // array is the only transformation; part_begin stays piece-local.
    double t1 = CkWallTimer();
    fofgpu::DDNode* hn = nb->device.hostNodes();
    auto oit = nb->dev_piece_node_off.find(CkMyPe());
    if (hn != nullptr && oit != nb->dev_piece_node_off.end()) {
      size_t k = 0;
      long part_at = dev_base_;
      for (auto& s : treepieces) {
        if (k >= oit->second.size()) break;
        const int off = oit->second[k++];
        if (off >= 0 && s.dnodes != nullptr) {
          std::memcpy(hn + off, s.dnodes,
                      (size_t)s.n_dnodes * sizeof(fofgpu::DDNode));
          // Rebase both index spaces into the process-flat ones: child
          // links into the concatenated node array, and part_begin into
          // the concatenated particle array. The SOURCE tree stays
          // piece-local (so it survives being built once at tree build
          // and reused); only this per-process copy is rebased.
          for (int i = 0; i < s.n_dnodes; i++) {
            if (hn[off + i].child_begin >= 0) hn[off + i].child_begin += off;
            hn[off + i].part_begin += (int)part_at;
          }
        }
        part_at += s.n;
      }
    }
    dev_t_tree_pack_ = CkWallTimer() - t1;

    // Whoever finishes last launches, right here, with no hop to the home
    // PE (stage 4). Every PE of the process is bound to the same device,
    // and hapiAddCallback queues the completion event on the PE that arms
    // it, so the launcher must be a PE that will go on running its
    // scheduler — which is exactly a PE returning from an entry method.
    if (nb->dev_pack_done.fetch_add(1) + 1 == CkNodeSize(CkMyNode()))
      deviceLaunch();
  }

  // Round 4, part 1 (the last PE to finish packing): enqueue the pass and
  // arm its completion. NOT an entry method — it runs at the tail of
  // devicePack on the PE that got there last.
  void deviceLaunch() {
    auto* nb = node_proxy.ckLocalBranch();
    deviceFinishTree(nb);
    // No flat tree means no traversal: runPhase1 early-returns on
    // n_nodes == 0 and hostLabels() then holds whatever was last written
    // there. In Verify mode that surfaces immediately as a wall of
    // mismatches; in Replace mode it would be ADOPTED as the answer.
    // devicePlanTree already prints a warning at this point, and a warning
    // is not enough once the device is the only arm.
    if (nb->dev_total_nodes == 0)
      CkAbort("FoF GPU: no flat device tree on process %d - phase 1 has "
              "nothing to traverse. Set PARATREET_DEVICE_TREE=1; the emit "
              "happens at TREE BUILD, so it cannot be turned on from here.",
              CkMyNode());
    nb->dev_launch_pe = CkMyPe();
    nb->dev_t_launch = CkWallTimer();
    // Stage 2: the real pass — whole-process union-find over the staged
    // particles and the uploaded tree, replacing phaseA + phaseB + merge.
    // Stage 3: the dense-node cell grid, OFF by default on the device
    // even though the CPU walk defaults to -G 4. Measured on lambb.00500
    // (design section 15.3) the grid does exactly what it is supposed to
    // — the walk drops monotonically with the threshold, 263 -> 217 ->
    // 202 -> 171 ms — and still loses, because its own stencil pass costs
    // more than it saves at these densities. It is kept, and kept
    // reachable through FOF_GPU_GRID, because it is the only thing that
    // stops a genuinely dense input (larger b, or a deeper halo catalogue)
    // from going quadratic. Do not read "off" as "not needed".
    const double gth = gpuGridThresh();
#ifdef FOF_GPU_HAS_HAPI
    if (gpuAsyncLaunch()) {
      // Enqueue and RETURN. Everything after the leaf-count readback is in
      // flight on the HAPI stream; this PE goes back to its scheduler and
      // hapiPollEvents — already installed on every PE by hapiInit, so it
      // costs nothing extra — fires deviceComplete when the stream drains.
      nb->device.enqueuePhase1((float)b2_, (float)gth);
      hapiAddCallback((hapiStream_t)nb->dev_stream,
                      CkCallback(CkIndex_FoFPhase1<Data>::deviceComplete(),
                                 this->thisProxy[CkMyPe()]),
                      nullptr);
      // The number stage 4 is judged by: how much of the pass held a
      // scheduler thread. It is the enqueue, not the fence in
      // deviceComplete — by the time that callback is serviced the stream
      // has already drained, so the fence there measures ~0 whether or
      // not the design works. This measures the part that BLOCKS: the
      // synchronous prepare, which the leaf-count readback forces.
      nb->dev_t_block = CkWallTimer() - nb->dev_t_launch;
      return;
    }
#endif
    // Fenced form: every sub-kernel wall is real. deviceComplete then just
    // collects them (completePhase1 is idempotent once the pass has
    // already been waited on).
    nb->device.runPhase1((float)b2_, (float)gth, nullptr);
    deviceComplete();
  }

  // Round 4, part 2: the pass has drained. On the async path this is a
  // Charm callback delivered by hapiPollEvents on the launching PE; on the
  // synchronous path deviceLaunch calls it directly.
  void deviceComplete() {
    auto* nb = node_proxy.ckLocalBranch();
    // Fence + counter readback. On the async path this is ~0 by
    // construction: the callback fires BECAUSE the stream drained, so
    // there is nothing left to wait for. dev_t_block, set at the enqueue,
    // is the number that says whether the design works.
    nb->device.completePhase1(&nb->dev_walk);
    nb->dev_t_launch = CkWallTimer() - nb->dev_t_launch;
    // Synchronous path: the whole pass blocked, by definition.
    if (!gpuAsyncLaunch()) nb->dev_t_block = nb->dev_t_launch;
    const fofgpu::WalkStats& w = nb->dev_walk;
    // A stack overflow means the traversal DROPPED work, which shows up
    // as silently under-merged components. Never a warning.
    if (w.stack_overflows != 0)
      CkAbort("FoF GPU: traversal stack overflowed %ld times - work was "
              "dropped; raise kStack in FoFDevice.cpp",
              w.stack_overflows);
    const int first = CkNodeFirst(CkMyNode());
    for (int pe = first; pe < first + CkNodeSize(CkMyNode()); pe++)
      this->thisProxy[pe].deviceScatter();
  }

  // Round 5 (every PE): take delivery of the device's answer.
  //
  // Replace mode ADOPTS it (deviceAdopt below builds the section-4
  // contract from it). Verify mode COMPARES it against the CPU chain's,
  // PARTICLE BY PARTICLE — the strongest gate available and an exact one,
  // not statistical: the CPU chain has already run (phaseA + phaseB +
  // merge + relabel), so every particle's group_number holds its
  // PROCESS-level tip — the global order of the minimum-order member of
  // its process-level component. The device union-find is process-wide
  // from the start and uses the same union-by-min-order rule, so it must
  // produce THE SAME VALUE for every particle. Not "the same number of
  // components": the same label, everywhere.
  void deviceScatter() {
    auto* nb = node_proxy.ckLocalBranch();
    const long* label = nb->device.hostLabels();
    double t0 = CkWallTimer();
    long bad = 0;
    if (paratreet::fofGpuMode() == paratreet::FoFGpuMode::Replace) {
      deviceAdopt(label);
    } else {
      long at = dev_base_;
      for (auto& s : treepieces)
        for (int i = 0; i < s.n; i++, at++)
          if (label[at] != s.parts[i].group_number) bad++;
    }
    dev_t_scatter_ = CkWallTimer() - t0;
    // `bad` is max-reduced below and aborts the run in runFoFPhase1; it is
    // the only report a mismatch gets.
    // Every PE has now read hostLabels(), so the staging can go. The last
    // depositor does it — on whatever PE that is, which is safe because
    // releaseStaging fences the stream first and nothing else in the
    // process touches the device until the next iteration's round 2.
    if (nb->dev_scatter_done.fetch_add(1) + 1 == CkNodeSize(CkMyNode()) &&
        gpuReleaseStaging())
      nb->device.releaseStaging();
    // max-reduced: pack (per PE), launch->completion (per process),
    // scatter (per PE), failures (summed in as a max so any nonzero
    // survives to the driver), tree pack (per PE), tree upload (per
    // process), and the scheduler-visible GPU wait (per process).
    double vals[7] = {dev_t_pack_,  nb->dev_t_launch,     dev_t_scatter_,
                      (double)bad,  dev_t_tree_pack_,
                      nb->dev_t_tree_upload, nb->dev_t_block};
    this->contribute(7 * sizeof(double), vals, CkReduction::max_double,
                     dev_done_cb_);
  }

  // Build the section-4 contract from the device's labels: uf_parent,
  // roots, rep_label, root_counts, plus every particle's group_number.
  //
  // Section 4 expected this to force a PROCESS-WIDE rep_label, because the
  // device union-find is process-wide and a component's minimum-order root
  // can live in any PE's range — and it warned that the read-after-write
  // hazard in applyTipEncoding/materializeLabels would then need deposit
  // barriers, calling that "a bug being introduced". It does not, and the
  // reason is worth keeping: NOTHING DOWNSTREAM READS ROOT IDENTITY.
  // applyTipEncoding, applyGlobalMap, applySliceOnPE, applyUF2Labels and
  // depositLabelCounts all iterate `roots` and only ever transform
  // rep_label[r] or sum root_counts[r] BY LABEL — depositLabelCounts says
  // so in as many words ("duplicate labels across roots need no
  // pre-merging here"). So the representative structure only has to
  // satisfy rep_label[uf_parent[i]] == label[i]; it does not have to agree
  // with the device's roots, and it can stay strictly per-PE.
  //
  // Which makes the cheapest valid construction a RUN-LENGTH ENCODING:
  // walk this PE's particles in order and open a new representative every
  // time the label changes. One linear pass, no hashing, perfect locality,
  // and it degrades gracefully — in the worst case (every particle a
  // different label) every particle is its own representative, which is
  // still correct, just no cheaper than a per-particle rewrite. Morton
  // order makes that worst case unreachable in practice: materializeLabels
  // already relies on consecutive particles nearly always sharing a root.
  //
  // FOF_COUNT_VERIFY=1 recomputes the counts from the particles and aborts
  // on disagreement, so this construction is gated by machinery that
  // already existed.
  void deviceAdopt(const long* label) {
    int n_local = 0;
    density_x = 0.0;
    size_x = 0.0;
    max_piece_n = 0;
    for (auto& s : treepieces) {
      s.offset = n_local;
      n_local += s.n;
      // Kept identical to phaseABody's: the phase3Stats cost-model probes
      // read these and a device run should still populate them.
      double vol = (double)s.root->data.box.volume();
      if (vol > 0) density_x += (double)s.n * (double)s.n / vol;
      size_x += std::pow((double)s.n, 1.28);
      if ((long)s.n > max_piece_n) max_piece_n = s.n;
    }
    uf_parent.resize(n_local);
    rep_label.assign(n_local, -1);
    root_counts.assign(n_local, 0);
    roots.clear();
    // flat_order stays EMPTY: it exists so find()/unite() can compare
    // global orders, and neither runs on this path (frozen_ blocks them).
    flat_order.clear();
    int run = -1;
    long run_label = 0;
    long at = dev_base_;
    for (auto& s : treepieces) {
      for (int i = 0; i < s.n; i++, at++) {
        const long g = label[at];
        const int k = s.offset + i;
        if (run < 0 || g != run_label) {
          run = k;
          run_label = g;
          roots.push_back(k);
          rep_label[k] = g;
        }
        uf_parent[k] = run;
        root_counts[run]++;
        s.parts[i].group_number = g;
      }
    }
    frozen_ = true;
    // Per-PE load signal for phase3Stats. On the CPU path it is the
    // phaseA wall; here the only per-PE phase-1 work is the staging, so
    // that is what it reports. Same meaning (this PE's share of phase 1),
    // different constituent — do not compare the two across modes.
    t_phaseA = dev_t_pack_ + dev_t_tree_pack_;
    t_phaseB = 0.0;
  }

  long dev_base_ = 0;
  int dev_n_ = 0;
  double dev_t_pack_ = 0;
  double dev_t_tree_pack_ = 0;
  double dev_t_scatter_ = 0;
  CkCallback dev_done_cb_;
#else
  // CPU-only build: fof.ci declares these entries unconditionally (it has
  // no preprocessor), so they need definitions. deviceStage0 closes its
  // callback so a caller that asks for the stage without a GPU build gets
  // an immediate, honest "nothing happened" rather than a hang; the rest
  // are unreachable.
  //
  // A CPU-only binary asked for FOF_GPU_PHASE1 does NOT reach here:
  // runFoFPhase1 aborts at the top instead. Closing the callback with
  // zeros would otherwise mean phase 1 silently produced no labels at all.
  void deviceStage0(double, const CkCallback& cb) {
    double vals[7] = {0, 0, 0, 0, 0, 0, 0};
    this->contribute(7 * sizeof(double), vals, CkReduction::max_double, cb);
  }
  void deviceInitOnHome() {}
  void devicePack() {}
  void deviceComplete() {}
  void deviceScatter() {}
#endif  // FOF_GPU

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
    phaseABody(b2);
    // Skew-split instrument (design/phasea-reassignment.md section 3):
    // per-process sum/max of the phaseA walls, read back by phase3Stats.
    nb->depositPhaseATime(t_phaseA);
    nb->depositPhaseAStages(t_pa_self, t_pa_cross);
    // Per-PE dump (FOF_STAGE_DUMP=1; relay12 item 26): pieces held, self
    // and cross seconds, one line per PE. The input the claim-priority
    // design needs — cross work is convex in pieces-per-PE, so the
    // correlation between piece count and cross time decides whether
    // pricing the marginal cross cost at claim time is worth building.
    static const bool stage_dump = [] {
      const char* e = std::getenv("FOF_STAGE_DUMP");
      return e && std::atoi(e) != 0;
    }();
    if (stage_dump)
      CkPrintf("FOF3STAT stage_pe: pe %d pieces %zu self %.4f cross %.4f\n",
               CkMyPe(), treepieces.size(), t_pa_self, t_pa_cross);
    if (nb->a_done.fetch_add(1) + 1 == CkNodeSize(CkMyNode())) {
      nb->stage_tA = CkWallTimer() - nb->chain_t0;
      if (stealA())
        CkPrintf("FOF3STAT s1_claims: node %d own %ld foreign %ld "
                 "pes_with_foreign %ld of %d\n",
                 CkMyNode(), nb->s1_own.load(), nb->s1_foreign.load(),
                 nb->s1_pes_foreign.load(), CkNodeSize(CkMyNode()));
      // Enumerate the process's phaseB pool before releasing the PEs
      // (parallel across the process's threads (buildPoolSlice); pe_treepieces frozen since registration;
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
      // Hand the enumeration to every thread of this process; the last
      // one to finish assembles the pool and starts phaseB.
      nb->pool_slices.assign(CkNodeSize(CkMyNode()), {});
      nb->slice_done.store(0);
      // PE-SET SPLIT: publish TreePiece index -> set id for this process,
      // HERE, on the single PE that is about to broadcast the slice build.
      // Written once, read-only afterwards by every PE — including by the
      // phase-3 ownership prune, which needs it to tell a cross-set local
      // pair (must be walked) from a same-set one (already merged, prune it).
      if (paratreet::peSetsHere() > 1) {
        auto& tbl = paratreet::pieceSetTable();
        int maxidx = -1;
        for (auto& kv : nb->pe_treepieces)
          for (auto& s : kv.second)
            if (s.root) maxidx = std::max(maxidx, s.root->tp_index);
        tbl.assign(maxidx + 1, (signed char)-1);
        for (auto& kv : nb->pe_treepieces) {
          const int set = paratreet::peSetOfPe(kv.first);
          for (auto& s : kv.second)
            if (s.root && s.root->tp_index >= 0)
              tbl[s.root->tp_index] = (signed char)set;
        }
      }
      int first = CkNodeFirst(CkMyNode());
      for (int pe = first; pe < first + CkNodeSize(CkMyNode()); pe++)
        this->thisProxy[pe].buildPoolSlice();
    }
  }

  // One pool unit enumeration, writing into a caller-owned slice.
  // Extracted from the build lambda so every thread of the process can
  // run it concurrently on its own stripe (see buildPoolSlice).
  void poolPushInto(std::vector<typename FoFPhase1Node<Data>::PoolUnit>& out,
                    Node<Data>* a, Node<Data>* b, int depth = 0) {
      double d2 = paratreet::mindist2(a->data.box, b->data.box, period_);
      if (d2 > b2_) return;
      // Depth 2 is reserved for OVERLAPPING pairs (gap 0): only dense
      // shared boundaries can hide a giant unit, and unconditional
      // grandchild granularity measurably inflated the per-unit fixed
      // costs (phaseB avg 0.014 -> 0.020 on the laptop). Separated
      // pairs stay at depth-1 granularity. Still pure geometry.
      // Split depth for OVERLAPPING pairs (gap zero) is tunable
      // (FOF_POOL_DEPTH, default 2). Measured 2026-08-07 at 80M: from
      // 1 to 4 nodes the whole phaseB stage IS one indivisible unit
      // (stage 0.109/0.134/0.073 s against a largest-unit 0.109/0.134/
      // 0.073 s) — a fragment of the densest region that no number of
      // threads or nodes can divide. Separated pairs still stop at
      // depth 1: they are cheap and splitting them only multiplies
      // per-unit overhead.
      static const int max_depth = [] {
        const char* e = std::getenv("FOF_POOL_DEPTH");
        int v = e ? std::atoi(e) : 2;
        return v >= 1 ? v : 2;
      }();
      // Size-based splitting (FOF_POOL_SPLIT_SIZE=C, default 0 = the
      // depth rule below, unchanged). Measured 2026-08-07 at 80M: the
      // depth knob above is inert — the unit that IS the whole phaseB
      // stage at 1-4 nodes is a SEPARATED pair stopped at depth 1 by
      // the d2 > 0 clause, on the assumption that separated pairs are
      // cheap. Two large boxes a hair apart in the densest region are
      // not cheap. With C > 0 a pair keeps splitting while either box
      // is still large compared with the linking length, whatever its
      // depth or separation, so unit cost is bounded by geometry
      // instead of by an assumption.
      static const double split_size = [] {
        const char* e = std::getenv("FOF_POOL_SPLIT_SIZE");
        return e ? std::atof(e) : 0.0;
      }();
      // The size rule is ADDITIVE: it may only split MORE than the
      // depth rule, never less. Measured 2026-08-07 at 80M with the
      // first (replacing) version, C=12 produced a LARGER largest unit
      // than the baseline (0.115 s against 0.063 s) because a pair
      // whose boxes were already under the threshold stopped
      // immediately, including pairs the depth rule would have split.
      bool depth_stop = depth >= max_depth || (depth == 1 && d2 > 0) ||
                        a->isLeaf() || b->isLeaf();
      bool stop_here = depth_stop;
      if (split_size > 0 && !a->isLeaf() && !b->isLeaf() && depth < 6) {
        const double blen = std::sqrt(b2_);
        bool still_big = boxMeasure(a) > split_size * blen ||
                         boxMeasure(b) > split_size * blen;
        if (still_big) stop_here = false;   // keep splitting big pairs
      }
      if (stop_here) {
        // LPT key, pure geometry (no thresholds): overlapping pairs
        // (gap 0) are the expensive ones, ordered by DESCENDING box
        // overlap volume; separated pairs follow by ascending gap.
        // Ascending sort then claims costliest-first, so the pool's
        // tail is cheap units and the last claim cannot be a giant.
        // m2 = rho_a * rho_b * V_int(a grown by b, b) * V_ball: the
        // expected-pairs estimate the cost probe validated (0.87 alone at
        // 2B against 0.04 for the particle-count product). Computed for
        // every unit (drives the adaptive tail split and the KD
        // partition costs); FOF_PB_M2KEY=1 (default) also makes it the
        // LPT key, -m2 so ascending sort = costliest first.
        double m2v = 0.0;
        {
          double va = (double)a->data.box.volume();
          double vb = (double)b->data.box.volume();
          if (va > 0 && vb > 0) {
            double bb2 = std::sqrt(b2_);
            double vint = 1.0;
            for (int ax = 0; ax < 3 && vint > 0; ax++) {
              double lo = std::max((double)a->data.box.lesser_corner[ax] - bb2,
                                   (double)b->data.box.lesser_corner[ax]);
              double hi = std::min((double)a->data.box.greater_corner[ax] + bb2,
                                   (double)b->data.box.greater_corner[ax]);
              vint = hi > lo ? vint * (hi - lo) : 0.0;
            }
            double vball = 4.18879 * bb2 * bb2 * bb2;
            m2v = ((double)a->data.n_below / va) *
                  ((double)b->data.n_below / vb) * vint * vball;
          }
        }
        static const bool m2key = [] {
          const char* e = std::getenv("FOF_PB_M2KEY");
          return !e || std::atoi(e) != 0;
        }();
        if (m2key && m2v > 0) {
          out.push_back({-m2v, a, b, m2v});
          return;
        }
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
        out.push_back({key, a, b, m2v});
        return;
      }
      for (int ci = 0; ci < a->n_children; ci++) {
        Node<Data>* ca = a->getChild(ci);
        if (ca == nullptr || ca->n_particles == 0) continue;
        for (int cj = 0; cj < b->n_children; cj++) {
          Node<Data>* cb = b->getChild(cj);
          if (cb == nullptr || cb->n_particles == 0) continue;
          poolPushInto(out, ca, cb, depth + 1);
        }
      }
  }
  // One thread's stripe of the pool enumeration. TreePiece pairs are taken
  // from the flattened (thread-pair, TreePiece-pair) space by stride, so
  // every thread walks a comparable share of the geometry without any
  // coordination. Writes only its own slice.
  // ---- PE-SET SPLIT (Kale, 2026-08-16; design in
  // reports/pe-set-split-analysis.md). Treat this process's PEs as s
  // independent sets for phase 1: pairs that cross a set boundary are LEFT
  // OUT of the phaseB pool, and phase 3 discovers them instead.
  //
  // Why that is safe, and it is not an assumption — it is three checked
  // mechanisms:
  //  1. Phase 3's edge predicate is "different tips within b", with NO
  //     ownership test (FoFPhase3.h). Its comment justifies that by the
  //     invariant "different tips within b implies different processes",
  //     which THIS CHANGE FALSIFIES — but the code never tested ownership,
  //     so the conclusion still holds. That comment is updated accordingly.
  //  2. The phase-3 SEEN table is written only when an edge is EMITTED
  //     (FoFPhase1Node::trySeenInsert), so suppression can never swallow a
  //     pair phase 1 declined to do.
  //  3. same_frag pruning fires only when both sides are already in one
  //     fragment, which is exactly when no edge is needed.
  // And the phase-3 walk already visits these pairs today: a baseline 2B run
  // reports same_frag 43353 prunes, and pre-relabel tips are process-local,
  // so those ARE same-process pairs the walk reaches and discards as merged.
  //
  // FOF_PE_SETS=s        number of sets (1 = off, the default)
  // FOF_PE_SETS_NODE=n   apply only on process n (unset = EVERY process)
  // FOF_PE_SETS_MODE=0|1 0 = blocked (SFC-adjacent PEs share a set, so the
  //                      least weight lands in phase 3), 1 = round robin
  //                      (the most). Which is better depends on how much
  //                      cheaper a pair is in phase 3 than in phaseB, which
  //                      is the open question, so both are measured.
  void buildPoolSlice() {
    auto* nb = node_proxy.ckLocalBranch();
    int rank = CkMyRank(), nranks = CkNodeSize(CkMyNode());
    auto& slice = nb->pool_slices[rank];
    const int sets = paratreet::peSetsHere();
    long idx = 0, dropped = 0;
    for (auto ita = nb->pe_treepieces.begin(); ita != nb->pe_treepieces.end();
         ++ita) {
      auto itb = ita;
      for (++itb; itb != nb->pe_treepieces.end(); ++itb) {
        if (sets > 1) {
          const int sa_set = paratreet::peSetOfPe(ita->first);
          const int sb_set = paratreet::peSetOfPe(itb->first);
          if (sa_set >= 0 && sb_set >= 0 && sa_set != sb_set) {
            dropped += (long)ita->second.size() * (long)itb->second.size();
            continue;   // phase 3 will find these
          }
        }
        for (auto& sa : ita->second)
          for (auto& sb : itb->second)
            if ((idx++ % nranks) == rank) poolPushInto(slice, sa.root, sb.root);
      }
    }
    if (sets > 1 && rank == 0)
      CkPrintf("FOF3STAT pe_sets: node %d sets %d mode %d piece_pairs_dropped %ld\n",
               CkMyNode(), sets, paratreet::peSetsMode(), dropped);
    // Sort this thread's own slice (in parallel with the others), so the
    // assembly below never runs a global comparison sort — that sort
    // would otherwise become the new serial bottleneck as unit counts
    // grow (Kale, 2026-08-07).
    std::stable_sort(slice.begin(), slice.end(),
                     [](const typename FoFPhase1Node<Data>::PoolUnit& x,
                        const typename FoFPhase1Node<Data>::PoolUnit& y) {
                       return x.key < y.key;
                     });
    if (nb->slice_done.fetch_add(1) + 1 == nranks) {
      size_t total = 0, longest = 0;
      for (auto& sl : nb->pool_slices) {
        total += sl.size();
        longest = std::max(longest, sl.size());
      }
      nb->phaseb_pool.clear();
      nb->phaseb_pool.reserve(total);
      // Round-robin merge of sorted slices: comparison-free and linear.
      // Claiming is costliest-first, and every slice is ordered that way,
      // so taking one unit from each slice in turn keeps the expensive
      // units at the front where the claim cursor reaches them first.
      // Exact global order is not needed — the pool is a scheduling
      // heuristic, not a result.
      for (size_t i = 0; i < longest; i++)
        for (auto& sl : nb->pool_slices)
          if (i < sl.size()) nb->phaseb_pool.push_back(sl[i]);
      for (auto& sl : nb->pool_slices)
        std::vector<typename FoFPhase1Node<Data>::PoolUnit>().swap(sl);
      // Campaign S2a: adaptive m2 tail split (FOF_PB_SPLIT x mean,
      // default 8; 0 disables). Split only units whose m2 exceeds the
      // threshold — the measured rule (~1% of units hold 48-63% of
      // time); children re-estimated by their own geometry, so the
      // recursion terminates on the estimate itself. The 24x-units
      // geometric rule this replaces split everything; this splits
      // exactly the dense-core tail.
      static const double split_fac = [] {
        const char* e = std::getenv("FOF_PB_SPLIT");
        return e ? std::atof(e) : 8.0;
      }();
      if (split_fac > 0 && !nb->phaseb_pool.empty()) {
        double mean = 0;
        for (auto& u : nb->phaseb_pool) mean += u.m2;
        mean /= nb->phaseb_pool.size();
        double thresh = split_fac * mean;
        if (thresh > 0) {
          std::vector<typename FoFPhase1Node<Data>::PoolUnit> out;
          out.reserve(nb->phaseb_pool.size());
          // Iterative worklist; split the larger side, gate children on
          // mindist; cap the extra depth so a pathological estimate
          // cannot run away.
          std::vector<std::pair<typename FoFPhase1Node<Data>::PoolUnit, int>>
              work;
          for (auto& u : nb->phaseb_pool) work.push_back({u, 0});
          while (!work.empty()) {
            auto pr = work.back();
            work.pop_back();
            auto& u = pr.first;
            bool splittable = pr.second < 4 && u.m2 > thresh &&
                              !(u.a->isLeaf() && u.b->isLeaf());
            if (!splittable) {
              out.push_back(u);
              continue;
            }
            Node<Data>* open_n = (!u.a->isLeaf() &&
                                  (u.b->isLeaf() ||
                                   boxMeasure(u.a) >= boxMeasure(u.b)))
                                     ? u.a
                                     : u.b;
            Node<Data>* other = open_n == u.a ? u.b : u.a;
            for (int ci = 0; ci < open_n->n_children; ci++) {
              Node<Data>* c = open_n->getChild(ci);
              if (c == nullptr || c->n_particles == 0) continue;
              if (paratreet::mindist2(c->data.box, other->data.box, period_) >
                  b2_)
                continue;
              typename FoFPhase1Node<Data>::PoolUnit cu;
              cu.a = c;
              cu.b = other;
              // Re-estimate the child pair by its own geometry.
              double m2v = 0.0;
              double va = (double)c->data.box.volume();
              double vb = (double)other->data.box.volume();
              if (va > 0 && vb > 0) {
                double bb2 = std::sqrt(b2_);
                double vint = 1.0;
                for (int ax = 0; ax < 3 && vint > 0; ax++) {
                  double lo =
                      std::max((double)c->data.box.lesser_corner[ax] - bb2,
                               (double)other->data.box.lesser_corner[ax]);
                  double hi =
                      std::min((double)c->data.box.greater_corner[ax] + bb2,
                               (double)other->data.box.greater_corner[ax]);
                  vint = hi > lo ? vint * (hi - lo) : 0.0;
                }
                m2v = ((double)c->data.n_below / va) *
                      ((double)other->data.n_below / vb) * vint *
                      (4.18879 * bb2 * bb2 * bb2);
              }
              cu.m2 = m2v;
              cu.key = m2v > 0 ? -m2v : 0.0;
              work.push_back({cu, pr.second + 1});
            }
          }
          nb->phaseb_pool.swap(out);
        }
      }
      // Campaign S2: KD partitioning (FOF_PB_PARTS, default 0 = off =
      // today's flat pool). m2-weighted median splits on unit centroids
      // (spatially coherent AND cost-balanced); the pool is reordered
      // partition-contiguous, each partition LPT-sorted internally, with
      // a cost, a claim flag, and a unit cursor.
      static const int kparts = [] {
        const char* e = std::getenv("FOF_PB_PARTS");
        return e ? std::atoi(e) : 0;
      }();
      static const bool pbmerge = [] {
        const char* e = std::getenv("FOF_PB_MERGE");
        return e && std::atoi(e) != 0;
      }();
      if (kparts > 1 && pbmerge && !nb->phaseb_pool.empty()) {
        // S2b region mode: KD over the PIECES (realized assignment,
        // n_below-weighted), then classify units by their two endpoints'
        // regions. Intra-region units become the region's B1 partition;
        // cross-region units form the flat B2 tail.
        auto& pool = nb->phaseb_pool;
        size_t n = pool.size();
        int depth = 0;
        while ((1 << depth) < kparts) depth++;
        int nleaf = 1 << depth;
        // Piece list from the realized registry.
        std::vector<double> px, py, pz, pw;
        for (auto& kv : nb->pe_treepieces)
          for (auto& sp : kv.second) {
            px.push_back(0.5 * ((double)sp.root->data.box.lesser_corner.x +
                                (double)sp.root->data.box.greater_corner.x));
            py.push_back(0.5 * ((double)sp.root->data.box.lesser_corner.y +
                                (double)sp.root->data.box.greater_corner.y));
            pz.push_back(0.5 * ((double)sp.root->data.box.lesser_corner.z +
                                (double)sp.root->data.box.greater_corner.z));
            pw.push_back((double)sp.root->data.n_below);
          }
        size_t np = px.size();
        const double* pc[3] = {px.data(), py.data(), pz.data()};
        struct Plane { int axis; double at; };
        std::vector<Plane> planes(2 * nleaf, {0, 0.0});
        std::vector<int> pidx(np);
        for (size_t i = 0; i < np; i++) pidx[i] = (int)i;
        struct Span { int lo, hi, d, node; };
        std::vector<Span> stack{{0, (int)np, depth, 0}};
        while (!stack.empty()) {
          Span sp = stack.back();
          stack.pop_back();
          if (sp.d == 0 || sp.hi - sp.lo <= 1) continue;
          double mn[3] = {1e300, 1e300, 1e300},
                 mx[3] = {-1e300, -1e300, -1e300};
          for (int i = sp.lo; i < sp.hi; i++)
            for (int ax = 0; ax < 3; ax++) {
              double c = pc[ax][pidx[i]];
              if (c < mn[ax]) mn[ax] = c;
              if (c > mx[ax]) mx[ax] = c;
            }
          int axis = 0;
          for (int ax = 1; ax < 3; ax++)
            if (mx[ax] - mn[ax] > mx[axis] - mn[axis]) axis = ax;
          std::sort(pidx.begin() + sp.lo, pidx.begin() + sp.hi,
                    [&](int x, int y) { return pc[axis][x] < pc[axis][y]; });
          double total = 0;
          for (int i = sp.lo; i < sp.hi; i++) total += pw[pidx[i]];
          double acc = 0;
          int split = sp.lo + 1;
          for (int i = sp.lo; i < sp.hi - 1; i++) {
            acc += pw[pidx[i]];
            if (acc >= total / 2) { split = i + 1; break; }
          }
          planes[sp.node] = {axis, pc[axis][pidx[split - 1]]};
          stack.push_back({sp.lo, split, sp.d - 1, 2 * sp.node + 1});
          stack.push_back({split, sp.hi, sp.d - 1, 2 * sp.node + 2});
        }
        auto locate = [&](Node<Data>* nd) {
          double c[3];
          c[0] = 0.5 * ((double)nd->data.box.lesser_corner.x +
                        (double)nd->data.box.greater_corner.x);
          c[1] = 0.5 * ((double)nd->data.box.lesser_corner.y +
                        (double)nd->data.box.greater_corner.y);
          c[2] = 0.5 * ((double)nd->data.box.lesser_corner.z +
                        (double)nd->data.box.greater_corner.z);
          int node = 0, leaf = 0;
          for (int d = depth; d > 0; d--) {
            bool right = c[planes[node].axis] > planes[node].at;
            if (right) leaf += (1 << (d - 1));
            node = 2 * node + (right ? 2 : 1);
          }
          return leaf;
        };
        // Classify; reorder [B1 by region | B2 tail].
        std::vector<int> region_of(n);
        std::vector<char> is_b1(n);
        for (size_t i = 0; i < n; i++) {
          int ra = locate(pool[i].a), rb = locate(pool[i].b);
          is_b1[i] = (ra == rb);
          region_of[i] = ra;
        }
        std::vector<typename FoFPhase1Node<Data>::PoolUnit> reordered;
        reordered.reserve(n);
        nb->pb_part_range.assign(nleaf, {0, 0});
        nb->pb_part_cost.assign(nleaf, 0.0);
        for (size_t i = 0; i < n; i++)
          if (is_b1[i]) nb->pb_part_cost[region_of[i]] += pool[i].m2;
        // Concatenate partitions COSTLIEST-FIRST so the global unit
        // cursor preserves the flat pool's LPT property across
        // partitions (index-order concatenation measured +60% phaseB:
        // the giant's units were claimed last).
        std::vector<int> order(nleaf);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int x, int y) {
          return nb->pb_part_cost[x] > nb->pb_part_cost[y];
        });
        for (int pnum : order) {
          uint32_t begin = (uint32_t)reordered.size();
          for (size_t i = 0; i < n; i++)
            if (is_b1[i] && region_of[i] == pnum) reordered.push_back(pool[i]);
          uint32_t end = (uint32_t)reordered.size();
          std::sort(reordered.begin() + begin, reordered.begin() + end,
                    [](const typename FoFPhase1Node<Data>::PoolUnit& x,
                       const typename FoFPhase1Node<Data>::PoolUnit& y) {
                      return x.key < y.key;
                    });
          nb->pb_part_range[pnum] = {begin, end};
        }
        nb->pb_b2_begin = reordered.size();
        double b2cost = 0;
        for (size_t i = 0; i < n; i++)
          if (!is_b1[i]) {
            reordered.push_back(pool[i]);
            b2cost += pool[i].m2;
          }
        std::sort(reordered.begin() + nb->pb_b2_begin, reordered.end(),
                  [](const typename FoFPhase1Node<Data>::PoolUnit& x,
                     const typename FoFPhase1Node<Data>::PoolUnit& y) {
                    return x.key < y.key;
                  });
        pool.swap(reordered);
        nb->pb_part_claimed.reset(new std::atomic<int>[nleaf]);
        nb->pb_part_next.reset(new std::atomic<uint32_t>[nleaf]);
        for (int pnum = 0; pnum < nleaf; pnum++) {
          nb->pb_part_claimed[pnum] = 0;
          nb->pb_part_next[pnum] = nb->pb_part_range[pnum].first;
        }
        nb->pb_b2_next = nb->pb_b2_begin;
        nb->pb_stage = 1;
        double m2tot = b2cost;
        for (int pnum = 0; pnum < nleaf; pnum++) m2tot += nb->pb_part_cost[pnum];
        CkPrintf("FOF3STAT pb_regions: node %d k %d b1_units %zu b2_units %zu "
                 "b2_m2 %.1f%%\n",
                 CkMyNode(), nleaf, nb->pb_b2_begin, n - nb->pb_b2_begin,
                 m2tot > 0 ? 100.0 * b2cost / m2tot : 0.0);
      } else if (kparts > 1 && !nb->phaseb_pool.empty()) {
        auto& pool = nb->phaseb_pool;
        size_t n = pool.size();
        int depth = 0;
        while ((1 << depth) < kparts) depth++;
        int nleaf = 1 << depth;
        std::vector<double> cx(n), cy(n), cz(n);
        for (size_t i = 0; i < n; i++) {
          cx[i] = 0.25 * ((double)pool[i].a->data.box.lesser_corner.x +
                          (double)pool[i].a->data.box.greater_corner.x +
                          (double)pool[i].b->data.box.lesser_corner.x +
                          (double)pool[i].b->data.box.greater_corner.x);
          cy[i] = 0.25 * ((double)pool[i].a->data.box.lesser_corner.y +
                          (double)pool[i].a->data.box.greater_corner.y +
                          (double)pool[i].b->data.box.lesser_corner.y +
                          (double)pool[i].b->data.box.greater_corner.y);
          cz[i] = 0.25 * ((double)pool[i].a->data.box.lesser_corner.z +
                          (double)pool[i].a->data.box.greater_corner.z +
                          (double)pool[i].b->data.box.lesser_corner.z +
                          (double)pool[i].b->data.box.greater_corner.z);
        }
        const double* coords[3] = {cx.data(), cy.data(), cz.data()};
        std::vector<int> idx(n), leaf_of(n, 0);
        for (size_t i = 0; i < n; i++) idx[i] = (int)i;
        // Weighted median KD split, iterative over (lo, hi, depth, base).
        struct Span { int lo, hi, d, base; };
        std::vector<Span> stack{{0, (int)n, depth, 0}};
        while (!stack.empty()) {
          Span sp = stack.back();
          stack.pop_back();
          if (sp.d == 0 || sp.hi - sp.lo <= 1) {
            for (int i = sp.lo; i < sp.hi; i++) leaf_of[idx[i]] = sp.base;
            continue;
          }
          double mn[3] = {1e300, 1e300, 1e300},
                 mx[3] = {-1e300, -1e300, -1e300};
          for (int i = sp.lo; i < sp.hi; i++)
            for (int ax = 0; ax < 3; ax++) {
              double c = coords[ax][idx[i]];
              if (c < mn[ax]) mn[ax] = c;
              if (c > mx[ax]) mx[ax] = c;
            }
          int axis = 0;
          for (int ax = 1; ax < 3; ax++)
            if (mx[ax] - mn[ax] > mx[axis] - mn[axis]) axis = ax;
          std::sort(idx.begin() + sp.lo, idx.begin() + sp.hi,
                    [&](int x, int y) { return coords[axis][x] < coords[axis][y]; });
          double total = 0;
          for (int i = sp.lo; i < sp.hi; i++) total += pool[idx[i]].m2;
          double acc = 0;
          int split = sp.lo + 1;
          for (int i = sp.lo; i < sp.hi - 1; i++) {
            acc += pool[idx[i]].m2;
            if (acc >= total / 2) { split = i + 1; break; }
          }
          stack.push_back({sp.lo, split, sp.d - 1, sp.base});
          stack.push_back({split, sp.hi, sp.d - 1,
                           sp.base + (1 << (sp.d - 1))});
        }
        // Reorder partition-contiguous; LPT order within each partition.
        std::vector<typename FoFPhase1Node<Data>::PoolUnit> reordered;
        reordered.reserve(n);
        nb->pb_part_range.assign(nleaf, {0, 0});
        nb->pb_part_cost.assign(nleaf, 0.0);
        for (size_t i = 0; i < n; i++) nb->pb_part_cost[leaf_of[i]] += pool[i].m2;
        // Costliest-first concatenation (see the region-mode comment).
        std::vector<int> order(nleaf);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int x, int y) {
          return nb->pb_part_cost[x] > nb->pb_part_cost[y];
        });
        for (int pnum : order) {
          uint32_t begin = (uint32_t)reordered.size();
          for (size_t i = 0; i < n; i++)
            if (leaf_of[i] == pnum) reordered.push_back(pool[i]);
          uint32_t end = (uint32_t)reordered.size();
          std::sort(reordered.begin() + begin, reordered.begin() + end,
                    [](const typename FoFPhase1Node<Data>::PoolUnit& x,
                       const typename FoFPhase1Node<Data>::PoolUnit& y) {
                      return x.key < y.key;
                    });
          nb->pb_part_range[pnum] = {begin, end};
        }
        pool.swap(reordered);
        nb->pb_part_claimed.reset(new std::atomic<int>[nleaf]);
        nb->pb_part_next.reset(new std::atomic<uint32_t>[nleaf]);
        for (int pnum = 0; pnum < nleaf; pnum++) {
          nb->pb_part_claimed[pnum] = 0;
          nb->pb_part_next[pnum] = nb->pb_part_range[pnum].first;
        }
      }
      int first = CkNodeFirst(CkMyNode());
      for (int pe = first; pe < first + CkNodeSize(CkMyNode()); pe++)
        this->thisProxy[pe].phaseBChained();
    }
  }

  void phaseBChained() {
    auto* nb = node_proxy.ckLocalBranch();
    if (!phaseBBody(b2_)) { // slice deadline: yield, re-enter by self-send
      this->thisProxy[CkMyPe()].phaseBChained();
      return;
    }
    // S2b B1 completion: last depositor runs the mid-phase union-find
    // over the B1 edges (RETAINED afterwards — the final merge needs
    // them for collapsed-name consistency) and fans out the
    // relabel+re-annotate stage; B2 starts when that stage's last
    // depositor flips pb_stage and re-broadcasts phaseBChained.
    if (nb->pb_stage == 1) {
      if (nb->b1_done.fetch_add(1) + 1 == CkNodeSize(CkMyNode())) {
        double tm0 = CkWallTimer();
        nb->mergeBody();
        nb->stage_tM1 = CkWallTimer() - tm0;
        CkPrintf("FOF3STAT pb_merge: node %d t %.3f map %zu\n", CkMyNode(),
                 nb->stage_tM1, nb->tip_map.size());
        int first = CkNodeFirst(CkMyNode());
        for (int pe = first; pe < first + CkNodeSize(CkMyNode()); pe++)
          this->thisProxy[pe].phaseBMidRelabel();
      }
      return;
    }
    if (nb->b_done.fetch_add(1) + 1 == CkNodeSize(CkMyNode())) {
      nb->stage_tB = CkWallTimer() - nb->chain_t0 - nb->stage_tA;
      double tm0 = CkWallTimer();
      nb->mergeBody();
      nb->stage_tM = CkWallTimer() - tm0;
      nb->printUnitHist();
      int first = CkNodeFirst(CkMyNode());
      for (int pe = first; pe < first + CkNodeSize(CkMyNode()); pe++)
        this->thisProxy[pe].relabelChained(nb->stage_tA, nb->stage_tB,
                                           nb->stage_tM);
    }
  }


  // S2b mid-phase stage, once per PE: apply the B1 merge map at
  // representative granularity, materialize, re-annotate the frozen-tip
  // node fields (the three-part atomicity rule), clear the tip-keyed
  // per-PE state that the relabel staled (SEEN keys and cert_tip memos
  // name old tips; clearing only re-emits idempotent duplicates), and
  // barrier into B2.
  void phaseBMidRelabel() {
    relabelBody();
    for (auto& sp : treepieces) annotateFrozenTips(sp.root);
    seen.clear();
    cert_tip.clear();
    auto* nb = node_proxy.ckLocalBranch();
    if (nb->mid_done.fetch_add(1) + 1 == CkNodeSize(CkMyNode())) {
      nb->pb_stage = 2;
      int first = CkNodeFirst(CkMyNode());
      for (int pe = first; pe < first + CkNodeSize(CkMyNode()); pe++)
        this->thisProxy[pe].phaseBChained();
    }
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
    for (auto& s : treepieces)
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
    // Representative-indirect: encode each root's label once, then
    // materialize. Duplicate labels across roots (post-merge) encode to
    // the same value, exactly as the per-particle rewrite did.
    for (int r : roots) {
      long tip = rep_label[r];
      CkEnforce(tip >= 0 && (uint64_t)tip <= paratreet::kUF2IdxMask);
      rep_label[r] = (long)paratreet::uf2EncodeTip(my_node, tip);
    }
    materializeLabels();
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
    for (auto& s : treepieces) {
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
    // Representative-indirect: probe the node-shared touched-label map
    // once per frozen root, then materialize.
    for (int r : roots) {
      uint64_t local_id = (uint64_t)rep_label[r] & paratreet::kUF2IdxMask;
      auto it = labels.find(local_id);
      if (it != labels.end()) {
        CkEnforce(it->second != -1);
        rep_label[r] = -(it->second + 2);
      } // else: untouched fragment keeps its encoded tip
    }
    materializeLabels();
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
  // TreePieces of n^2/V (pair count within a chare ~ n * local density).
  // Correlated against t_phaseA across PEs by the stats reduction — the
  // density-drives-phase1-work quantifier (Kale, 2026-07-29). size_x and
  // max_piece_n are the SIZE predictor companions (sum n^1.28 and the
  // largest piece, design/phasea-reassignment.md section 3).
  double density_x = 0.0;
  double size_x = 0.0;
  long max_piece_n = 0;

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
    // Piece-load model validation (design/piece-load-model.md): predicted
    // vs ACTUAL, per process, in one line. This is the gate on whether a
    // (n, volume) model is good enough to migrate pieces by, and it costs
    // one print per process — no algorithm change, so it can ride any run.
    if (CkMyRank() == 0) node_proxy.ckLocalBranch()->printLoadModel(b2_);
    if (CkMyRank() == 0) node_proxy.ckLocalBranch()->printPhaseAStages();
    if (CkMyRank() == 0) paratreet::fofKeepAliveGapsPrint();
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
    // Skew-split instrument (design/phasea-reassignment.md section 3;
    // INDEPENDENT of the union-find mode): every PE of a process reads the
    // same deposited sum/max, so max over PEs == max over processes (the
    // memoryStats trick). proc_skew[0] = this process's within skew
    // (max/avg of its PEs' phaseA walls); [1] = this process's avg (its
    // spread across processes is the cross-process factor); [2] = largest
    // piece (the floor of any piece-granularity rebalancing).
    auto* nbs = node_proxy.ckLocalBranch();
    double proc_avg = nbs->a_time_sum / (double)CkNodeSize(CkMyNode());
    double proc_skew[3] = {proc_avg > 0 ? nbs->a_time_max / proc_avg : 0.0,
                           proc_avg, (double)max_piece_n};
    // Size-predictor correlation terms (x = size_x, y = t_phaseA; the
    // sums of y and y^2 already travel in times/corr).
    double size_corr[3] = {size_x, size_x * t_phaseA, size_x * size_x};
    CkReduction::tupleElement tupleRedn[] = {
      CkReduction::tupleElement(sizeof(sums), sums, CkReduction::sum_long),
      CkReduction::tupleElement(sizeof(long), &peak, CkReduction::max_long),
      CkReduction::tupleElement(sizeof(per_pe), per_pe, CkReduction::min_long),
      CkReduction::tupleElement(sizeof(per_pe), per_pe, CkReduction::max_long),
      CkReduction::tupleElement(sizeof(times), times, CkReduction::sum_double),
      CkReduction::tupleElement(sizeof(times), times, CkReduction::min_double),
      CkReduction::tupleElement(sizeof(times), times, CkReduction::max_double),
      CkReduction::tupleElement(sizeof(corr), corr, CkReduction::sum_double),
      CkReduction::tupleElement(sizeof(proc_skew), proc_skew,
                                CkReduction::max_double),
      CkReduction::tupleElement(sizeof(size_corr), size_corr,
                                CkReduction::sum_double)
    };
    CkReductionMsg* msg = CkReductionMsg::buildFromTuple(tupleRedn, 10);
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
    // Same barrier, same reason: this PE's phaseB wall is final here (the
    // walk's QD is behind us), and it must be deposited before phase3Stats
    // prints the per-process load-model line.
    node_proxy.ckLocalBranch()->depositPhaseBTime(t_phaseB);
    this->contribute(cb);
  }

  // Distributed tip-sentinel check: every registered (TreePiece-owned)
  // particle must hold a valid tip, i.e. a global particle order in
  // [0, n_total). Phase 1 writes every registered particle, so an
  // out-of-range value means some copy was never touched. Runs on each PE
  // over its own particles (no gather), so it stays affordable at any N —
  // the fof3 harness runs it in both check modes.
  void verifyTips(long n_total, const CkCallback& cb) {
    for (auto& s : treepieces) {
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
    // The counts were maintained through every label rewrite since the
    // phaseA freeze (Kale's design, 2026-08-06) — no particle pass here.
    // FOF_COUNT_VERIFY=1 recomputes from the particles and compares.
    static const bool count_verify = [] {
      const char* e = std::getenv("FOF_COUNT_VERIFY");
      return e && std::atoi(e) != 0;
    }();
    if (count_verify) {
      std::unordered_map<long, long> check, maintained;
      for (auto& s : treepieces)
        for (int i = 0; i < s.n; i++) check[s.parts[i].group_number]++;
      for (int r : roots) maintained[rep_label[r]] += root_counts[r];
      if (check != maintained) {
        CkPrintf("COUNT VERIFY MISMATCH pe %d: maintained %zu labels, "
                 "recount %zu labels\n", CkMyPe(), maintained.size(),
                 check.size());
        CkAbort("freeze-time component counts diverge from particle labels");
      }
    }
    // Per-root pairs, labels current in rep_label; histogramShard sums by
    // label, so duplicate labels across roots need no pre-merging here.
    std::vector<std::vector<std::pair<long, long>>> buckets(n_shards);
    for (int r : roots)
      buckets[labelShardMix(rep_label[r]) % n_shards].emplace_back(
          rep_label[r], root_counts[r]);
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
  // registered TreePiece-owned particle blocks; with matching decompositions
  // the Partition target leaves alias these blocks, so they see the global
  // labels too.
  void applyGlobalMap(const std::vector<std::pair<long, long>>& map_vec,
                      const CkCallback& cb) {
    // Representative-indirect: probe once per frozen root, then
    // materialize. (Stage 3 of design/relabel-representative.md will
    // replace the full-map broadcast feeding this with per-process
    // slices; the application below is already slice-ready.)
    std::unordered_map<long, long> tip_map(map_vec.begin(), map_vec.end());
    for (int r : roots) {
      auto it = tip_map.find(rep_label[r]);
      if (it != tip_map.end()) rep_label[r] = it->second;
    }
    materializeLabels();
    this->contribute(cb);
  }

  // Stage-3 sliced counterpart of applyGlobalMap: the map arrived as this
  // process's owner-sharded slice, stored once on the node branch
  // (FoFPhase1Node::applyGlobalMapSlice); probe it read-only per frozen
  // root, materialize, and close the global reduction. Every PE of every
  // process contributes exactly once (processor 0 sends a slice — possibly
  // empty — to every process).
  void applySliceOnPE(const CkCallback& cb) {
    auto& slice = node_proxy.ckLocalBranch()->global_slice;
    if (!slice.empty()) {
      for (int r : roots) {
        auto it = slice.find(rep_label[r]);
        if (it != slice.end()) rep_label[r] = it->second;
      }
      materializeLabels();
    }
    this->contribute(cb);
  }

  // Validation/debugging helper: concat-reduce (position, tip, order) for
  // every particle registered on this PE.
  void collect(const CkCallback& cb) {
    std::vector<paratreet::FoFParticleRecord> recs;
    for (auto& s : treepieces) {
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
    // SELF pair of an internal node: enumerate unordered child pairs
    // directly (i <= j), once each. The one-side-at-a-time split below
    // would otherwise produce BOTH orderings of every distinct child
    // pair ((c_i, R) opens R into all (c_i, c_j), and (c_j, R) into all
    // (c_j, c_i)), recursively at every level of the self-descent —
    // verified 2026-08-11 against the ArborX pair-traversal comparison
    // (design/optimization-inventory.md gap 1). Suppression prunes a
    // mirror only after its first ordering MERGED the pair; unmerged
    // pairs redid their whole verification descent. Distinct unordered
    // pairs descend only to distinct unordered pairs, so this guard at
    // the self nodes makes the entire self-descent mirror-free.
    if (a == b) {
      // Per-level grid gate (2026-08-11, design/optimization-inventory.md
      // bottom section; phaseA only — grid_ctx_ is set only around phaseA
      // self-pair walks): a dense INTERNAL node's self pair is solved by
      // the cell grid instead of descending. Computable at every level
      // only since FragData::n_below exists; the root-level gate in
      // phaseABody remains the first chance, this catches dense cores
      // inside mixed pieces whose root-average diluted that gate.
      // FOF_GRID_ROOT_ONLY=1 restores the old behavior (the A/B arm).
      if (grid_ctx_ != nullptr && grid_thresh_ > 0 && !gridRootOnly() &&
          a->data.n_below >= 64) {
        const double vol = (double)a->data.box.volume();
        const double bb = std::sqrt(b2_);
        const double cc = bb / std::sqrt(6.0);
        if (vol > 0 &&
            (double)a->data.n_below * cc * cc * cc / vol >= grid_thresh_) {
          const TreePieceRef& s = *grid_ctx_;
          int flat = firstFlat(a, s);
          if (gridSelfUnionRange(a->data.box, s.parts + (flat - s.offset),
                                 (int)a->data.n_below, flat))
            return;
        }
      }
      for (int i = 0; i < a->n_children; i++)
        for (int j = i; j < a->n_children; j++)
          walk(a->getChild(i), a->getChild(j), leaf_fn, cert_fn, prune_fn);
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
  bool gridSelfUnion(const TreePieceRef& s) {
    return gridSelfUnionRange(s.root->data.box, s.parts, s.n, s.offset);
  }

  // Range form: the same cell-grid solver over any CONTIGUOUS particle
  // range (a subtree of a key-sorted piece is contiguous by
  // construction). base = the range.s first flat union-find index.
  // Used at the piece root (above) and, since the per-level gate
  // (2026-08-11, design/optimization-inventory.md bottom section), at
  // any internal self node whose occupancy passes -G.
  bool gridSelfUnionRange(const OrientedBox<Real>& box, const Particle* parts,
                          int n, int base) {
    const double b = std::sqrt(b2_);
    const double c = b / std::sqrt(6.0);
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
    std::vector<std::pair<uint64_t, int>> cells(n);
    for (int i = 0; i < n; i++) {
      int64_t ix = (int64_t)(((double)parts[i].position.x - ox) / c);
      int64_t iy = (int64_t)(((double)parts[i].position.y - oy) / c);
      int64_t iz = (int64_t)(((double)parts[i].position.z - oz) / c);
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
      int rep = base + cells[k].second;
      while (e < (int)cells.size() && cells[e].first == cells[k].first) {
        unite(rep, base + cells[e].second);
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
          const Particle& pa = parts[cells[a].second];
          for (int q = nb->begin; q < nb->end; q++) {
            const Particle& pb = parts[cells[q].second];
            if (paratreet::periodicDistSq(pa.position, pb.position, period_) <=
                b2_) {
              unite(base + cells[a].second, base + cells[q].second);
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
  int firstFlat(Node<Data>* n, const TreePieceRef& s) {
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
  void uniteSubtree(Node<Data>* n, const TreePieceRef& s, int rep) {
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
  int certRep(Node<Data>* n, const TreePieceRef& s) {
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
  int connectedRep(Node<Data>* n, const TreePieceRef& s) {
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
    // One fragment under this node: the star through the representative
    // has no edges to add, so neither the subtree walk nor a memo entry
    // is needed. This is the case the memo was paying for.
    if (tipAnnotate() && n->data.uniform()) return n->data.min_frag;
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
                     const TreePieceRef& sa, const TreePieceRef& sb) {
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
    // When each leaf holds a single fragment, every particle pair in this
    // visit can only ever produce the SAME edge: if the two fragments are
    // already one, there is no edge to find at all; otherwise the first
    // pair within range is the only one worth finding. Without this the
    // loop emits that one edge once per particle pair and the seen-set
    // throws all but the first away - measured at 2B, 7.8 billion
    // emissions to keep 1.1 million edges.
    const bool both_uniform =
        tipAnnotate() && a->data.uniform() && b->data.uniform();
    if (both_uniform && a->data.min_frag == b->data.min_frag) return;
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
        if (both_uniform) return;   // one edge is all this pair can yield
      }
    }
  }


 private:
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
    // Post-freeze unions would invalidate the representative structure
    // (rep_label/roots) and the full path compression materializeLabels
    // relies on.
    CkEnforce(!frozen_);
    int rx = find(x), ry = find(y);
    if (rx == ry) return;
    // Union by min: the root with the smaller global particle order wins.
    if (flat_order[rx] < flat_order[ry]) uf_parent[ry] = rx;
    else                                 uf_parent[rx] = ry;
  }

  CProxy_FoFPhase1Node<Data> node_proxy;
  std::vector<TreePieceRef> treepieces;
  // uf_parent stays int: PE-LOCAL flat indices (a single PE never holds
  // 2^31 particles). flat_order holds GLOBAL orders: 64-bit.
  std::vector<int> uf_parent;  // per-PE UF over the flat index space
  std::vector<long> flat_order; // flat index -> global particle order
  std::vector<std::pair<long, long>> edge_buf;
  std::unordered_set<paratreet::TipPairKey, paratreet::TipPairKeyHash> seen;
  // Representative-indirect relabeling (design/relabel-representative.md):
  // after the phaseA freeze a particle never changes fragment, so every
  // later label map is applied to rep_label at ROOT granularity
  // (thousands of entries) and materialized into the particles by one
  // indexed load through uf_parent (root-direct after the freeze — the
  // freeze runs find() on every index, so full path compression holds).
  // root_counts rides the freeze (item-10 component counting) and is
  // deposited as (rep_label[r], root_counts[r]) pairs; the per-label
  // hash map it used to feed (tip_counts) and the rekey pass are gone.
  std::vector<int> roots;        // compact list of frozen root indices
  std::vector<long> rep_label;   // flat index -> current label (root slots)
  std::vector<long> root_counts; // flat index -> member count (root slots)
  bool frozen_ = false;          // guards unite() against post-freeze unions
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
  // Per-level grid context (set around phaseA SELF-pair walks only; see
  // walk's self-node gate). FOF_GRID_ROOT_ONLY=1 restores root-only
  // gating as the A/B arm.
  const TreePieceRef* grid_ctx_ = nullptr;
  static bool gridRootOnly() {
    static const bool on = [] {
      const char* e = std::getenv("FOF_GRID_ROOT_ONLY");
      return e && std::atoi(e) != 0;
    }();
    return on;
  }
  // Sliced-drain state (campaign P1): stage-active flag guards the
  // start-of-stage initialization across slice re-entries; accumulated
  // wall over completed slices.
  bool pb_active_ = false;
  double pb_t_accum_ = 0.0;
  // Campaign S2: the partition this PE currently holds (-1 = none);
  // persists across P1 slice re-entries.
  int pb_cur_part_ = -1;
  // Final-reduction callback of the within-process chain (startPhase1Chain).
  CkCallback done_cb_;
  // Touched-component (negative-label) per-process totals held between
  // histogramShard and collectTouchedCounts.
  std::vector<std::pair<long, long>> touched_totals_;
  // Box period for PBC (design/pbc.md); {0,0,0} = open (default).
  Vector3D<Real> period_ = Vector3D<Real>(0, 0, 0);
};

namespace fof {

// TreePiece-registration functor, delivered to every TreePiece element through
// the core's generic TreePiece::callPerTreePieceFn hook: hands the element's
// local tree root and contiguous particle block to the FoFPhase1 branch on
// the element's PE. This is the inversion that keeps the core FoF-free —
// the core exposes TreePiece views to any PerTreePieceAble, and this module
// consumes them; the core never names an FoF type.
template <typename Data>
class TreePieceRegisterFn : public paratreet::PerTreePieceAble<Data> {
  CProxy_FoFPhase1<Data> fof_;

 public:
  TreePieceRegisterFn(void) = default;
  explicit TreePieceRegisterFn(CProxy_FoFPhase1<Data> fof) : fof_(fof) {}
  TreePieceRegisterFn(CkMigrateMessage* m)
      : paratreet::PerTreePieceAble<Data>(m) {}

  // Hand-expanded PUPable_decl_template: the macro's register_PUP_ID body
  // calls register_constructor unqualified, which fails here because the
  // PUP::able base is reached through a DEPENDENT base class
  // (PerTreePieceAble<Data>) — unqualified lookup does not search dependent
  // bases. Same contents, with the call qualified.
 private:
  static PUP::able* call_PUP_constructor(void) {
    return new TreePieceRegisterFn<Data>((CkMigrateMessage*)0);
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
    paratreet::PerTreePieceAble<Data>::pup(p);
    p | fof_;
  }

  virtual void operator()(Node<Data>* local_root, Particle* particles,
                          int n_particles) override {
    fof_.ckLocalBranch()->registerTreePiece(local_root, particles, n_particles);
  }

  // Flat-tree form (src/DeviceTree.h). dnodes is null unless
  // PARATREET_DEVICE_TREE is on, and phase 1's CPU paths never read it,
  // so this is the same registration either way.
  virtual void operator()(Node<Data>* local_root, Particle* particles,
                          int n_particles, const paratreet::DNode* dnodes,
                          int n_dnodes) override {
    fof_.ckLocalBranch()->registerTreePiece(local_root, particles, n_particles,
                                            dnodes, n_dnodes);
  }
};

template <typename Data>
PUP::able::PUP_ID TreePieceRegisterFn<Data>::my_PUP_ID = 0;

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
  PUPable_reg2(TreePieceRegisterFn<Data>, makeName("fof::TreePieceRegisterFn"));
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
  // Device path (design/phase1-gpu.md section 18). Zero when the CPU chain
  // computed phase 1; in Replace mode the four CPU stages above are zero
  // instead, because they genuinely did not run. `device_wall` is the wall
  // of the whole device chain on the driving thread, so it is the one to
  // compare against phaseA+phaseB+merge+relabel.
  double device_wall = 0, device_pack = 0, device_tree = 0, device_pass = 0,
         device_scatter = 0, device_block = 0;
};

// Convenience driver for the full phase-1 sequence. Must be called from a
// [threaded] entry method (uses blocking callbacks), between tree build and
// the next rebuild/reset (registered particle blocks must stay alive).
// `stages`, if non-null, receives the per-stage wall times.
template <typename Data>
void runFoFPhase1(CProxy_TreePiece<Data> treepieces,
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
    fof::TreePieceRegisterFn<Data> reg_fn(fof);
    treepieces.callPerTreePieceFn(
        CkReference<paratreet::PerTreePieceAble<Data>>(reg_fn),
        CkCallbackResumeThread());
  }
  local.register_s = CkWallTimer() - t;

  const FoFGpuMode gpu_mode = fofGpuMode();
#ifndef FOF_GPU
  // Refusing beats falling back. A binary built without FOF_GPU that
  // quietly ran the CPU chain when asked for the device path would report
  // "the GPU path" timings that are the CPU path's, which is the same
  // class of silent-no-op as trap 16's untraced traced run.
  if (gpu_mode != FoFGpuMode::Off)
    CkAbort("FoF: FOF_GPU_PHASE1/FOF_GPU_VERIFY was set but this binary was "
            "built without FOF_GPU. Rebuild with GPU=1 (fof/gpu/rebuild_deps.sh).");
#endif
  // The device traversal has no periodic wrap (design/pbc.md is a CPU-only
  // path so far, and section 8 lists PBC as explicitly deferred). Under
  // Verify that shows up as mismatches on the boundary particles; under
  // Replace it would be a quietly wrong answer at the box faces, which is
  // exactly the kind of result that looks plausible.
  if (gpu_mode == FoFGpuMode::Replace &&
      (period.x != 0 || period.y != 0 || period.z != 0))
    CkAbort("FoF GPU: FOF_GPU_PHASE1 does not implement periodic boundaries "
            "(period %g %g %g). Run without -P, or use FOF_GPU_VERIFY to "
            "measure the device pass beside the CPU answer.",
            (double)period.x, (double)period.y, (double)period.z);

  // The CPU chain: one broadcast starts the within-process chain; the
  // single global reduction at its end delivers the max-reduced stage
  // walls. SKIPPED ENTIRELY in Replace mode — that is stage 4.
  if (gpu_mode != FoFGpuMode::Replace) {
    void* result = nullptr;
    fof.startPhase1Chain(b2, grid_occupancy_threshold,
                         CkCallbackResumeThread(result));
    CkReductionMsg* m = (CkReductionMsg*)result;
    const double* v = (const double*)m->getData();
    local.phaseA = v[0];
    local.phaseB = v[1];
    local.merge = v[2];
    local.relabel = v[3];
    delete m;
  }
  // The device chain (design/phase1-gpu.md sections 11-18).
  //
  // In Verify mode it runs AFTER the CPU chain, deliberately: the chain
  // has then written every particle's PROCESS-level tip into
  // group_number, which is exactly what the device union-find computes,
  // so the check is an exact per-particle comparison against an answer
  // this codebase already trusts. The device pass reads only positions,
  // orders and the flat tree, and writes only its own buffers, so it
  // cannot perturb the result it is being checked against.
  //
  // In Replace mode it is the only thing that runs, and deviceAdopt turns
  // its labels into the section-4 contract that everything downstream
  // consumes.
  if (gpu_mode != FoFGpuMode::Off) {
    double tg = CkWallTimer();
    void* gres = nullptr;
    fof.deviceStage0(b2, CkCallbackResumeThread(gres));
    CkReductionMsg* gm = (CkReductionMsg*)gres;
    const double* gv = (const double*)gm->getData();
    local.device_wall = CkWallTimer() - tg;
    local.device_pack = gv[0];
    local.device_pass = gv[1];
    local.device_scatter = gv[2];
    local.device_tree = gv[4] + gv[5];
    local.device_block = gv[6];
    // The timings above are what `-c stats` reports as the phase1_device
    // line; gv[3] is the verify-mode mismatch count, and nonzero is fatal.
    if (gv[3] != 0)
      CkAbort("FoF GPU: the device labeling disagrees with the CPU "
              "labeling on %ld particles", (long)gv[3]);
    delete gm;
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
