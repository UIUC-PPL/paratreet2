// Kokkos implementation of the device side of FoF phase 1
// (design/phase1-gpu.md). Compiled by hipcc (or nvcc_wrapper), NEVER by
// charmc: no Charm header is included here, and FoFDevice.h names no
// Kokkos type, so the two toolchains meet only at a POD interface.

#include "FoFDevice.h"

#include <Kokkos_Core.hpp>

#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <climits>
#include <vector>

#if defined(KOKKOS_ENABLE_HIP)
#include <hip/hip_runtime.h>
#endif
#if defined(KOKKOS_ENABLE_CUDA)
#include <cuda_runtime.h>
#endif

namespace fofgpu {

namespace {

using ExecSpace = Kokkos::DefaultExecutionSpace;
using MemSpace = ExecSpace::memory_space;

#if defined(KOKKOS_ENABLE_HIP)
using PinnedSpace = Kokkos::HIPHostPinnedSpace;
#elif defined(KOKKOS_ENABLE_CUDA)
using PinnedSpace = Kokkos::CudaHostPinnedSpace;
#else
using PinnedSpace = Kokkos::HostSpace;
#endif

// Top tree over the process's TreePiece roots. The pieces are ~500
// scattered boxes; without a tree over them every leaf's traversal would
// start by testing all of them (500 x 2.6M leaf tests at 80M). Built on
// the host at upload, it is tiny (2 x n_pieces nodes).
struct DTopNode {
  float lo[3], hi[3];
  int left, right;  // indices into the top array, or -1
  int piece;        // >= 0: a leaf, index into piece_root; else -1
};

template <typename T>
using DView = Kokkos::View<T*, MemSpace>;
template <typename T>
using HView = Kokkos::View<T*, PinnedSpace>;

// Kokkos::initialize is process-wide and must happen exactly once. The
// owning nodegroup branch is the only caller (design/phase1-gpu.md
// section 6.1 — note the jacobi2D pattern uses a GROUP, which calls this
// once per PE and would double-initialize at ppn > 1), but this library
// also has a standalone test driver, so ownership is tracked rather than
// assumed: whoever initialized Kokkos finalizes it.
bool g_we_initialized_kokkos = false;

}  // namespace

struct Device::Impl {
  bool inited = false;
  int device_id = -1;
  size_t n = 0;

  HView<float> h_pos;
  HView<long> h_order;
  HView<long> h_label;

  DView<float> d_pos;
  DView<long> d_order;
  DView<long> d_label;

  HView<DDNode> h_nodes;
  HView<int> h_piece_root;
  HView<int> h_piece_base;
  DView<DDNode> d_nodes;
  DView<DTopNode> d_top;
  DView<int> d_leaves;     // node indices of the leaves
  DView<int> d_parent;     // union-find over process-flat particle indices
  DView<int> d_node_rep;   // positive-certificate memo, -1 = unpublished
  DView<int> d_grid_root;  // stage 3: maximal dense ancestor, -1 = none
  long n_leaves = 0;
  int n_top = 0;
  std::vector<DTopNode> h_top;
  // Stage 4: these two outlive one call because enqueuePhase1() returns
  // with kernels still reading them and completePhase1() reads them back.
  // `counters` in particular was a function-local View, which would have
  // been destroyed while the walk was still incrementing it.
  Kokkos::View<long*, MemSpace> counters;
  WalkStats pending;
  bool in_flight = false;
  DView<int> d_piece_root;
  DView<int> d_piece_base;
  long n_nodes = 0;
  int n_pieces = 0;

  // The Charm HAPI stream, or null for the default execution space.
  // Stored as a raw handle and wrapped ON DEMAND rather than held as an
  // ExecSpace member: a Kokkos::HIP instance cannot be constructed
  // before Kokkos::initialize, and an Impl member would be constructed
  // in Device's constructor — which the nodegroup runs at chare
  // creation, long before init(). (Failure mode if you get this wrong:
  // "Kokkos::HIP::HIP instance constructor : ERROR device not
  // initialized" at startup.) Wrapping is a handle copy, not a
  // resource acquisition, so doing it per call costs nothing.
  void* stream = nullptr;

