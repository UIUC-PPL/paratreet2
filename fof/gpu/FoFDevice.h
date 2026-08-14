#ifndef PARATREET_FOF_DEVICE_H_
#define PARATREET_FOF_DEVICE_H_

// Device-side FoF phase 1 (design/phase1-gpu.md). THIS HEADER IS THE
// CHARM/KOKKOS FIREWALL: it names neither, so FoFPhase1.h (heavy Charm
// templates) is never seen by hipcc and FoFDevice.cpp never sees a Charm
// header. Same discipline as tests/treecache's plain-compiler gate.
//
// Stage 0 scope (design/phase1-gpu.md section 8): device selection,
// pinned staging buffers the process's PEs fill in parallel, a
// host->device->host round trip, and the stream bridge to Charm's HAPI.
// No union-find, no traversal yet — those are stages 2 and 3.
//
// Threading contract: one Device per PROCESS, owned by the FoF nodegroup
// branch (one process per GPU is an enforced invariant, see checkMapping).
// resize()/roundTrip()/init() are called by the owning PE only; the
// host*() buffers may then be filled concurrently by the process's PEs,
// each writing its own disjoint slice.

#include <cstddef>

namespace fofgpu {

// What the device library was compiled against and what it bound to.
struct DeviceInfo {
  int    device_id = -1;
  int    n_visible = 0;          // devices visible to this process
  char   backend[32] = {0};      // Kokkos::DefaultExecutionSpace::name()
  char   name[128] = {0};        // GPU name, empty on a host backend
  size_t total_global_mem = 0;   // bytes, 0 on a host backend
  bool   is_device_backend = false;
};

// Per-step walls of one round trip, plus the values the device computed.
// The bounding box is real work the later stages need (grid origin), and
// it is checkable against a host computation — that is what makes this a
// gate rather than a smoke test.
// Mirror of paratreet::DNode (src/DeviceTree.h). Duplicated rather than
// included because this header must not pull in Charm/paratreet headers —
// the layout is asserted identical on the host side at upload.
struct DDNode {
  float lo[3], hi[3];
  int part_begin;
  int n_below;
  int child_begin;
  int n_children;
};

// Result of the stage-1 tree upload check.
struct TreeStats {
  long n_nodes = 0;
  long n_pieces = 0;
  double t_upload = 0;
  double t_check = 0;
  long particles_under_roots = 0;  // device-summed: must equal the staged n
  long bad_nodes = 0;              // device-counted structural violations
};

// Stage 2: the device union-find pass (design/phase1-gpu.md section 5,
// K1-K4). Replaces phaseA + phaseB + merge for the process.
struct WalkStats {
  long n_leaves = 0;
  long n_top_nodes = 0;
  double t_prepare = 0;   // leaf list, top tree, parent init
  double t_grid = 0;      // stage 3: the dense-node cell-grid pre-pass
  // ... split four ways, because the total alone said nothing useful:
  // it varied 2.9x across processes with the same cell count.
  double t_grid_mark = 0;   // dense gate + maximal-dense-ancestor fixpoint
  double t_grid_bin = 0;    // layout scans, binning, counting sort
  double t_grid_union = 0;  // same-cell cliques
  double t_grid_nbr = 0;    // forward-half stencil pass
  double t_walk = 0;      // the traversal itself
  double t_freeze = 0;    // find-all + label write
  double t_download = 0;
  // Async form only (enqueuePhase1): walk + freeze + download are one
  // unfenced sequence, so they are one number, measured in the completion
  // callback. Zero on the synchronous path, where the three above are real.
  double t_device_tail = 0;
  long stack_overflows = 0;   // must be 0; a nonzero count means LOST WORK
  long certificates = 0;      // positive certificates fired
  long leaf_pairs = 0;        // leaf-leaf pair tests performed
  // Stage 3 (design/phase1-gpu.md section 15).
  long suppressed = 0;        // pairs pruned by connectivity suppression
  long grid_nodes = 0;        // maximal dense nodes solved by the grid
  long grid_particles = 0;    // particles they cover
  long grid_cells = 0;        // cells allocated for them
  long grid_probes = 0;       // residual-stencil cell pairs distance-tested
  bool walk_solo = false;     // true = one thread per leaf (FOF_GPU_WALK)
  double leaf_occupancy = 0;  // particles/leaf, which is what picks the shape
};

struct RoundTripStats {
  long   n = 0;
  double t_upload = 0;
  double t_kernel = 0;
  double t_download = 0;
  float  box_lo[3] = {0, 0, 0};
  float  box_hi[3] = {0, 0, 0};
  double checksum = 0;           // sum of (x+y+z), device-computed
};

class Device {
 public:
  // True when a real device backend is compiled in. A host-backend build
  // (Kokkos Serial) still works end to end — that is the section-7 gate 6
  // arm, and the reason this returns a value instead of failing to link.
  static bool available();
  static const char* backendName();

  Device();
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  // device_id comes from Charm's hapiGetDevice() — HAPI has already done
  // the PE->GPU mapping, so this library never guesses. Idempotent;
  // initializes Kokkos if no one else has.
  void init(int device_id);
  void finalize();
  bool initialized() const;
  DeviceInfo info() const;

