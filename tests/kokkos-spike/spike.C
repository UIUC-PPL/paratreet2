// Stage-0 integration gate: Charm++ (reconverse/LCI) + HAPI + Kokkos/HIP
// on Frontier, at the production geometry (design/phase1-gpu.md stage 0).
//
// What this gate proves, in the order it would fail:
//   1. the HIP-enabled charm build links against a hipcc-compiled Kokkos
//      object through the POD firewall in fof/gpu/FoFDevice.h;
//   2. HAPI's PE->GPU mapping is 1:1 with processes (the invariant);
//   3. Kokkos initializes ONCE per process from a nodegroup branch;
//   4. a HAPI stream drives Kokkos kernels (ExecSpace(stream));
//   5. the process's PEs fill disjoint slices of one pinned buffer
//      concurrently and an atomic deposit counter sequences the launch;
//   6. completion arrives as a Charm callback via hapiAddCallback with
//      NO fence and no blocked scheduler thread;
//   7. every particle round-trips through the device byte-exact.
//
// NOTE: this file is compiled by charmc and includes NO Kokkos header —
// that is the firewall working. FoFDevice.cpp is the only Kokkos TU.

#include "spike.decl.h"
#include "../../fof/gpu/FoFDevice.h"

// CMK_HIP / CMK_CUDA arrive with charm++.h (conv-mach-opt.h), so this
// guard works and the gate still builds against a charm WITHOUT a GPU
// backend — where it degrades to a host-backend Kokkos round trip,
// which is section-7 gate 6's arm.
#if defined(CMK_CUDA) || defined(CMK_HIP)
#define SPIKE_HAS_HAPI 1
// hapi_portable.h defines hapiStream_t as hipStream_t/cudaStream_t but
// does NOT pull in the vendor runtime header, so the type has to be
// declared here first. hip_runtime_api.h (not hip_runtime.h) is the
// host-only surface and compiles under g++, which is what charmc uses.
#if defined(CMK_HIP)
#include <hip/hip_runtime_api.h>
#endif
#include <hapi.h>
#endif

#include <atomic>
#include <cstdio>
#include <vector>

/* readonly */ CProxy_Main main_proxy;
/* readonly */ CProxy_DeviceManager dm_proxy;
/* readonly */ CProxy_Filler filler_proxy;
/* readonly */ long n_per_process;

// The staged value of particle i of process p. Deterministic and
// process-dependent, so a mis-bound buffer or a crossed process shows up
// as a mismatch rather than as a plausible number.
static inline float spikePos(long p, long i, int axis) {
  return static_cast<float>((p * 1000003L + i * 3 + axis) % 65536) * 0.125f;
}
static inline long spikeOrder(long p, long i) { return p * 1000000000L + i; }

class DeviceManager : public CBase_DeviceManager {
  fofgpu::Device dev_;
  std::atomic<int> deposits_{0};
  CkCallback ready_cb_;
  void* stream_ = nullptr;
  bool ok_ = true;
  double t_launch_ = 0;

 public:
  DeviceManager() {}

  fofgpu::Device& device() { return dev_; }

  // A nodegroup entry runs on WHICHEVER PE of the process picks the
  // message up — measured, not assumed: this broadcast landed on PEs
  // 9, 20, 30, 46, 61, 72, 88, 105 rather than on the CkNodeFirst PEs.
  // So it does nothing but hop to the home PE, which is the single owner
  // of every Kokkos and HAPI call (design/phase1-gpu.md section 6.1).
  void start(CkCallback cb) {
    ready_cb_ = cb;
    filler_proxy[CkNodeFirst(CkMyNode())].beginOnHome();
  }

  // Runs on the home PE.
  void initOnHome() {
    const int procs_per_physical_node = CmiNumNodes() / CmiNumPhysicalNodes();

    int device_id = -1;
#ifdef SPIKE_HAS_HAPI
    hapiCheck(hapiGetDevice(&device_id));
#endif
    dev_.init(device_id);

    // The 1:1 process/GPU invariant (design/phase1-gpu.md section 2).
    // Report rather than abort here so the gate prints the whole picture
    // before failing.
    if (!dev_.checkMapping(procs_per_physical_node)) {
      CkPrintf("[proc %d] MAPPING FAIL: %d processes on this physical node, "
               "%d devices visible\n",
               CkMyNode(), procs_per_physical_node, dev_.info().n_visible);
      ok_ = false;
    }

    fofgpu::DeviceInfo di = dev_.info();
    CkPrintf("[proc %d pe %d node %d] backend=%s device=%d visible=%d "
             "gpu=\"%s\" mem=%.1f GB pes=%d procs/node=%d\n",
             CkMyNode(), CkMyPe(), CmiPhysicalNodeID(CkMyPe()), di.backend,
             di.device_id, di.n_visible, di.name,
             di.total_global_mem / 1073741824.0, CkNodeSize(CkMyNode()),
             procs_per_physical_node);

    // Bind Kokkos to a HAPI stream: kernels and copies then ride the same
    // stream hapiAddCallback watches.
#ifdef SPIKE_HAS_HAPI
    hapiCreateStreams();
    stream_ = (void*)hapiGetStream();
#endif
    dev_.setStream(stream_);

    dev_.resize((size_t)n_per_process);
    deposits_.store(0);

    // Report the mapping verdict (its own reduction, so a bad mapping is
    // visible even if the round trip later hangs).
    int bad = ok_ ? 0 : 1;
    this->contribute(sizeof(int), &bad, CkReduction::sum_int, ready_cb_);

    // Hand the pack to every PE of this process — the K0 shape.
    const int first = CkNodeFirst(CkMyNode());
    for (int pe = first; pe < first + CkNodeSize(CkMyNode()); pe++)
      filler_proxy[pe].fillSlice();
  }