  ExecSpace exec() const {
#if defined(KOKKOS_ENABLE_HIP)
    return stream ? ExecSpace(static_cast<hipStream_t>(stream)) : ExecSpace();
#elif defined(KOKKOS_ENABLE_CUDA)
    return stream ? ExecSpace(static_cast<cudaStream_t>(stream)) : ExecSpace();
#else
    return ExecSpace();
#endif
  }
};


// ---------------------------------------------------------------------
// Stage 2 kernels (design/phase1-gpu.md section 5).
// ---------------------------------------------------------------------

namespace {

// Box predicates, transliterated from paratreet::mindist2 / maxdist2
// (fof/FoFPhase1.h). Same operation order, and the whole library is
// built -ffp-contract=off, so a pair sitting exactly on b decides the
// same way it does on the CPU.
KOKKOS_INLINE_FUNCTION float boxMinDist2(const float* alo, const float* ahi,
                                         const float* blo, const float* bhi) {
  float d2 = 0.f;
  for (int k = 0; k < 3; k++) {
    const float g = fmaxf(alo[k] - bhi[k], blo[k] - ahi[k]);
    if (g > 0.f) d2 += g * g;
  }
  return d2;
}

KOKKOS_INLINE_FUNCTION float boxMaxDist2(const float* alo, const float* ahi,
                                         const float* blo, const float* bhi) {
  float d2 = 0.f;
  for (int k = 0; k < 3; k++) {
    const float d = fmaxf(fabsf(alo[k] - bhi[k]), fabsf(ahi[k] - blo[k]));
    d2 += d * d;
  }
  return d2;
}

// Lock-free union-find over process-flat particle indices.
//
// UNION BY MINIMUM GLOBAL ORDER, exactly as the CPU's unite() does
// (FoFPhase1.h): particle orders are globally unique, so this is a TOTAL
// order, every parent link points to a strictly smaller order, and the
// parent graph is acyclic BY CONSTRUCTION — which is what makes the CAS
// attach safe with no rank scheme and no lock. Path compression uses
// plain stores: they only ever move a link closer to the root, so a race
// with another thread's compression or CAS cannot invert the order.
//
// The result is ORDER-INDEPENDENT (union-find is a semilattice), so the
// nondeterministic execution order cannot change the final partition and
// the tip — the minimum order in the component — is deterministic. That
// is what makes an exact comparison against the CPU arm meaningful.
KOKKOS_INLINE_FUNCTION int ufFind(const Kokkos::View<int*, MemSpace>& parent,
                                  int i) {
  while (true) {
    const int p = parent(i);
    if (p == i) return i;
    const int g = parent(p);
    parent(i) = g;  // path splitting
    i = g;
  }
}

KOKKOS_INLINE_FUNCTION void ufUnite(const Kokkos::View<int*, MemSpace>& parent,
                                    const Kokkos::View<long*, MemSpace>& order,
                                    int a, int b) {
  while (true) {
    a = ufFind(parent, a);
    b = ufFind(parent, b);
    if (a == b) return;
    int lo = a, hi = b;
    if (order(a) > order(b)) { lo = b; hi = a; }
    // Attach the larger-order root under the smaller-order one.
    if (Kokkos::atomic_compare_exchange(&parent(hi), hi, lo) == hi) return;
  }
}

// Star-union every particle under node m into its first particle, at most
// once process-wide. Returns immediately (doing nothing) if another
// thread already owns or has finished the job — see the call site for why
// that is safe.
KOKKOS_INLINE_FUNCTION void starUnion(
    const Kokkos::View<DDNode*, MemSpace>& nodes,
    const Kokkos::View<int*, MemSpace>& parent,
    const Kokkos::View<long*, MemSpace>& order, 
    const Kokkos::View<int*, MemSpace>& node_rep, int m) {
  if (node_rep(m) != -1) return;  // claimed or done
  if (Kokkos::atomic_compare_exchange(&node_rep(m), -1, -2) != -1) return;
  const DDNode d = nodes(m);
  for (int i = 1; i < d.n_below; i++)
    ufUnite(parent, order, d.part_begin, d.part_begin + i);
  Kokkos::atomic_store(&node_rep(m), d.part_begin);
}

// Team form of the star-union: claim the node once process-wide, then
// split its particles across the lanes. The barrier before publishing is
// required — the memo must not read "done" until every lane's unite has
// landed.
template <typename TeamMember>
KOKKOS_INLINE_FUNCTION void teamStarUnion(
    const TeamMember& team, const Kokkos::View<DDNode*, MemSpace>& nodes,
    const Kokkos::View<int*, MemSpace>& parent,
    const Kokkos::View<long*, MemSpace>& order,
    const Kokkos::View<int*, MemSpace>& node_rep, int m) {
  int claimed = 0;
  Kokkos::single(Kokkos::PerTeam(team),
                 [&](int& out) {
                   out = (node_rep(m) == -1 &&
                          Kokkos::atomic_compare_exchange(&node_rep(m), -1,
                                                          -2) == -1)
                             ? 1
                             : 0;
                 },
                 claimed);
  if (!claimed) return;
  const DDNode d = nodes(m);
  Kokkos::parallel_for(Kokkos::TeamThreadRange(team, 1, d.n_below),
                       [&](const int i) {
                         ufUnite(parent, order, d.part_begin, d.part_begin + i);
                       });
  team.team_barrier();
  Kokkos::single(Kokkos::PerTeam(team),
                 [&]() { Kokkos::atomic_store(&node_rep(m), d.part_begin); });
}

// Residual stencil for the b/sqrt(6) cell grid, generated exactly as the
// CPU's gridOffsets() does (fof/FoFPhase1.h): forward half-space, and a
// pair of cells offset by d can hold a pair within b only if
// sum(((|d|-1)+)^2) <= 6. Face offsets (a single +-1) are in the list and
// are recognized by the kernel as test-free unions.
std::vector<int> gridOffsetTable() {
  std::vector<int> v;
  for (int dz = -3; dz <= 3; dz++)
    for (int dy = -3; dy <= 3; dy++)
      for (int dx = -3; dx <= 3; dx++) {
        if (dz < 0 || (dz == 0 && dy < 0) || (dz == 0 && dy == 0 && dx <= 0))
          continue;
        auto g = [](int d) { int a = (d < 0 ? -d : d) - 1; return a > 0 ? a : 0; };
        if (g(dx) * g(dx) + g(dy) * g(dy) + g(dz) * g(dz) <= 6) {
          v.push_back(dx); v.push_back(dy); v.push_back(dz);
        }
      }
  return v;
}

// Cell dimensions of a node's grid. Kept in one place because the count
// (used to size the cell array) and the binning (used to fill it) MUST
// agree exactly — a mismatch writes outside the node's block and
// corrupts a neighbour's grid silently.
KOKKOS_INLINE_FUNCTION void gridDims(const DDNode& d, float c, int dim[3]) {
  for (int k = 0; k < 3; k++) {
    const float ext = d.hi[k] - d.lo[k];
    int n = (int)(ext / c) + 1;
    if (n < 1) n = 1;
    dim[k] = n;
  }
}

// ---------------------------------------------------------------------
// The two shapes the traversal can take, behind one interface.
//
// Stage 2 measured team-per-leaf at 1.6x thread-per-leaf and the walk has
// been a team kernel since. Stage 3's suppression then removed ~40x of
// the work behind each leaf, which is exactly the kind of change that can
// invert such a result: the team pays a broadcast and a barrier on EVERY
// pop, and there is now far less work per pop to amortise them against.
//
// Rather than keep two copies of a 200-line traversal in sync — the
// reliable way to make an A/B lie — the body is written once against
// these two adapters. `Wave` is the real wavefront; `Solo` is a single
// thread, where every collective degenerates to straight-line code and
// the shared stack becomes a private array.
template <typename Member>
struct Wave {
  const Member& m;
  KOKKOS_INLINE_FUNCTION void barrier() const { m.team_barrier(); }
  template <typename F>
  KOKKOS_INLINE_FUNCTION void one(const F& f) const {
    Kokkos::single(Kokkos::PerTeam(m), f);
  }
  template <typename F>
  KOKKOS_INLINE_FUNCTION int oneBcast(const F& f) const {
    int out = 0;
    Kokkos::single(Kokkos::PerTeam(m), f, out);
    return out;
  }
  template <typename F>
  KOKKOS_INLINE_FUNCTION void forRange(int begin, int end, const F& f) const {
    Kokkos::parallel_for(Kokkos::TeamThreadRange(m, begin, end), f);
  }
  template <typename F>
  KOKKOS_INLINE_FUNCTION int countRange(int begin, int end, const F& f) const {
    int acc = 0;
    Kokkos::parallel_reduce(Kokkos::TeamThreadRange(m, begin, end), f, acc);
    return acc;
  }
};

struct Solo {
  KOKKOS_INLINE_FUNCTION void barrier() const {}
  template <typename F>
  KOKKOS_INLINE_FUNCTION void one(const F& f) const { f(); }
  template <typename F>
  KOKKOS_INLINE_FUNCTION int oneBcast(const F& f) const {
    int out = 0;
    f(out);
    return out;
  }
  template <typename F>
  KOKKOS_INLINE_FUNCTION void forRange(int begin, int end, const F& f) const {
    for (int i = begin; i < end; i++) f(i);
  }
  template <typename F>
  KOKKOS_INLINE_FUNCTION int countRange(int begin, int end, const F& f) const {
    int acc = 0;
    for (int i = begin; i < end; i++) f(i, acc);
    return acc;
  }
};

// Star-union every particle under node m into its first particle, at most
// once process-wide, with the work split across whatever the adapter has.
// The barrier before publishing is required: the memo must not read "done"
// until every lane's unite has landed.
template <typename Team>
KOKKOS_INLINE_FUNCTION void starUnionT(
    const Team& tm, const Kokkos::View<DDNode*, MemSpace>& nodes,
    const Kokkos::View<int*, MemSpace>& parent,
    const Kokkos::View<long*, MemSpace>& order,
    const Kokkos::View<int*, MemSpace>& node_rep, int m) {
  const int claimed = tm.oneBcast([&](int& o) {
    o = (node_rep(m) == -1 &&
         Kokkos::atomic_compare_exchange(&node_rep(m), -1, -2) == -1)
            ? 1
            : 0;
  });
  if (!claimed) return;
  const DDNode d = nodes(m);
  tm.forRange(1, d.n_below, [&](const int i) {
    ufUnite(parent, order, d.part_begin, d.part_begin + i);
  });
  tm.barrier();
  tm.one([&]() { Kokkos::atomic_store(&node_rep(m), d.part_begin); });
}

// One leaf's traversal of the process forest. Written once, run by both
// kernel shapes. `stack`/`sp` are team scratch under Wave and a private
// array under Solo; everything else is identical, which is the point.
template <typename Team>
KOKKOS_INLINE_FUNCTION void walkOneLeaf(
    const Team& tm, int li, int* stack, int* sp, int kStack, float b2,
    const Kokkos::View<DDNode*, MemSpace>& d_nodes,
    const Kokkos::View<DTopNode*, MemSpace>& d_top,
    const Kokkos::View<int*, MemSpace>& d_piece_root,
    const Kokkos::View<int*, MemSpace>& d_parent,
    const Kokkos::View<long*, MemSpace>& d_order,
    const Kokkos::View<int*, MemSpace>& d_node_rep,
    const Kokkos::View<int*, MemSpace>& d_grid_root,
    const Kokkos::View<float*, MemSpace>& d_pos,
    const Kokkos::View<long*, MemSpace>& counters) {
  const DDNode L = d_nodes(li);
  // The dense ancestor whose grid pass already solved every pair inside
  // it, or -1. The traversal can only reach that subtree THROUGH this
  // node, so pruning here prunes all of it.
  const int l_grid = d_grid_root(li);

  // Connectivity suppression state (design section 15). Once L is known
  // internally connected, l_rep is a handle to its component and stays
  // valid forever: union-find is monotone, so a node never becomes
  // disconnected and find(l_rep) is always current.
  int l_rep = -1;
  bool l_dirty = true;

  tm.one([&]() {
    stack[0] = -1;  // top-tree root
    sp[0] = 1;
  });
  tm.barrier();

  while (true) {
    // Pop once for the whole team and broadcast; INT_MIN = done.
    const int e = tm.oneBcast(
        [&](int& out) { out = (sp[0] > 0) ? stack[--sp[0]] : INT_MIN; });
    if (e == INT_MIN) break;

    if (e < 0) {  // top tree
      const DTopNode t = d_top(-e - 1);
      if (boxMinDist2(L.lo, L.hi, t.lo, t.hi) <= b2) {
        tm.one([&]() {
          if (t.piece >= 0) {
            if (sp[0] < kStack) stack[sp[0]++] = d_piece_root(t.piece);
            else Kokkos::atomic_fetch_add(&counters(0), 1L);
          } else if (sp[0] + 2 <= kStack) {
            stack[sp[0]++] = -(t.left + 1);
            stack[sp[0]++] = -(t.right + 1);
          } else {
            Kokkos::atomic_fetch_add(&counters(0), 1L);
          }
        });
      }
      tm.barrier();
      continue;
    }

    // Every pair inside L's dense ancestor was solved by the grid
    // pre-pass, and this is the only way in. Same prune the CPU takes
    // when gridSelfUnionRange returns true.
    if (e == l_grid) {
      tm.barrier();
      continue;
    }

    const DDNode M = d_nodes(e);
    if (boxMinDist2(L.lo, L.hi, M.lo, M.hi) > b2) {
      tm.barrier();
      continue;
    }

    // ---- connectivity suppression (the CPU's prune_fn) ----
    // If L and M are each internally connected AND already in the same
    // component, no pair between them can add anything, at any level
    // below either — so the whole subtree is dead. This is what stops a
    // leaf in a dense core from re-certifying against every node of a
    // core it has already joined.
    //
    // The memo is d_node_rep, exactly as on the CPU, where the
    // certificate memo and the connectivity memo are deliberately the
    // SAME map: an entry means "internally connected, with this
    // representative", which is monotone and never has to be
    // invalidated. All reads that steer collective code are broadcast
    // from one lane; a per-lane read of a racing location could
    // otherwise diverge inside a team barrier.
    if (l_rep < 0) {
      const int r0 = tm.oneBcast([&](int& o) { o = d_node_rep(li); });
      if (r0 >= 0) {
        l_rep = r0;
      } else if (l_dirty) {
        // Lazy upgrade: L is internally connected iff all its particles
        // currently share a root. Each lane compares against the root it
        // read itself, which is sound even when they differ: roots only
        // ever move upward, so a particle matching ANY root that
        // part_begin held is connected to part_begin.
        const int root0 = ufFind(d_parent, L.part_begin);
        const int bad = tm.countRange(1, L.n_below, [&](const int i, int& acc) {
          if (ufFind(d_parent, L.part_begin + i) != root0) acc += 1;
        });
        if (bad == 0) {
          l_rep = L.part_begin;
          tm.one([&]() {
            // CAS from -1 only: never clobber a star-union's in-progress
            // claim (-2) or another rep.
            Kokkos::atomic_compare_exchange(&d_node_rep(li), -1, L.part_begin);
          });
        }
        l_dirty = false;
      }
    }
    if (l_rep >= 0) {
      int mr = tm.oneBcast([&](int& o) { o = d_node_rep(e); });
      if (mr < 0 && M.child_begin >= 0) {
        // Bottom-up upgrade from the children's memos, the CPU's
        // connectedRep for an internal node: non-recursive, so
        // connectivity percolates up one level per visit as the walk
        // revisits the node against new partners. M's own first particle
        // is child 0's first particle, so it is a valid representative.
        mr = tm.oneBcast([&](int& o) {
          o = -1;
          int r = -1;
          for (int c = 0; c < M.n_children; c++) {
            const int cr = d_node_rep(M.child_begin + c);
            if (cr < 0) return;
            const int fr = ufFind(d_parent, cr);
            if (r < 0) r = fr;
            else if (fr != r) return;
          }
          if (r < 0) return;
          Kokkos::atomic_compare_exchange(&d_node_rep(e), -1, M.part_begin);
          o = M.part_begin;
        });
      }
      // Decided by ONE lane and broadcast. ufFind reads memory that other
      // teams are actively mutating, so two lanes of the same wavefront
      // can legitimately return different roots; branching per lane on
      // that would split the team across a barrier further down.
      const int same = tm.oneBcast([&](int& o) {
        o = (mr >= 0 &&
             ufFind(d_parent, mr) == ufFind(d_parent, l_rep))
                ? 1
                : 0;
        if (o) Kokkos::atomic_fetch_add(&counters(3), 1L);
      });
      if (same) {
        tm.barrier();
        continue;
      }
    }

    // Positive certificate: every cross pair is a friend, so both sides
    // collapse with no distance test and no descent. The memo is CLAIMED
    // with a CAS, not merely checked: thousands of teams can reach a hot
    // node at once, and without the claim they all run the same
    // O(n_below) star-union.
    if (boxMaxDist2(L.lo, L.hi, M.lo, M.hi) <= b2) {
      starUnionT(tm, d_nodes, d_parent, d_order, d_node_rep, li);
      if (e != li) {
        starUnionT(tm, d_nodes, d_parent, d_order, d_node_rep, e);
        tm.one([&]() {
          // Uniting the two FIRST particles is enough: whoever won each
          // claim is committed to collapsing that node, and union-find is
          // order-independent, so it does not matter which finishes first.
          ufUnite(d_parent, d_order, L.part_begin, M.part_begin);
          Kokkos::atomic_fetch_add(&counters(1), 1L);
        });
      }
      // L is now internally connected (this team's star-union did it, or
      // the team that owns the claim is committed to it), so let the next
      // iteration pick up the published memo.
      l_dirty = true;
      tm.barrier();
      continue;
    }

    if (M.child_begin < 0) {
      // Leaf pair, owned by the smaller node index so the mirror visit
      // from M's own team does nothing (the each-pair-once rule).
      if (e >= li) {
        const int ab = L.part_begin, bb = M.part_begin;
        const int nb_ = (e == li) ? L.n_below : M.n_below;
        // The WHOLE product over the lanes, not one lane per i. Ranging
        // over L.n_below put 12 of the 64 lanes to work and made each of
        // them walk all 12 partners serially; ranging over the flattened
        // 12x12 fills the wavefront and finishes in ~2 steps instead of
        // 12. The cost is reloading the i-side position per pair, which
        // is a broadcast from cache, not a gather.
        const int self = (e == li) ? 1 : 0;
        tm.forRange(0, L.n_below * nb_, [&](const int t) {
          const int i = t / nb_;
          const int j = t - i * nb_;
          if (self && j <= i) return;  // unordered pairs once
          const int q = ab + i;
          const int p = (self ? ab : bb) + j;
          const float dx = d_pos(3 * q) - d_pos(3 * p);
          const float dy = d_pos(3 * q + 1) - d_pos(3 * p + 1);
          const float dz = d_pos(3 * q + 2) - d_pos(3 * p + 2);
          if (dx * dx + dy * dy + dz * dz <= b2)
            ufUnite(d_parent, d_order, q, p);
        });
        tm.one([&]() { Kokkos::atomic_fetch_add(&counters(2), 1L); });
        l_dirty = true;  // L may have just merged; recheck the memo
      }
      tm.barrier();
      continue;
    }

    tm.one([&]() {
      if (sp[0] + M.n_children <= kStack) {
        for (int c = 0; c < M.n_children; c++)
          stack[sp[0]++] = M.child_begin + c;
      } else {
        Kokkos::atomic_fetch_add(&counters(0), 1L);
      }
    });
    tm.barrier();
  }
}

}  // namespace

bool Device::available() {
#if defined(KOKKOS_ENABLE_HIP) || defined(KOKKOS_ENABLE_CUDA)
  return true;
#else
  return false;
#endif
}

const char* Device::backendName() { return ExecSpace::name(); }

Device::Device() : p_(new Impl()) {}

Device::~Device() {
  finalize();
  delete p_;
}

void Device::init(int device_id) {
  if (p_->inited) return;
  if (!Kokkos::is_initialized()) {
    Kokkos::InitializationSettings args;
    if (device_id >= 0) args.set_device_id(device_id);
    // Explicit settings, never argv: Kokkos must not parse Charm's
    // command line (it would eat unknown flags and complain about ours).
    Kokkos::initialize(args);
    g_we_initialized_kokkos = true;
  }
  p_->device_id = device_id;
  p_->inited = true;
}

void Device::finalize() {
  if (!p_->inited) return;
  // Drop the Views before finalizing Kokkos: their deallocation goes
  // through the runtime.
  p_->h_pos = HView<float>();
  p_->h_order = HView<long>();
  p_->h_label = HView<long>();
  p_->d_pos = DView<float>();
  p_->d_order = DView<long>();
  p_->d_label = DView<long>();
  p_->h_nodes = HView<DDNode>();
  p_->h_piece_root = HView<int>();
  p_->h_piece_base = HView<int>();
  p_->d_nodes = DView<DDNode>();
  p_->d_top = DView<DTopNode>();
  p_->d_leaves = DView<int>();
  p_->d_parent = DView<int>();
  p_->d_node_rep = DView<int>();
  p_->d_grid_root = DView<int>();
  p_->n_leaves = 0;
  p_->n_top = 0;
  p_->h_top.clear();
  p_->d_piece_root = DView<int>();
  p_->d_piece_base = DView<int>();
  p_->n_nodes = 0;
  p_->n_pieces = 0;
  p_->n = 0;
  p_->inited = false;
  if (g_we_initialized_kokkos && Kokkos::is_initialized()) {
    Kokkos::finalize();
    g_we_initialized_kokkos = false;
  }
}

bool Device::initialized() const { return p_->inited; }

DeviceInfo Device::info() const {
  DeviceInfo di;
  di.device_id = p_->device_id;
  di.is_device_backend = available();
  std::snprintf(di.backend, sizeof(di.backend), "%s", ExecSpace::name());
#if defined(KOKKOS_ENABLE_HIP)
  int count = 0;
  if (hipGetDeviceCount(&count) == hipSuccess) di.n_visible = count;
  hipDeviceProp_t prop;
  int dev = p_->device_id >= 0 ? p_->device_id : 0;
  if (hipGetDeviceProperties(&prop, dev) == hipSuccess) {
    std::snprintf(di.name, sizeof(di.name), "%s", prop.name);
    di.total_global_mem = prop.totalGlobalMem;
  }
#elif defined(KOKKOS_ENABLE_CUDA)
  int count = 0;
  if (cudaGetDeviceCount(&count) == cudaSuccess) di.n_visible = count;
  cudaDeviceProp prop;
  int dev = p_->device_id >= 0 ? p_->device_id : 0;
  if (cudaGetDeviceProperties(&prop, dev) == cudaSuccess) {
    std::snprintf(di.name, sizeof(di.name), "%s", prop.name);
    di.total_global_mem = prop.totalGlobalMem;
  }
#else
  di.n_visible = 1;  // host backend: the "device" is this process itself
#endif
  return di;
}

bool Device::checkMapping(int n_procs_on_physical_node) const {
  if (!available()) return true;  // host backend: nothing to share
  DeviceInfo di = info();
  if (di.n_visible <= 0) return false;
  // The invariant is that no two processes SHARE a GPU, which is not the
  // same as using every GPU: 2 processes on an 8-GCD node share nothing
  // (HAPI hands them devices 0 and 4). So the test is <=, not ==. Two
  // acceptable shapes: the launcher bound this process to exactly one
  // device (n_visible == 1, the --gpus-per-task case), or all devices are
  // visible and there are no more processes than devices.
  if (di.n_visible == 1) return true;
  return n_procs_on_physical_node <= di.n_visible;
}

void Device::setStream(void* stream) { p_->stream = stream; }

void Device::resize(size_t n) {
  if (n == p_->n) return;
  // WithoutInitializing: these are staging buffers, every byte is
  // overwritten before it is read, and zero-filling 300+ MB per process
  // is pure cost.
  p_->h_pos = HView<float>(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                              "fof_h_pos"), 3 * n);
  p_->h_order = HView<long>(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                               "fof_h_order"), n);
  p_->h_label = HView<long>(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                               "fof_h_label"), n);
  p_->d_pos = DView<float>(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                              "fof_d_pos"), 3 * n);
  p_->d_order = DView<long>(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                               "fof_d_order"), n);
  p_->d_label = DView<long>(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                               "fof_d_label"), n);
  p_->n = n;
}