  // 1:1 process/GPU invariant (design/phase1-gpu.md section 2). Returns
  // true when n_procs_on_node == visible devices * (procs sharing this
  // device must be 1). Caller aborts with its own message.
  bool checkMapping(int n_procs_on_physical_node) const;

  // Bridge to a Charm HAPI stream (hapiGetStream()), passed as void* so
  // this header stays free of hip/cuda types. null restores the default
  // execution space. Kernel launches and copies then ride that stream, so
  // hapiAddCallback() on the same stream delivers completion as a Charm
  // callback.
  void setStream(void* stream);

  // Staging. resize() allocates pinned host + device buffers for n
  // particles; the host buffers are then filled slice-wise by the
  // process's PEs (positions xyz-interleaved, orders one per particle).
  void   resize(size_t n);
  size_t size() const;
  float* hostPositions();   // 3n floats
  long*  hostOrders();      // n
  long*  hostLabels();      // n, written by roundTrip()

  // Upload this process's flat trees as one concatenated array
  // (design/phase1-gpu.md stage 1). `piece_root` holds, per TreePiece,
  // the index of its root in the concatenated array; `piece_part_base`
  // holds the offset of its particle block in the process-flat particle
  // space, so a node's particles are
  // [piece_part_base + part_begin, + n_below).
  // Two-step, because the concatenation is the expensive half and it
  // parallelizes: resizeTree() allocates PINNED host buffers, the
  // process's PEs then fill disjoint slices of them (each PE copies and
  // rebases its OWN pieces), and uploadTree() copies the finished array
  // to the device and checks it there. Doing the concatenation on one PE
  // measured 236 ms of a 248 ms total at 80M — see design/phase1-gpu.md
  // section 13.
  void    resizeTree(long n_nodes, int n_pieces);
  DDNode* hostNodes();       // n_nodes, pinned
  int*    hostPieceRoot();   // n_pieces
  int*    hostPieceBase();   // n_pieces
  void    uploadTree(TreeStats* out);

  // Stage 0: upload, one identity pass (labels[i] = orders[i]) plus a
  // bounding-box and checksum reduction, download. Synchronous.
  void roundTrip(RoundTripStats* out);

  // The same round trip WITHOUT the reductions and WITHOUT a fence:
  // everything is enqueued on the stream set by setStream() and the call
  // returns immediately. The caller then arms hapiAddCallback() on that
  // same stream, so completion arrives as a Charm callback and no
  // scheduler thread ever blocks — the mechanism the phase-1 deposit
  // chain will use (design/phase1-gpu.md section 0). hostLabels() is
  // valid only after that callback fires.
  void enqueueRoundTrip();

  // Stage 2: whole-process FoF on the device. Reads the staged positions
  // and orders and the uploaded tree; writes hostLabels() with each
  // particle's TIP (the global order of its component's minimum-order
  // member) — the same value the CPU chain writes into
  // Particle::group_number after phaseA+phaseB+merge+relabel.
  //
  // Synchronous, with a fence between kernels so the per-kernel walls are
  // real. This is the MEASUREMENT form; production uses the
  // enqueue/complete pair below.
  //
  // grid_thresh is the CPU's -G occupancy gate (expected particles per
  // cell of side b/sqrt(6)); <= 0 disables the stage-3 grid pre-pass and
  // leaves the traversal to solve dense nodes by descent.
  void runPhase1(float b2, float grid_thresh, WalkStats* out);

  // Stage 4: the same pass, split so no scheduler thread waits on the GPU.
  //
  // enqueuePhase1() runs the prepare (which CANNOT be asynchronous — the
  // leaf-list parallel_scan returns its count to the host, and both the
  // walk's launch bounds and the shape choice depend on it) and then
  // ENQUEUES the walk, the freeze and the download on the stream set by
  // setStream(), returning with all of it in flight. The caller then arms
  // hapiAddCallback() on that same stream; when the callback fires,
  // completePhase1() fences, reads back the device counters and fills in
  // the stats. hostLabels() is valid only after completePhase1().
  //
  // Either call may come from ANY PE of the owning process (HAPI binds
  // every PE of the process to the same device at startup), but only one
  // at a time: the pair shares Impl state, and the deposit chain that
  // drives it already guarantees a single caller.
  void enqueuePhase1(float b2, float grid_thresh);
  void completePhase1(WalkStats* out);

  // Release everything the pass no longer needs: the pinned host staging
  // (positions, orders, the concatenated node array) and the device-side
  // scratch (union-find parent, certificate memo, leaf list, grid roots).
  // hostLabels() SURVIVES, because the scatter reads it after this.
  //
  // Phase 1 is a small slice of the iteration and everything above is dead
  // for the rest of it, but it stays RESIDENT — ~600 MB per process of it
  // pinned, which is not ordinary memory: it is unpageable and registered
  // with the driver, on nodes where the phase-3 node cache and libfabric's
  // MR cache are both competing for the same pages (design/phase1-gpu.md
  // section 17.3). The next iteration's resize()/resizeTree() reallocate.
  void releaseStaging();

  void fence();

 private:
  // One body behind runPhase1() and enqueuePhase1(); `async` selects only
  // whether the trailing fences are taken.
  void runPhase1Impl(float b2, float grid_thresh, bool async);

  struct Impl;
  Impl* p_;
};

}  // namespace fofgpu

#endif  // PARATREET_FOF_DEVICE_H_
