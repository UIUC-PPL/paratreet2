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

  void fence();

 private:
  struct Impl;
  Impl* p_;
};

}  // namespace fofgpu

#endif  // PARATREET_FOF_DEVICE_H_