size_t Device::size() const { return p_->n; }
float* Device::hostPositions() { return p_->h_pos.data(); }
long* Device::hostOrders() { return p_->h_order.data(); }
long* Device::hostLabels() { return p_->h_label.data(); }

void Device::fence() {
  if (p_->inited) p_->exec().fence();
}

void Device::resizeTree(long n_nodes, int n_pieces) {
  if (!p_->inited || n_nodes <= 0) return;
  if (p_->n_nodes == n_nodes && p_->n_pieces == n_pieces) return;
  p_->h_nodes = HView<DDNode>(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "fof_h_nodes"), n_nodes);
  p_->h_piece_root = HView<int>(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "fof_h_proot"), n_pieces);
  p_->h_piece_base = HView<int>(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "fof_h_pbase"), n_pieces);
  p_->d_nodes = DView<DDNode>(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "fof_d_nodes"), n_nodes);
  p_->d_piece_root = DView<int>(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "fof_d_proot"), n_pieces);
  p_->d_piece_base = DView<int>(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "fof_d_pbase"), n_pieces);
  p_->n_nodes = n_nodes;
  p_->n_pieces = n_pieces;
}

DDNode* Device::hostNodes() { return p_->h_nodes.data(); }
int* Device::hostPieceRoot() { return p_->h_piece_root.data(); }
int* Device::hostPieceBase() { return p_->h_piece_base.data(); }

