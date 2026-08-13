// Kokkos implementation of the device side of FoF phase 1
// (design/phase1-gpu.md). Compiled by hipcc (or nvcc_wrapper), NEVER by
// charmc: no Charm header is included here, and FoFDevice.h names no
// Kokkos type, so the two toolchains meet only at a POD interface.

#include "FoFDevice.h"

#include <Kokkos_Core.hpp>

#include <cstring>
#include <cstdio>

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
  // Each process must see (or be bound to) its own GPU. Two shapes are
  // acceptable: the launcher gave this process exactly one device
  // (n_visible == 1, the --gpus-per-task case), or all devices are
  // visible and there are exactly as many processes as devices.
  if (di.n_visible == 1) return true;
  return n_procs_on_physical_node == di.n_visible;
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
