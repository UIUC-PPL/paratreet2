// Kokkos implementation of the device side of FoF phase 1
// (design/phase1-gpu.md). Compiled by hipcc (or nvcc_wrapper), NEVER by
// charmc: no Charm header is included here, and FoFDevice.h names no
// Kokkos type, so the two toolchains meet only at a POD interface.

#include "FoFDevice.h"

#include <Kokkos_Core.hpp>

#include <cstring>
#include <cstdio>
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
  long n_leaves = 0;
  int n_top = 0;
  std::vector<DTopNode> h_top;
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

void Device::runPhase1(float b2, WalkStats* out) {
  WalkStats st;
  if (!p_->inited || p_->n == 0 || p_->n_nodes == 0) {
    if (out) *out = st;
    return;
  }
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
  exec.fence();
  st.t_prepare = timer.seconds();
  st.n_leaves = n_leaves;
  st.n_top_nodes = p_->n_top;

  // ---- the walk ----
  // ONE TEAM (wavefront) PER LEAF, not one thread.
  //
  // The first version ran one thread per leaf and was ~2x the CPU rather
  // than the ~10x the plan projected. The leaf-size sweep said why: 12 ->
  // 32 -> 64 particles per leaf made the kernel 2.3x then 4.5x SLOWER,
  // the opposite of the expected direction, which can only happen if the
  // per-thread leaf-pair product (leaf_size^2, run serially by one lane)
  // dominates. Both halves of that are fixed by putting the wavefront on
  // the work instead of on independent leaves:
  //
  //   - every lane runs the SAME traversal on the SAME stack, so the node
  //     loads are one broadcast per team instead of 64 scattered gathers;
  //   - the leaf-pair cross product and the certificate star-union are
  //     split across the 64 lanes and read consecutive particles, so they
  //     coalesce.
  //
  // The stack lives in team scratch (LDS) once per team rather than once
  // per lane: 64 identical copies in private memory would be 32 KB per
  // wavefront and would wreck occupancy. Lane 0 owns push/pop and
  // broadcasts, which is what keeps the shared stack consistent.
  timer.reset();
  Kokkos::View<long*, MemSpace> counters("fof_counters", 3);
  Kokkos::deep_copy(exec, counters, 0L);
  const int kStack = 128;
  const int kTeam = 64;  // MI250X wavefront; a warp on NVIDIA is 32
  using policy_t = Kokkos::TeamPolicy<ExecSpace>;
  using member_t = policy_t::member_type;
  const size_t shmem = (size_t)kStack * sizeof(int) + 2 * sizeof(int);
  policy_t policy(exec, (int)n_leaves, kTeam);
  policy.set_scratch_size(0, Kokkos::PerTeam((int)shmem));
  Kokkos::parallel_for(
      "fof_walk", policy, KOKKOS_LAMBDA(const member_t& team) {
        const int li = d_leaves(team.league_rank());
        const DDNode L = d_nodes(li);

        int* stack = (int*)team.team_shmem().get_shmem(kStack * sizeof(int));
        int* sp = (int*)team.team_shmem().get_shmem(2 * sizeof(int));
        Kokkos::single(Kokkos::PerTeam(team), [&]() {
          stack[0] = -1;  // top-tree root
          sp[0] = 1;
        });
        team.team_barrier();

        while (true) {
          // Pop once for the whole team and broadcast; INT_MIN = done.
          int e = 0;
          Kokkos::single(Kokkos::PerTeam(team),
                         [&](int& out) {
                           out = (sp[0] > 0) ? stack[--sp[0]] : INT_MIN;
                         },
                         e);
          if (e == INT_MIN) break;

          if (e < 0) {  // top tree
            const DTopNode t = d_top(-e - 1);
            if (boxMinDist2(L.lo, L.hi, t.lo, t.hi) <= b2) {
              Kokkos::single(Kokkos::PerTeam(team), [&]() {
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
            team.team_barrier();
            continue;
          }

          const DDNode M = d_nodes(e);
          if (boxMinDist2(L.lo, L.hi, M.lo, M.hi) > b2) {
            team.team_barrier();
            continue;
          }

          // Positive certificate: every cross pair is a friend, so both
          // sides collapse with no distance test and no descent. The memo
          // is CLAIMED with a CAS, not merely checked: thousands of teams
          // can reach a hot node at once, and without the claim they all
          // run the same O(n_below) star-union.
          if (boxMaxDist2(L.lo, L.hi, M.lo, M.hi) <= b2) {
            teamStarUnion(team, d_nodes, d_parent, d_order, d_node_rep, li);
            if (e != li) {
              teamStarUnion(team, d_nodes, d_parent, d_order, d_node_rep, e);
              Kokkos::single(Kokkos::PerTeam(team), [&]() {
                // Uniting the two FIRST particles is enough: whoever won
                // each claim is committed to collapsing that node, and
                // union-find is order-independent, so it does not matter
                // which finishes first.
                ufUnite(d_parent, d_order, L.part_begin, M.part_begin);
                Kokkos::atomic_fetch_add(&counters(1), 1L);
              });
            }
            team.team_barrier();
            continue;
          }

          if (M.child_begin < 0) {
            // Leaf pair, owned by the smaller node index so the mirror
            // visit from M's own team does nothing (the structural
            // each-pair-once rule).
            if (e >= li) {
              const int ab = L.part_begin, bb = M.part_begin;
              const int nb_ = (e == li) ? L.n_below : M.n_below;
              // Flatten the product over the lanes: consecutive lanes
              // take consecutive j, so the inner reads of the partner's
              // positions coalesce.
              Kokkos::parallel_for(
                  Kokkos::TeamThreadRange(team, L.n_below), [&](const int i) {
                    const float xi = d_pos(3 * (ab + i));
                    const float yi = d_pos(3 * (ab + i) + 1);
                    const float zi = d_pos(3 * (ab + i) + 2);
                    const int j0 = (e == li) ? i + 1 : 0;
                    for (int j = j0; j < nb_; j++) {
                      const int p = (e == li) ? ab + j : bb + j;
                      const float dx = xi - d_pos(3 * p);
                      const float dy = yi - d_pos(3 * p + 1);
                      const float dz = zi - d_pos(3 * p + 2);
                      if (dx * dx + dy * dy + dz * dz <= b2)
                        ufUnite(d_parent, d_order, ab + i, p);
                    }
                  });
              Kokkos::single(Kokkos::PerTeam(team), [&]() {
                Kokkos::atomic_fetch_add(&counters(2), 1L);
              });
            }
            team.team_barrier();
            continue;
          }

          Kokkos::single(Kokkos::PerTeam(team), [&]() {
            if (sp[0] + M.n_children <= kStack) {
              for (int c = 0; c < M.n_children; c++)
                stack[sp[0]++] = M.child_begin + c;
            } else {
              Kokkos::atomic_fetch_add(&counters(0), 1L);
            }
          });
          team.team_barrier();
        }
      });
  exec.fence();
  st.t_walk = timer.seconds();

  // ---- freeze: full path compression + the tip write ----
  timer.reset();
  Kokkos::parallel_for(
      "fof_freeze", Kokkos::RangePolicy<ExecSpace>(exec, 0, n_part),
      KOKKOS_LAMBDA(const long i) {
        d_label(i) = d_order(ufFind(d_parent, (int)i));
      });
  exec.fence();
  st.t_freeze = timer.seconds();

  timer.reset();
  Kokkos::deep_copy(exec, p_->h_label, d_label);
  exec.fence();
  st.t_download = timer.seconds();

  auto h_counters = Kokkos::create_mirror_view(counters);
  Kokkos::deep_copy(h_counters, counters);
  st.stack_overflows = h_counters(0);
  st.certificates = h_counters(1);
  st.leaf_pairs = h_counters(2);
  if (out) *out = st;
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