void Device::uploadTree(TreeStats* out) {
  TreeStats st;
  st.n_nodes = p_->n_nodes;
  st.n_pieces = p_->n_pieces;
  if (!p_->inited || p_->n_nodes == 0) {
    if (out) *out = st;
    return;
  }
  // A NEW tree has just been staged, so the top tree built over the
  // previous one is stale. It was cached on `h_top.empty()` alone and
  // cleared only by finalize(), so from iteration 2 the walk descended a
  // top tree whose boxes and piece links described the PREVIOUS tree
  // build. That went unnoticed because the FoF harness does not move
  // particles between iterations, which makes every rebuild produce an
  // identical tree — the cache was right for the wrong reason, and would
  // have silently under-merged the moment anything moved.
  // Rebuilding is O(n_pieces) on the host (~1000 entries) plus a ~40 KB
  // upload: below the noise of the node upload it rides along with.
  p_->h_top.clear();
  p_->n_top = 0;
  ExecSpace exec = p_->exec();
  Kokkos::Timer timer;
  Kokkos::deep_copy(exec, p_->d_nodes, p_->h_nodes);
  Kokkos::deep_copy(exec, p_->d_piece_root, p_->h_piece_root);
  Kokkos::deep_copy(exec, p_->d_piece_base, p_->h_piece_base);
  exec.fence();
  st.t_upload = timer.seconds();

  // Structural check ON THE DEVICE, not a host echo: this is what proves
  // the array arrived intact and that the indices are self-consistent in
  // device memory. Every node must hold particles, its children must live
  // inside the array and after it (the layout contract in
  // src/DeviceTree.h), and its subtree counts must add up.
  timer.reset();
  auto d_nodes = p_->d_nodes;
  const long n_nodes = p_->n_nodes;
  long bad = 0;
  Kokkos::parallel_reduce(
      "fof_tree_check", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_nodes),
      KOKKOS_LAMBDA(const long i, long& acc) {
        const DDNode& d = d_nodes(i);
        bool ok = d.n_below > 0 && d.part_begin >= 0;
        if (d.child_begin >= 0) {
          ok = ok && d.n_children > 0 && d.child_begin > i &&
               d.child_begin + d.n_children <= n_nodes;
          if (ok) {
            long sum = 0;
            for (int c = 0; c < d.n_children; c++)
              sum += d_nodes(d.child_begin + c).n_below;
            ok = ok && sum == d.n_below;
          }
        } else {
          ok = ok && d.n_children == 0;
        }
        ok = ok && d.lo[0] <= d.hi[0] && d.lo[1] <= d.hi[1] &&
             d.lo[2] <= d.hi[2];
        if (!ok) acc += 1;
      },
      bad);

  // Particles reachable from the piece roots, summed on the device:
  // equals the staged particle count exactly when every tree is complete.
  auto d_root = p_->d_piece_root;
  long under_roots = 0;
  Kokkos::parallel_reduce(
      "fof_tree_roots", Kokkos::RangePolicy<ExecSpace>(exec, 0, p_->n_pieces),
      KOKKOS_LAMBDA(const int i, long& acc) {
        acc += d_nodes(d_root(i)).n_below;
      },
      under_roots);
  exec.fence();
  st.t_check = timer.seconds();
  st.bad_nodes = bad;
  st.particles_under_roots = under_roots;
  if (out) *out = st;
}

// Host-side top tree over the piece root boxes: recursive median split on
// the longest axis of the centroid bounds. n_pieces is a few hundred, so
// this is microseconds and its quality barely matters — what matters is
// that a leaf's traversal starts at ONE node instead of testing every
// piece.
static int buildTopTree(std::vector<DTopNode>& out, const DDNode* nodes,
                        const int* piece_root, int* idx, int begin, int end) {
  const int me = (int)out.size();
  out.push_back(DTopNode());
  DTopNode t;
  for (int k = 0; k < 3; k++) { t.lo[k] = 1e30f; t.hi[k] = -1e30f; }
  for (int i = begin; i < end; i++) {
    const DDNode& r = nodes[piece_root[idx[i]]];
    for (int k = 0; k < 3; k++) {
      if (r.lo[k] < t.lo[k]) t.lo[k] = r.lo[k];
      if (r.hi[k] > t.hi[k]) t.hi[k] = r.hi[k];
    }
  }
  if (end - begin == 1) {
    t.left = t.right = -1;
    t.piece = idx[begin];
    out[me] = t;
    return me;
  }
  int axis = 0;
  float best = t.hi[0] - t.lo[0];
  for (int k = 1; k < 3; k++)
    if (t.hi[k] - t.lo[k] > best) { best = t.hi[k] - t.lo[k]; axis = k; }
  const int mid = begin + (end - begin) / 2;
  std::nth_element(idx + begin, idx + mid, idx + end,
                   [&](int a, int b) {
                     const DDNode& ra = nodes[piece_root[a]];
                     const DDNode& rb = nodes[piece_root[b]];
                     return (ra.lo[axis] + ra.hi[axis]) <
                            (rb.lo[axis] + rb.hi[axis]);
                   });
  t.piece = -1;
  const int l = buildTopTree(out, nodes, piece_root, idx, begin, mid);
  const int r = buildTopTree(out, nodes, piece_root, idx, mid, end);
  t.left = l;
  t.right = r;
  out[me] = t;
  return me;
}

void Device::runPhase1(float b2, float grid_thresh, WalkStats* out) {
  runPhase1Impl(b2, grid_thresh, false);
  completePhase1(out);
}

void Device::enqueuePhase1(float b2, float grid_thresh) {
  runPhase1Impl(b2, grid_thresh, true);
}

// Fence the stream and collect what only the device knows. In the
// synchronous form every stage already fenced and this is just the
// counter readback; in the asynchronous form this is where the walk, the
// freeze and the download are finally waited on — from a Charm callback,
// with the scheduler having run the whole time.
void Device::completePhase1(WalkStats* out) {
  if (!p_->in_flight) {
    if (out) *out = p_->pending;
    return;
  }
  p_->in_flight = false;
  WalkStats& st = p_->pending;
  ExecSpace exec = p_->exec();
  Kokkos::Timer timer;
  exec.fence();
  // Nonzero only on the async path, where the walk/freeze/download walls
  // are not separable: it is the whole tail measured from the callback.
  if (st.t_walk == 0) st.t_device_tail = timer.seconds();
  auto h_counters = Kokkos::create_mirror_view(p_->counters);
  Kokkos::deep_copy(h_counters, p_->counters);
  st.stack_overflows = h_counters(0);
  st.certificates = h_counters(1);
  st.leaf_pairs = h_counters(2);
  st.suppressed = h_counters(3);
  if (out) *out = st;
}

