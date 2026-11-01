// FoF3 as an in-situ plug-in (design/plugin-interface.md): the hosted app
// main and the API in FoFPluginAPI.h. Linked only into host programs
// (libmodulefofplugin.a, linked with `-module fofplugin`); FoF3 registers its own main.
//
// paratreet's MainChare runs in the host program too. With conf.hosted set
// here it skips argv parsing and Driver::init loads no file, so startup
// creates the groups, the Driver and the cache and then calls run(), which
// only marks the plug-in ready. Each fofplugin::run() then drives one
// Driver::runHosted over the particles the host staged, with this main's
// hooks as the FoF iteration (the essential sequence of
// examples/fof3/FoF3.C, without its diagnostics).

#include "fofplugin.decl.h"
#include "Paratreet.h"
#include "FoFData.h"
#include "FoFPhase1.h"
#include "FoFPhase3.h"
#include "FoFPluginAPI.h"

#include <cmath>

static void fofPluginRegisterReducers() { BoundingBox::registerReducer(); }

class FoFPluginMain : public paratreet::Main<FragData> {
 public:
  // Valid on PE 0 only (created in main(), which runs in the mainchare).
  CProxy_FoFPhase1<FragData> fof;
  CProxy_FoFPhase1Node<FragData> fof_node;
  CkGroupID uf_node_map_gid;
  fofplugin::Params params;
  double b = 0.0;
  bool ready = false;
  std::vector<CkCallback> waiting;

  void __register() override {
    paratreet::Main<FragData>::__register();
    fof::registerChares<FragData>();
  }

  Real getTimestep(BoundingBox&, Real) override { return 0; }

  // FoF3's defaults (examples/fof3/Main.C), plus hosted and single
  // distribution, which runHosted requires.
  void setDefaults() override {
    conf.hosted = true;
    conf.single_distribution = true;
    conf.min_n_treepieces = CkNumPes() * 8;
    conf.min_n_partitions = CkNumPes() * 8;
    conf.max_particles_per_leaf = params.leaf_size;
    conf.decomp_type = paratreet::DecompType::eOct;
    conf.tree_type = paratreet::TreeType::eOct;
    conf.num_iterations = 1;
    conf.perturb_particles = false;
    conf.read_velocity_and_soft = false;
    conf.num_share_nodes = 0;
    conf.cache_share_depth = 3;
    conf.request_pause_interval = 20;
    conf.iter_pause_interval = 1000;
  }

  void main(CkArgMsg*) override {
    fof_node = CProxy_FoFPhase1Node<FragData>::ckNew();
    fof = CProxy_FoFPhase1<FragData>::ckNew(fof_node);
    fof.deviceWarmup();
  }

  // Startup is complete (MainChare::run): release anyone in whenReady.
  void run() override {
    ready = true;
    for (auto& cb : waiting) cb.send();
    waiting.clear();
  }

  Vector3D<Real> period() const {
    return Vector3D<Real>(params.period, params.period, params.period);
  }

  void preTraversalFn(ProxyPack<FragData>& pack) override {
    uf_node_map_gid = CProxy_UFNodeMap::ckNew();
    const BoundingBox& universe = thread_state_holder.ckLocalBranch()->universe;
    b = params.linking_length > 0
            ? params.linking_length
            : params.b_factor *
                  std::cbrt(universe.box.volume() / (double)universe.n_particles);
    if (params.period > 0 && b >= 0.5 * params.period)
      CkAbort("fofplugin: linking length %g is not < period/2 = %g\n", b,
              0.5 * params.period);
    CkPrintf("FOFPLUGIN linking_length %.17g n %ld period %g\n", b,
             (long)universe.n_particles, params.period);
    paratreet::FoFPhase1Stages p1s;
    paratreet::runFoFPhase1(pack.treepiece, fof, fof_node, b, period(), &p1s,
                            4.0);
    fof.applyTipEncoding(CkCallbackResumeThread());
    pack.treepiece.upwardPass(CkCallbackResumeThread());
    CkWaitQD();
    pack.driver.loadCache(CkCallbackResumeThread());
  }

  void traversalFn(BoundingBox&, ProxyPack<FragData>& pack, int) override {
    paratreet::runFoFPhase3Dist(pack.partition, fof, fof_node, b, period(),
                                true, pack.treepiece, uf_node_map_gid,
                                params.stream_batch);
  }

  void postIterationFn(BoundingBox&, ProxyPack<FragData>&, int) override {
    fof.returnHostLabels(CkCallbackResumeThread());
    CkWaitQD();
  }
};

PARATREET_REGISTER_MAIN(FoFPluginMain);

namespace fofplugin {

static FoFPluginMain& pluginMain() {
  return static_cast<FoFPluginMain&>(*CsvAccess(paratreet::main_));
}

void whenReady(const CkCallback& cb) {
  CkEnforce(CkMyPe() == 0);
  auto& m = pluginMain();
  if (m.ready) cb.send();
  else m.waiting.push_back(cb);
}

size_t stage(const Particle* p, size_t n) {
  auto& labels = paratreet::hostLabels();
  const size_t first = labels.size();
  CkEnforce(first + n < (size_t(1) << paratreet::kOriginIndexBits));
  std::vector<::Particle> staged(n);
  for (size_t i = 0; i < n; i++) {
    ::Particle& q = staged[i];
    q.position = Vector3D<Real>(p[i].x, p[i].y, p[i].z);
    q.velocity = Vector3D<Real>(p[i].vx, p[i].vy, p[i].vz);
    q.mass = p[i].mass;
    CkEnforce(p[i].id <= paratreet::kUF2IdxMask);
    q.order = (long)p[i].id;
    q.origin = (uint64_t(CkMyPe()) << paratreet::kOriginIndexBits) | (first + i);
  }
  readers.ckLocalBranch()->stage(staged.data(), n);
  labels.resize(first + n, -1);
  return first;
}

void run(const Params& params, const CkCallback& done) {
  CkEnforce(CkMyPe() == 0);
  auto& m = pluginMain();
  CkEnforce(m.ready);
  m.params = params;
  // Per-call configuration (the leaf size is read through the broadcast
  // copy on every PE), then one hosted pass.
  m.conf.max_particles_per_leaf = params.leaf_size;
  treespec.receiveConfiguration(CkCallbackResumeThread(),
                                CkReference<paratreet::Configuration>(m.conf));
  m.driver.runHosted(done);
}

const long* labels(size_t& n) {
  auto& l = paratreet::hostLabels();
  n = l.size();
  return l.data();
}

void clear() { paratreet::hostLabels().clear(); }

}  // namespace fofplugin

#include "templates.h"

#include "fofplugin.def.h"