  // Plain method (not an entry): called synchronously from each PE's
  // Filler through ckLocalBranch, exactly like FoFPhase1Node::submitEdges.
  // The counter is atomic because the callers are different threads.
  void sliceDeposited() {
    if (deposits_.fetch_add(1) + 1 == CkNodeSize(CkMyNode())) {
      // Last depositor. Launch from the process's HOME PE, not from
      // whichever thread happened to finish last: the home PE is the one
      // that armed the stream and is the one whose scheduler loop polls
      // the HAPI event queue (CpvAccess(hapi_event_pool) is per-PE).
      filler_proxy[CkNodeFirst(CkMyNode())].launchOnHome();
    }
  }

  // Runs on the home PE.
  void launch() {
    t_launch_ = CkWallTimer();
    dev_.enqueueRoundTrip();
#ifdef SPIKE_HAS_HAPI
    // No fence: completion comes back as a message. hapiEvent stores the
    // callback BY VALUE (hapi_impl.cpp recordEvent), so a temporary is
    // safe and there is nothing to delete.
    hapiAddCallback((hapiStream_t)stream_,
                    CkCallback(CkIndex_DeviceManager::deviceComplete(),
                               dm_proxy[CkMyNode()]));
#else
    dev_.fence();
    thisProxy[CkMyNode()].deviceComplete();
#endif
  }

  void deviceComplete() {
    // Elapsed from enqueue to callback: H2D + kernel + D2H + the event
    // poll, with the scheduler never blocked in between.
    CkPrintf("[proc %d pe %d] device round trip %.3f ms (%ld particles)\n",
             CkMyNode(), CkMyPe(), (CkWallTimer() - t_launch_) * 1e3,
             n_per_process);
    const int first = CkNodeFirst(CkMyNode());
    for (int pe = first; pe < first + CkNodeSize(CkMyNode()); pe++)
      filler_proxy[pe].verifySlice(
          CkCallback(CkReductionTarget(Main, reportDone), main_proxy));
  }

  bool ok() const { return ok_; }
};

class Filler : public CBase_Filler {
 public:
  Filler() {}

  // This PE's slice of the process's staging buffer.
  void mySlice(long* begin, long* end) const {
    const int rank = CkMyRank(), nranks = CkNodeSize(CkMyNode());
    *begin = (long)((double)n_per_process * rank / nranks);
    *end = (long)((double)n_per_process * (rank + 1) / nranks);
  }

  void fillSlice() {
    DeviceManager* dm = dm_proxy.ckLocalBranch();
    float* pos = dm->device().hostPositions();
    long* order = dm->device().hostOrders();
    long b, e;
    mySlice(&b, &e);
    const long p = CkMyNode();
    // Concurrent writes from every PE of the process into ONE pinned
    // buffer allocated by the home PE — disjoint slices, no locking.
    for (long i = b; i < e; i++) {
      pos[3 * i] = spikePos(p, i, 0);
      pos[3 * i + 1] = spikePos(p, i, 1);
      pos[3 * i + 2] = spikePos(p, i, 2);
      order[i] = spikeOrder(p, i);
    }
    dm->sliceDeposited();
  }

  void beginOnHome() { dm_proxy.ckLocalBranch()->initOnHome(); }
  void launchOnHome() { dm_proxy.ckLocalBranch()->launch(); }

  void verifySlice(CkCallback cb) {
    DeviceManager* dm = dm_proxy.ckLocalBranch();
    const long* label = dm->device().hostLabels();
    long b, e;
    mySlice(&b, &e);
    const long p = CkMyNode();
    int fail = 0;
    for (long i = b; i < e; i++) {
      if (label[i] != spikeOrder(p, i)) {
        if (fail == 0)
          CkPrintf("[proc %d pe %d] VERIFY FAIL at %ld: got %ld want %ld\n",
                   (int)p, CkMyPe(), i, label[i], spikeOrder(p, i));
        fail++;
      }
    }
    this->contribute(sizeof(int), &fail, CkReduction::sum_int, cb);
  }
};

class Main : public CBase_Main {
  double t0_;

 public:
  Main(CkArgMsg* m) {
    n_per_process = 4000000;
    if (m->argc > 1) n_per_process = atol(m->argv[1]);
    delete m;

    main_proxy = thisProxy;
    CkPrintf("kokkos-spike: %d PEs, %d processes, %d physical nodes, "
             "%ld particles/process, device backend compiled in: %s\n",
             CkNumPes(), CkNumNodes(), CmiNumPhysicalNodes(), n_per_process,
             fofgpu::Device::available() ? "yes" : "NO (host backend)");

    dm_proxy = CProxy_DeviceManager::ckNew();
    filler_proxy = CProxy_Filler::ckNew();

    t0_ = CkWallTimer();
    dm_proxy.start(CkCallback(CkReductionTarget(Main, reportReady), thisProxy));
  }

  // Reached once every process has staged, launched, and completed.
  void reportReady(int n_bad) {
    if (n_bad) {
      CkPrintf("FAILED: %d processes reported a bad process/GPU mapping\n",
               n_bad);
      CkExit(1);
    }
  }

  void reportDone(int n_fail) {
    const double t = CkWallTimer() - t0_;
    if (n_fail) {
      CkPrintf("FAILED: %d particles did not round-trip\n", n_fail);
      CkExit(1);
    }
    CkPrintf("OK: %ld particles/process round-tripped through the device on "
             "every process (%.3f s wall, staging included)\n",
             n_per_process, t);
    CkExit();
  }
};

#include "spike.def.h"