// Free the staging that the rest of the iteration does not read.
// hostLabels() is deliberately kept: the scatter runs after this.
void Device::releaseStaging() {
  if (!p_->inited) return;
  p_->exec().fence();
  p_->h_pos = HView<float>();
  p_->h_order = HView<long>();
  p_->h_nodes = HView<DDNode>();
  p_->h_piece_root = HView<int>();
  p_->h_piece_base = HView<int>();
  p_->d_pos = DView<float>();
  p_->d_order = DView<long>();
  p_->d_nodes = DView<DDNode>();
  p_->d_piece_root = DView<int>();
  p_->d_piece_base = DView<int>();
  p_->d_top = DView<DTopNode>();
  p_->d_leaves = DView<int>();
  p_->d_parent = DView<int>();
  p_->d_node_rep = DView<int>();
  p_->d_grid_root = DView<int>();
  p_->counters = Kokkos::View<long*, MemSpace>();
  p_->h_top.clear();
  p_->n_top = 0;
  p_->n_leaves = 0;
  // Forces the next resize()/resizeTree() to reallocate rather than
  // early-return on a matching size against buffers that no longer exist.
  p_->n = 0;
  p_->n_nodes = 0;
  p_->n_pieces = 0;
  // d_label/h_label survive: `n` is now 0, so resize() will replace them
  // on the next iteration, and the scatter still has to read h_label
  // before then.
}


// ---------------------------------------------------------------------------
// relay41 FIX: keep ROCm's helper threads off the PE cores.
//
// WHAT WAS WRONG.  ROCm creates its helper threads lazily, on the first real
// HIP work, from whatever thread happens to be calling -- and pthread_create
// hands the child the CALLER'S affinity mask.  Under Charm's +pemap that
// caller is a worker PE pinned to exactly one core, so the helper is born
// welded to that core and then alternates with the PE at the scheduler
// timeslice.  Measured on Frontier at 896 PEs (reports/relay40.txt,
// relay41.txt): the victim PE accumulates 2.7 SECONDS of runqueue wait in a
// 26 s run while every unshared PE accumulates 0.0 ms, each quiescence
// detection round costs a full 16 ms slice, and an LD_PRELOAD interposer on
// pthread_create pins the blame precisely -- 256 libamdhip64 and 128
// libhsa-runtime64 creations from a PINNED creator, with this function at
// frames #14/#15 of the backtrace.
//
// WHERE IS SAFE, and this is the whole design decision.  NOT "every CPU in
// the cpuset".  A Charm PE that has nothing to do SPINS in the scheduler's
// idle loop, so every PE core carries a permanently runnable thread; a
// wide-masked helper would simply pick one of them and start swapping it out.
// The victim would rotate instead of disappearing, which is no fix at all.
// The SMT SIBLINGS of the PE cores carry nothing: the pemap never names them,
// so their runqueues are empty.  A helper there gets a runqueue to itself.  It
// shares the physical core's execution units with its PE, which costs a few
// percent WHILE IT IS ACTUALLY RUNNING, but it never deschedules anyone.
// (Cores 0/8/16/... and their siblings are not in the job's cpuset on
// Frontier -- the first core of each L3 group is reserved -- so the obvious
// "park it on a spare core" is not available.)
//
// The caller's OWN core is deliberately excluded from the widened mask, so
// the child cannot inherit it.  The calling PE is therefore migrated to a
// sibling for the duration of this one call and put straight back.
//
// FOF_NO_AFFINITY_FIX=1 disables the whole thing, for A/B.
// ---------------------------------------------------------------------------
#if defined(__linux__)
#include <sched.h>
#include <dirent.h>

namespace {

// Parse a Linux cpu-list -- "0,8,16" or "0-3,8" or "4,68" -- into `out`.
// `skip` is excluded (used to drop a core from its own sibling list); pass -1
// to keep everything.
void fof_parse_cpulist(const char* buf, cpu_set_t* out, int skip) {
  const char* p = buf;
  while (*p) {
    char* end = nullptr;
    long a = strtol(p, &end, 10);
    if (end == p) break;
    long b = a;
    if (*end == '-') { p = end + 1; b = strtol(p, &end, 10); }
    for (long x = a; x <= b; ++x)
      if ((int)x != skip && x >= 0 && x < CPU_SETSIZE) CPU_SET((int)x, out);
    p = (*end == ',') ? end + 1 : end;
    if (*end == '\0') break;
  }
}

// Add the SMT siblings of cpu `c` (excluding c) to `out`, from sysfs.
// thread_siblings_list is either "4,68" or "4-5" depending on the machine.
void fof_add_siblings(int c, cpu_set_t* out) {
  char path[160];
  snprintf(path, sizeof path,
           "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", c);
  FILE* f = fopen(path, "r");
  if (!f) return;
  char buf[256];
  if (fgets(buf, sizeof buf, f)) fof_parse_cpulist(buf, out, c);
  fclose(f);
}

// The logical CPUs the pemap left empty: for every thread of this process that
// is pinned to exactly one core, that core's SMT sibling.  Any core that some
// thread is pinned to is then removed, so the result cannot contain a PE core
// even on a machine where SMT siblings overlap the pemap.
bool fof_helper_mask(cpu_set_t* out) {
  CPU_ZERO(out);
  cpu_set_t pinned;
  CPU_ZERO(&pinned);
  DIR* d = opendir("/proc/self/task");
  if (!d) return false;
  int npinned = 0;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] == '.') continue;
    long tid = strtol(e->d_name, nullptr, 10);
    if (tid <= 0) continue;
    cpu_set_t m;
    CPU_ZERO(&m);
    if (sched_getaffinity((pid_t)tid, sizeof m, &m) != 0) continue;
    if (CPU_COUNT(&m) != 1) continue;            // not a pinned PE
    for (int c = 0; c < CPU_SETSIZE; ++c)
      if (CPU_ISSET(c, &m)) { CPU_SET(c, &pinned); fof_add_siblings(c, out); ++npinned; break; }
  }
  closedir(d);
  if (npinned == 0) return false;
  for (int c = 0; c < CPU_SETSIZE; ++c)
    if (CPU_ISSET(c, &pinned)) CPU_CLR(c, out);  // never hand back a PE core
  return CPU_COUNT(out) > 0;
}

// RAII: widen for the duration of a block, restore on the way out.
struct FoFHelperAffinityScope {
  cpu_set_t save;
  bool active;
  FoFHelperAffinityScope() : active(false) {
    static const bool off = (getenv("FOF_NO_AFFINITY_FIX") != nullptr);
    if (off) return;
    CPU_ZERO(&save);
    if (sched_getaffinity(0, sizeof save, &save) != 0) return;
    if (CPU_COUNT(&save) != 1) return;      // not pinned: nothing to protect

    cpu_set_t safe;
    CPU_ZERO(&safe);
    const char* how = "SMT siblings";

    // FOF_HELPER_CPUS names the landing zone explicitly, e.g. the CCD-first
    // cores 0,8,16,24,32,40,48,56 that Frontier reserves for system services
    // and that --core-spec=0 makes available.  A GPU-completion helper IS a
    // service thread, so that is where it belongs -- and unlike the sibling
    // set it survives ppn 14, where every SMT sibling is itself a PE.
    // The list is the SAME on every process; each process's helpers land on
    // whichever of those CPUs the kernel picks, and locality is preserved
    // because the CCD-first core shares an L3 with that process's own PEs.
    const char* env = getenv("FOF_HELPER_CPUS");
    if (env && *env) {
      fof_parse_cpulist(env, &safe, -1);
      how = "FOF_HELPER_CPUS";
      if (CPU_COUNT(&safe) == 0) {
        static bool warned_bad = false;
        if (!warned_bad) {
          warned_bad = true;
          fprintf(stderr, "[fofgpu] WARNING: FOF_HELPER_CPUS=\"%s\" parsed to "
                          "an empty set; falling back to SMT siblings\n", env);
        }
        how = "SMT siblings";
      }
    }
    if (CPU_COUNT(&safe) == 0 && !fof_helper_mask(&safe)) {
      // Nothing safe exists -- every CPU this process can see already has a PE
      // pinned to it.  That is the ppn 14 case.  DECLINE rather than guess,
      // but SAY SO: a silent decline reads as "the fix is working".
      static bool warned = false;
      if (!warned) {
        warned = true;
        fprintf(stderr,
                "[fofgpu] WARNING: affinity fix DECLINED -- no CPU is free of "
                "pinned PEs, so HIP helper threads will inherit this PE's core "
                "and preempt it in ~16 ms slices (see reports/relay42.txt). "
                "Either leave one CPU per process unpinned, or run with "
                "--core-spec=0 and set FOF_HELPER_CPUS to the reserved "
                "cores.\n");
      }
      return;
    }
    if (sched_setaffinity(0, sizeof safe, &safe) == 0) {
      active = true;
      static bool announced = false;
      if (!announced) {
        announced = true;
        fprintf(stderr, "[fofgpu] affinity fix active (%s): HIP helper threads "
                        "will inherit %d CPUs, not this PE's core\n",
                how, CPU_COUNT(&safe));
      }
    }
  }
  ~FoFHelperAffinityScope() {
    if (active) sched_setaffinity(0, sizeof save, &save);
  }
};

}  // namespace
#define FOF_HELPER_AFFINITY_SCOPE FoFHelperAffinityScope fof_aff_scope_
#else
#define FOF_HELPER_AFFINITY_SCOPE do {} while (0)
#endif

void Device::runPhase1Impl(float b2, float grid_thresh, bool async) {
  // See the block above: ROCm spawns its helpers from here, and they
  // inherit this PE's single-core mask unless we widen it first.
  FOF_HELPER_AFFINITY_SCOPE;
  p_->pending = WalkStats();
  p_->in_flight = false;
  WalkStats& st = p_->pending;
  // Measurement knob only: which shape the traversal takes. Read here
  // rather than plumbed through the interface because it selects between
  // two implementations of the SAME function, not between two behaviours
  // — both arms must produce identical labels, and the gate checks that.
  // Walk shape. NOT a fixed choice, because it is not independent of the
  // tree: the two shapes cross as leaf occupancy changes, and picking one
  // globally is what made both the stage-2 and the first stage-3 answers
  // wrong. Measured at 80M (device pass, max process):
  //
  //     mean leaf occupancy   solo     team
  //     3.7  (-l 12)          150 ms   308 ms
  //     8.3  (-l 32)          247 ms   175 ms
  //     16   (-l 64)          586 ms   125 ms
  //
  // Solo pays a serial leaf_size^2 pair product and wins only while that
  // is small; team pays a broadcast and a barrier per pop and wins as
  // soon as there is enough work per leaf to amortise them. The crossover
  // sits between 3.7 and 8.3, so the gate is 6. FOF_GPU_WALK=solo|team
  // forces either arm.
  const char* wenv = std::getenv("FOF_GPU_WALK");
  bool solo_walk;
  if (wenv != nullptr) {
    solo_walk = wenv[0] != 't';
  } else {
    solo_walk = true;  // decided below, once the leaf list exists
  }
  const bool walk_forced = wenv != nullptr;
  if (!p_->inited || p_->n == 0 || p_->n_nodes == 0) return;
  ExecSpace exec = p_->exec();
  Kokkos::Timer timer;

  // ---- upload the staged particles (K0's device half) ----
  // Must happen here, not in some earlier call: this is the only entry
  // point that reads them, and leaving the H2D copy to a sibling method
  // silently produced all-zero labels the first time (the orders were
  // never on the device at all).
  Kokkos::deep_copy(exec, p_->d_pos, p_->h_pos);
  Kokkos::deep_copy(exec, p_->d_order, p_->h_order);

  // ---- prepare: top tree, leaf list, union-find, certificate memo ----
  if (p_->h_top.empty()) {
    std::vector<int> idx(p_->n_pieces);
    for (int i = 0; i < p_->n_pieces; i++) idx[i] = i;
    p_->h_top.reserve(2 * p_->n_pieces);
    buildTopTree(p_->h_top, p_->h_nodes.data(), p_->h_piece_root.data(),
                 idx.data(), 0, p_->n_pieces);
    p_->n_top = (int)p_->h_top.size();
    p_->d_top = DView<DTopNode>(
        Kokkos::view_alloc(Kokkos::WithoutInitializing, "fof_d_top"),
        p_->n_top);
    Kokkos::View<const DTopNode*, Kokkos::HostSpace,
                 Kokkos::MemoryTraits<Kokkos::Unmanaged> >
        h(p_->h_top.data(), p_->n_top);
    Kokkos::deep_copy(exec, p_->d_top, h);
  }

  const long n_nodes = p_->n_nodes;
  const size_t n_part = p_->n;
  auto d_nodes = p_->d_nodes;
  auto d_top = p_->d_top;
  auto d_piece_root = p_->d_piece_root;

  // Leaf list, compacted by a SCAN rather than by an atomic counter.
  // Atomic compaction was the obvious way to write this and it is wrong
  // for the wrong reason: it is correct (the pair-once rule compares node
  // indices, not list positions) but it hands each wavefront 64 unrelated
  // leaves, so 64 lanes traverse 64 unrelated parts of the tree together.
  // Node order is spatially coherent — children are contiguous and the
  // particles under them are key-sorted — so preserving it gives lanes in
  // a wavefront overlapping descents. It also makes the launch
  // deterministic, which matters when chasing a mismatch.
  p_->d_leaves = DView<int>(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "fof_d_leaves"), n_nodes);
  auto d_leaves = p_->d_leaves;
  long n_leaves = 0;
  Kokkos::parallel_scan(
      "fof_leaf_list", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_nodes),
      KOKKOS_LAMBDA(const long i, long& acc, const bool final) {
        const bool is_leaf = d_nodes(i).child_begin < 0;
        if (is_leaf && final) d_leaves(acc) = (int)i;
        if (is_leaf) acc += 1;
      },
      n_leaves);

  p_->d_parent = DView<int>(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "fof_d_parent"), n_part);
  p_->d_node_rep = DView<int>(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "fof_d_node_rep"),
      n_nodes);
  auto d_parent = p_->d_parent;
  auto d_node_rep = p_->d_node_rep;
  auto d_pos = p_->d_pos;
  auto d_order = p_->d_order;
  auto d_label = p_->d_label;
  Kokkos::parallel_for(
      "fof_uf_init", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_part),
      KOKKOS_LAMBDA(const long i) { d_parent(i) = (int)i; });
  Kokkos::parallel_for(
      "fof_rep_init", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_nodes),
      KOKKOS_LAMBDA(const long i) { d_node_rep(i) = -1; });
  p_->d_grid_root = DView<int>(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "fof_d_grid_root"),
      n_nodes);
  auto d_grid_root = p_->d_grid_root;
  Kokkos::parallel_for(
      "fof_grid_root_init", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_nodes),
      KOKKOS_LAMBDA(const long i) { d_grid_root(i) = -1; });
  exec.fence();
  st.t_prepare = timer.seconds();
  st.n_leaves = n_leaves;
  st.n_top_nodes = p_->n_top;

  if (!walk_forced && n_leaves > 0)
    solo_walk = ((double)n_part / (double)n_leaves) < 6.0;
  st.walk_solo = solo_walk;
  st.leaf_occupancy = n_leaves > 0 ? (double)n_part / (double)n_leaves : 0.0;

  // ---- stage 3: the dense-node cell grid (design section 5, K3) ----
  //
  // A node dense enough that its own cell grid is cheaper than descending
  // it is solved OUTRIGHT, before the walk, and the walk then prunes the
  // whole node for every leaf inside it. Cell side c = b/sqrt(6) buys two
  // test-free guarantees: a same-cell pair is within b (diagonal
  // c*sqrt(3) = b/sqrt(2)) and a FACE-adjacent pair is within b (max
  // separation c*sqrt(6) = b). Only the residual stencil needs distance
  // tests, and only between cells not already in the same component.
  //
  // This is the same algorithm as the CPU's gridSelfUnionRange, but the
  // data structure is different in the one place it matters: the CPU
  // sorts particles by cell key and binary-searches an occupied-cell list
  // (the divergence its own comment flags), while here every dense node
  // gets a DENSE cell array, so a neighbour probe is a single load. That
  // is affordable precisely because the node is dense: the occupancy gate
  // says cells <= n_below / thresh.
  if (grid_thresh > 0.f) {
    timer.reset();
    Kokkos::Timer gt;
    // c in DOUBLE, from b2 in double, exactly as the CPU computes it.
    // The cell side is load-bearing at both ends and cannot be nudged
    // either way: too large and a face-adjacent pair can exceed b, so a
    // free union links a non-friend; too small and a pair within b can
    // land more than 3 cells apart, so the sum(((|d|-1)+)^2) <= 6 stencil
    // MISSES it. Only c = b/sqrt(6) satisfies both, so the two arms had
    // better round it the same way.
    const float cell = (float)(std::sqrt((double)b2) / std::sqrt(6.0));
    const float thresh = grid_thresh;

    // (1) Maximal dense nodes. `d_grid_root(i)` ends up holding the
    // topmost dense ancestor-or-self of i, or -1. Seeded at every dense
    // node and pushed DOWN: children have higher indices than parents, a
    // node has exactly one parent so no two threads write the same entry,
    // and an ancestor's value overwrites a descendant's — which is what
    // makes the fixpoint "maximal", not merely "some".
    const long cell_cap_slope = 16;   // cells <= 16*n_below + 64, else skip
    Kokkos::parallel_for(
        "fof_grid_mark", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_nodes),
        KOKKOS_LAMBDA(const long i) {
          const DDNode d = d_nodes(i);
          if (d.n_below < 64) return;
          const double vol = (double)(d.hi[0] - d.lo[0]) *
                             (double)(d.hi[1] - d.lo[1]) *
                             (double)(d.hi[2] - d.lo[2]);
          if (!(vol > 0.0)) return;
          const double cc = (double)cell;
          if ((double)d.n_below * cc * cc * cc / vol < (double)thresh) return;
          int dim[3];
          gridDims(d, cell, dim);
          const long cells = (long)dim[0] * dim[1] * dim[2];
          // A needle-shaped box has near-zero volume and passes the
          // occupancy gate on that alone while needing far more cells
          // than it has particles. Leave those to the walk.
          if (cells > cell_cap_slope * (long)d.n_below + 64) return;
          d_grid_root(i) = (int)i;
        });
    long changed = 1;
    for (int pass = 0; pass < 64 && changed != 0; pass++) {
      changed = 0;
      Kokkos::parallel_reduce(
          "fof_grid_push", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_nodes),
          KOKKOS_LAMBDA(const long i, long& acc) {
            const int g = d_grid_root(i);
            if (g < 0) return;
            const DDNode d = d_nodes(i);
            if (d.child_begin < 0) return;
            for (int c = 0; c < d.n_children; c++)
              if (d_grid_root(d.child_begin + c) != g) {
                d_grid_root(d.child_begin + c) = g;
                acc += 1;
              }
          },
          changed);
    }

    exec.fence();
    st.t_grid_mark = gt.seconds();
    gt.reset();

    // (2) Compact the maximal dense nodes and lay out their particle and
    // cell blocks with two scans.
    DView<int> d_gnode(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                          "fof_d_gnode"), n_nodes);
    long n_grid = 0;
    Kokkos::parallel_scan(
        "fof_grid_list", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_nodes),
        KOKKOS_LAMBDA(const long i, long& acc, const bool final) {
          const bool mine = d_grid_root(i) == (int)i;
          if (mine && final) d_gnode(acc) = (int)i;
          if (mine) acc += 1;
        },
        n_grid);

    st.grid_nodes = n_grid;
    if (n_grid > 0) {
      DView<long> d_gpoff(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                             "fof_d_gpoff"), n_grid + 1);
      DView<long> d_gcoff(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                             "fof_d_gcoff"), n_grid + 1);
      long n_gpart = 0, n_gcell = 0;
      Kokkos::parallel_scan(
          "fof_grid_poff", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_grid),
          KOKKOS_LAMBDA(const long g, long& acc, const bool final) {
            if (final) d_gpoff(g) = acc;
            acc += d_nodes(d_gnode(g)).n_below;
          },
          n_gpart);
      Kokkos::parallel_scan(
          "fof_grid_coff", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_grid),
          KOKKOS_LAMBDA(const long g, long& acc, const bool final) {
            if (final) d_gcoff(g) = acc;
            int dim[3];
            gridDims(d_nodes(d_gnode(g)), cell, dim);
            acc += (long)dim[0] * dim[1] * dim[2];
          },
          n_gcell);
      Kokkos::parallel_for(
          "fof_grid_tail", Kokkos::RangePolicy<ExecSpace>(exec, 0, 1),
          KOKKOS_LAMBDA(const int) {
            d_gpoff(n_grid) = n_gpart;
            d_gcoff(n_grid) = n_gcell;
          });
      st.grid_particles = n_gpart;
      st.grid_cells = n_gcell;

      // (3) Bin. FLAT over grid particles and over cells, not one team per
      // dense node: the dense nodes span four orders of magnitude in size
      // (a 64-particle clump and a 500k-particle halo core are both one
      // node), so a team-per-node kernel gives the halo core 64 threads
      // and the clump 64 threads. Measured, that imbalance cost 227 ms on
      // the worst process against 43 ms on the best with the SAME particle
      // count — it was the whole reason the grid did not pay for itself.
      //
      // The flat form needs a particle -> dense-node map, which is one
      // scan away: the blocks are contiguous, so marking each block's
      // first slot and running an inclusive sum labels every slot with
      // its block. Same trick for the cell blocks.
      DView<int> d_gpart(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                            "fof_d_gpart"), n_gpart);
      DView<int> d_gcell(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                            "fof_d_gcell"), n_gpart);
      DView<int> d_qnode("fof_d_qnode", n_gpart);  // zero-filled
      DView<int> d_cnode("fof_d_cnode", n_gcell);  // zero-filled
      DView<int> d_ccount("fof_d_ccount", n_gcell + 1);  // zero-filled
      Kokkos::parallel_for(
          "fof_grid_seg", Kokkos::RangePolicy<ExecSpace>(exec, 1, n_grid),
          KOKKOS_LAMBDA(const long g) {
            d_qnode(d_gpoff(g)) = 1;
            d_cnode(d_gcoff(g)) = 1;
          });
      Kokkos::parallel_scan(
          "fof_grid_qseg", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_gpart),
          KOKKOS_LAMBDA(const long q, int& acc, const bool final) {
            acc += d_qnode(q);
            if (final) d_qnode(q) = acc;
          });
      Kokkos::parallel_scan(
          "fof_grid_cseg", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_gcell),
          KOKKOS_LAMBDA(const long c, int& acc, const bool final) {
            acc += d_cnode(c);
            if (final) d_cnode(c) = acc;
          });
      Kokkos::parallel_for(
          "fof_grid_bin", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_gpart),
          KOKKOS_LAMBDA(const long q) {
            const int g = d_qnode(q);
            const DDNode d = d_nodes(d_gnode(g));
            int dim[3];
            gridDims(d, cell, dim);
            const int p = d.part_begin + (int)(q - d_gpoff(g));
            int ix[3];
            for (int k = 0; k < 3; k++) {
              int v = (int)((d_pos(3 * p + k) - d.lo[k]) / cell);
              if (v < 0) v = 0;
              if (v >= dim[k]) v = dim[k] - 1;
              ix[k] = v;
            }
            const long c = d_gcoff(g) +
                           (long)(ix[2] * dim[1] + ix[1]) * dim[0] + ix[0];
            d_gpart(q) = p;
            d_gcell(q) = (int)c;
            Kokkos::atomic_fetch_add(&d_ccount(c), 1);
          });

      // (4) Counting sort into per-cell particle lists.
      DView<long> d_cbegin(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                              "fof_d_cbegin"), n_gcell + 1);
      long total = 0;
      Kokkos::parallel_scan(
          "fof_grid_cscan", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_gcell),
          KOKKOS_LAMBDA(const long c, long& acc, const bool final) {
            if (final) d_cbegin(c) = acc;
            acc += d_ccount(c);
          },
          total);
      Kokkos::parallel_for(
          "fof_grid_ctail", Kokkos::RangePolicy<ExecSpace>(exec, 0, 1),
          KOKKOS_LAMBDA(const int) { d_cbegin(n_gcell) = n_gpart; });
      DView<int> d_cursor("fof_d_cursor", n_gcell);  // zero-filled
      DView<int> d_citem(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                            "fof_d_citem"), n_gpart);
      Kokkos::parallel_for(
          "fof_grid_scatter", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_gpart),
          KOKKOS_LAMBDA(const long q) {
            const long c = d_gcell(q);
            const int at = Kokkos::atomic_fetch_add(&d_cursor(c), 1);
            d_citem(d_cbegin(c) + at) = d_gpart(q);
          });

      exec.fence();
      st.t_grid_bin = gt.seconds();
      gt.reset();

      // (5) Same-cell cliques: every particle unites with its cell's
      // first item. Test-free — the cell diagonal is b/sqrt(2).
      Kokkos::parallel_for(
          "fof_grid_clique", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_gpart),
          KOKKOS_LAMBDA(const long q) {
            const long c = d_gcell(q);
            const int rep = d_citem(d_cbegin(c));
            const int p = d_gpart(q);
            if (p != rep) ufUnite(d_parent, d_order, rep, p);
          });

      exec.fence();
      st.t_grid_union = gt.seconds();
      gt.reset();

      // (6) Neighbour pass over the forward-half stencil, one thread per
      // occupied cell. Face-adjacent neighbours are test-free; the rest
      // are skipped outright when the two cells are already in the same
      // component, and otherwise stop at the FIRST witness (the cliques
      // already made each cell one component, so one link merges them).
      const std::vector<int> offs_h = gridOffsetTable();
      const int n_off = (int)offs_h.size() / 3;
      DView<int> d_offs(Kokkos::view_alloc(Kokkos::WithoutInitializing,
                                           "fof_d_offs"), offs_h.size());
      {
        Kokkos::View<const int*, Kokkos::HostSpace,
                     Kokkos::MemoryTraits<Kokkos::Unmanaged> >
            h(offs_h.data(), offs_h.size());
        Kokkos::deep_copy(exec, d_offs, h);
      }
      long probes = 0;
      Kokkos::parallel_reduce(
          "fof_grid_nbr", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_gcell),
          KOKKOS_LAMBDA(const long c, long& acc) {
            const long begin = d_cbegin(c), end = d_cbegin(c + 1);
            if (begin >= end) return;
            const long g = d_cnode(c);
            const DDNode d = d_nodes(d_gnode(g));
            int dim[3];
            gridDims(d, cell, dim);
            const long cbase = d_gcoff(g);
            long loc = c - cbase;
            const int ix = (int)(loc % dim[0]);
            loc /= dim[0];
            const int iy = (int)(loc % dim[1]);
            const int iz = (int)(loc / dim[1]);
            const int rep = d_citem(begin);
            // NOTE: ufFind(rep) below is deliberately re-evaluated inside
            // the offset loop. Hoisting it out is SAFE (a stale root that
            // matches still proves connectivity, since union-find is
            // monotone and ufFind only returns current roots, so staleness
            // can only cost redundant tests, never skip a real one) — and
            // it cost 4-7x, 13-97 ms of stencil going to 138-679 ms. The
            // early-out is not overhead around the real work; it IS the
            // work. Once the face-adjacent unions merge a halo, a stale
            // root stops matching almost every time, and the full
            // cell-by-cell distance product runs for all ~157 offsets.
            for (int o = 0; o < n_off; o++) {
              const int dx = d_offs(3 * o), dy = d_offs(3 * o + 1),
                        dz = d_offs(3 * o + 2);
              const int jx = ix + dx, jy = iy + dy, jz = iz + dz;
              if (jx < 0 || jx >= dim[0] || jy < 0 || jy >= dim[1] ||
                  jz < 0 || jz >= dim[2])
                continue;
              const long nc =
                  cbase + (long)(jz * dim[1] + jy) * dim[0] + jx;
              const long nb0 = d_cbegin(nc), nb1 = d_cbegin(nc + 1);
              if (nb0 >= nb1) continue;
              const int nrep = d_citem(nb0);
              const int ad = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) +
                             (dz < 0 ? -dz : dz);
              if (ad == 1) {  // face-adjacent: separation <= c*sqrt(6) = b
                ufUnite(d_parent, d_order, rep, nrep);
                continue;
              }
              if (ufFind(d_parent, rep) == ufFind(d_parent, nrep)) continue;
              acc += 1;
              bool merged = false;
              for (long a = begin; a < end && !merged; a++) {
                const int pa = d_citem(a);
                const float xa = d_pos(3 * pa), ya = d_pos(3 * pa + 1),
                            za = d_pos(3 * pa + 2);
                for (long bq = nb0; bq < nb1; bq++) {
                  const int pb = d_citem(bq);
                  const float ddx = xa - d_pos(3 * pb);
                  const float ddy = ya - d_pos(3 * pb + 1);
                  const float ddz = za - d_pos(3 * pb + 2);
                  if (ddx * ddx + ddy * ddy + ddz * ddz <= b2) {
                    ufUnite(d_parent, d_order, pa, pb);
                    merged = true;  // one witness merges the components
                    break;
                  }
                }
              }
            }
          },
          probes);
      st.grid_probes = probes;
      exec.fence();
      st.t_grid_nbr = gt.seconds();
    }
    exec.fence();
    st.t_grid = timer.seconds();
  }

  // ---- the walk ----
  // TWO SHAPES, one body (see Wave/Solo and walkOneLeaf above).
  //
  // Stage 2 replaced thread-per-leaf with team-per-leaf and measured
  // 850 -> 516 ms, on the evidence that the serial per-thread leaf-pair
  // product dominated. Stage 3's suppression then deleted ~40x of the
  // work behind each leaf, so each traversal now does far fewer pops with
  // far less work in each — while the team still pays a broadcast and a
  // barrier on every pop. FOF_GPU_WALK=solo runs the same traversal one
  // thread per leaf so that conclusion can be re-tested rather than
  // assumed; see design/phase1-gpu.md section 16.
  timer.reset();
  // Impl-owned, not function-local: on the async path this call returns
  // while the walk is still incrementing it, and a View destroyed here
  // would free device memory out from under a running kernel.
  p_->counters = Kokkos::View<long*, MemSpace>("fof_counters", 4);
  auto counters = p_->counters;
  Kokkos::deep_copy(exec, counters, 0L);
  const int kStack = 128;
  auto d_leaves_k = d_leaves;
  auto d_top_k = p_->d_top;
  auto d_piece_root_k = d_piece_root;
  auto d_grid_root_k = d_grid_root;
  if (solo_walk) {
    // Private stack, so no scratch and no collectives. 128 ints is 512 B
    // of scratch per thread, which is exactly the occupancy cost this arm
    // exists to weigh against the barrier cost of the team arm.
    Kokkos::parallel_for(
        "fof_walk_solo", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_leaves),
        KOKKOS_LAMBDA(const long k) {
          int stack[128];
          int sp[2];
          walkOneLeaf(Solo(), d_leaves_k(k), stack, sp, kStack, b2, d_nodes,
                      d_top_k, d_piece_root_k, d_parent, d_order, d_node_rep,
                      d_grid_root_k, d_pos, counters);
        });
  } else {
    // The stack lives in team scratch (LDS) once per team rather than once
    // per lane: 64 identical copies in private memory would be 32 KB per
    // wavefront and would wreck occupancy. Lane 0 owns push/pop and
    // broadcasts, which is what keeps the shared stack consistent.
    const int kTeam = 64;  // MI250X wavefront; a warp on NVIDIA is 32
    using policy_t = Kokkos::TeamPolicy<ExecSpace>;
    using member_t = policy_t::member_type;
    const size_t shmem = (size_t)kStack * sizeof(int) + 2 * sizeof(int);
    policy_t policy(exec, (int)n_leaves, kTeam);
    policy.set_scratch_size(0, Kokkos::PerTeam((int)shmem));
    Kokkos::parallel_for(
        "fof_walk", policy, KOKKOS_LAMBDA(const member_t& team) {
          int* stack = (int*)team.team_shmem().get_shmem(kStack * sizeof(int));
          int* sp = (int*)team.team_shmem().get_shmem(2 * sizeof(int));
          walkOneLeaf(Wave<member_t>{team}, d_leaves_k(team.league_rank()),
                      stack, sp, kStack, b2, d_nodes, d_top_k, d_piece_root_k,
                      d_parent, d_order, d_node_rep, d_grid_root_k, d_pos,
                      counters);
        });
  }
  // The ONLY difference between the two forms, and it is three fences.
  // Everything above this point is identical, which is what makes the
  // synchronous form a usable oracle for the asynchronous one: the same
  // kernels, in the same order, on the same stream.
  if (!async) {
    exec.fence();
    st.t_walk = timer.seconds();
  }

  // ---- freeze: full path compression + the tip write ----
  timer.reset();
  Kokkos::parallel_for(
      "fof_freeze", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_part),
      KOKKOS_LAMBDA(const long i) {
        d_label(i) = d_order(ufFind(d_parent, (int)i));
      });
  if (!async) {
    exec.fence();
    st.t_freeze = timer.seconds();
  }

  timer.reset();
  Kokkos::deep_copy(exec, p_->h_label, d_label);
  if (!async) {
    exec.fence();
    st.t_download = timer.seconds();
  }
  p_->in_flight = true;
}

void Device::enqueueRoundTrip() {
  if (!p_->inited || p_->n == 0) return;
  const size_t n = p_->n;
  ExecSpace exec = p_->exec();
  auto d_order = p_->d_order;
  auto d_label = p_->d_label;
  // Every operation takes the exec-space instance explicitly, so all of
  // them land on the HAPI stream and nothing here fences. Kokkos
  // deep_copy with an execution space argument is asynchronous.
  Kokkos::deep_copy(exec, p_->d_pos, p_->h_pos);
  Kokkos::deep_copy(exec, d_order, p_->h_order);
  Kokkos::parallel_for(
      "fof_stage0_identity_async", Kokkos::RangePolicy<ExecSpace>(exec, 0, n),
      KOKKOS_LAMBDA(const size_t i) { d_label(i) = d_order(i); });
  Kokkos::deep_copy(exec, p_->h_label, d_label);
}

void Device::roundTrip(RoundTripStats* out) {
  RoundTripStats st;
  st.n = static_cast<long>(p_->n);
  if (!p_->inited || p_->n == 0) {
    if (out) *out = st;
    return;
  }
  const size_t n = p_->n;
  ExecSpace exec = p_->exec();

  auto d_pos = p_->d_pos;
  auto d_order = p_->d_order;
  auto d_label = p_->d_label;

  Kokkos::Timer timer;
  Kokkos::deep_copy(exec, d_pos, p_->h_pos);
  Kokkos::deep_copy(exec, d_order, p_->h_order);
  exec.fence();
  st.t_upload = timer.seconds();

  timer.reset();
  // Identity pass: proves the device wrote something the host can check
  // particle by particle, and is the exact shape K4's label write will
  // have (one store per particle from a value the device computed).
  Kokkos::parallel_for(
      "fof_stage0_identity", Kokkos::RangePolicy<ExecSpace>(exec, 0, n),
      KOKKOS_LAMBDA(const size_t i) { d_label(i) = d_order(i); });

  // Bounding box + checksum. Real work — the grid origin the later
  // stages need — and independently computable on the host, which is
  // what makes this a gate.
  Kokkos::MinMaxScalar<float> mm[3];
  for (int axis = 0; axis < 3; axis++) {
    Kokkos::MinMaxScalar<float> r;
    Kokkos::parallel_reduce(
        "fof_stage0_bbox", Kokkos::RangePolicy<ExecSpace>(exec, 0, n),
        KOKKOS_LAMBDA(const size_t i, Kokkos::MinMaxScalar<float>& acc) {
          const float v = d_pos(3 * i + axis);
          if (v < acc.min_val) acc.min_val = v;
          if (v > acc.max_val) acc.max_val = v;
        },
        Kokkos::MinMax<float>(r));
    mm[axis] = r;
  }
  double checksum = 0;
  Kokkos::parallel_reduce(
      "fof_stage0_checksum", Kokkos::RangePolicy<ExecSpace>(exec, 0, n),
      KOKKOS_LAMBDA(const size_t i, double& acc) {
        acc += static_cast<double>(d_pos(3 * i)) +
               static_cast<double>(d_pos(3 * i + 1)) +
               static_cast<double>(d_pos(3 * i + 2));
      },
      checksum);
  exec.fence();
  st.t_kernel = timer.seconds();
  for (int axis = 0; axis < 3; axis++) {
    st.box_lo[axis] = mm[axis].min_val;
    st.box_hi[axis] = mm[axis].max_val;
  }
  st.checksum = checksum;

  timer.reset();
  Kokkos::deep_copy(exec, p_->h_label, d_label);
  exec.fence();
  st.t_download = timer.seconds();

  if (out) *out = st;
}

}  // namespace fofgpu
