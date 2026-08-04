#include "Main.h"

#include "GravityVisitor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

PARATREET_REGISTER_MAIN(ExMain);

/* readonly */ int peanoKey;
/* readonly */ CProxy_GravityCheck gravity_check;

static void initialize() {
  BoundingBox::registerReducer();
}

// Per-leaf deposit for the verification harness: runs on each Partition
// element's leaves (framework callPerLeafFn), pushing flat records into
// the PE's GravityCheck branch.
static void depositLeafFn(SpatialNode<GravityData>& leaf,
                          Partition<GravityData>* partition) {
  auto* branch = gravity_check.ckLocalBranch();
  for (int i = 0; i < leaf.n_particles; i++) {
    const Particle& p = leaf.particles()[i];
    branch->samples.push_back(
        PartSample{p.order, p.mass, p.soft, p.position, p.acceleration});
  }
}
PARATREET_REGISTER_PER_LEAF_FN(DepositFn, GravityData, depositLeafFn);

// Defaults are applied before the framework parses the command line.
void ExMain::setDefaults(void) {
  // Free -c for the app's check-mode flag: the framework registers -c
  // (minVerticesPerComponent), which gravity does not use (release_arg
  // removes it from the framework parse table so its argv text reaches
  // getopt below). -o and -T collide with nothing framework-registered.
  conf.release_arg("c");
  conf.min_n_subtrees = CkNumPes() * 8;
  conf.min_n_partitions = CkNumPes() * 8;
  conf.max_particles_per_leaf = 12;
  conf.decomp_type = paratreet::DecompType::eOct;
  conf.tree_type = paratreet::TreeType::eOct;
  conf.num_iterations = 1;
  conf.num_share_nodes = 0;
  conf.cache_share_depth = 3;
  conf.request_pause_interval = 20;
  conf.iter_pause_interval = 1000;
}

void ExMain::main(CkArgMsg* m) {
  peanoKey = 3;

  // App-specific arguments; everything framework-registered was consumed
  // and removed from argv by Configuration::parse before this runs.
  int c;
  while ((c = getopt(m->argc, m->argv, "o:T:c:")) != -1) {
    switch (c) {
      case 'o':
        theta = atof(optarg);
        if (theta < 0.0)
          CkAbort("-o requires an opening angle >= 0 (0 = exact always-open "
                  "mode for verification)");
        break;
      case 'T':
        fixed_dt = atof(optarg);
        if (!(fixed_dt > 0.0)) CkAbort("-T requires a timestep > 0");
        break;
      case 'c':
        if (strcmp(optarg, "full") == 0)      check_mode = CheckMode::Full;
        else if (strcmp(optarg, "off") == 0)  check_mode = CheckMode::Off;
        else if (strcmp(optarg, "auto") == 0) check_mode = CheckMode::Auto;
        else CkAbort("-c requires one of: full, off, auto");
        break;
      default:
        break;
    }
  }
  delete m;

  CkPrintf("\n[PARATREET GRAVITY (monopole Barnes-Hut)]\n");
  if (conf.input_file.empty()) CkAbort("Input file unspecified");
  CkPrintf("Input file: %s\n", conf.input_file.c_str());
  CkPrintf("Decomposition type: %s\n",
           paratreet::asString(conf.decomp_type).c_str());
  CkPrintf("Tree type: %s\n", paratreet::asString(conf.tree_type).c_str());
  CkPrintf("Opening angle theta: %g\n", theta);
  CkPrintf("Fixed timestep: %g\n", fixed_dt);
  CkPrintf("Iterations: %d\n", conf.num_iterations);

  gravity_check = CProxy_GravityCheck::ckNew();
}

void ExMain::run() {
  driver.run(CkCallbackResumeThread());
  CkExit();
}

Real ExMain::getTimestep(BoundingBox& universe, Real max_velocity) {
  // Fixed timestep for deterministic regressions (design/barnes-hut-app.md,
  // Kale 2026-08-04); the adaptive old-paratreet rule is intentionally not
  // ported. max_velocity is ignored.
  return fixed_dt;
}

void ExMain::preTraversalFn(ProxyPack<GravityData>& proxy_pack) {
  proxy_pack.driver.loadCache(CkCallbackResumeThread());
}

void ExMain::traversalFn(BoundingBox& universe,
                         ProxyPack<GravityData>& proxy_pack, int iter) {
  proxy_pack.partition.template startDown<GravityVisitor>(
      GravityVisitor(theta));
}

void ExMain::postIterationFn(BoundingBox& universe,
                             ProxyPack<GravityData>& proxy_pack, int iter) {
  // Verify at iteration 0 only: the driver has already run the traversal
  // (accelerations populated) and the first half-kick (velocities — not
  // compared), but not yet perturb (positions unmoved, accelerations not
  // yet zeroed).
  if (iter != 0) return;
  bool enabled = check_mode == CheckMode::Full ||
                 (check_mode == CheckMode::Auto &&
                  universe.n_particles <= check_auto_limit);
  if (!enabled) return;
  if (universe.n_particles > 200000)
    CkAbort("-c full direct sum is O(n^2); refusing above 200k particles");
  runDirectSumCheck(universe, proxy_pack);
}

void ExMain::runDirectSumCheck(BoundingBox& universe,
                               ProxyPack<GravityData>& proxy_pack) {
  double t0 = CkWallTimer();
  proxy_pack.partition.callPerLeafFn(
      PARATREET_PER_LEAF_FN(DepositFn, GravityData), CkCallbackResumeThread());
  void* result = nullptr;
  gravity_check.collect(CkCallbackResumeThread(result));
  CkReductionMsg* msg = (CkReductionMsg*)result;
  long n = msg->getSize() / sizeof(PartSample);
  CkEnforce((size_t)n * sizeof(PartSample) == (size_t)msg->getSize());
  CkEnforce(n == universe.n_particles);
  std::vector<PartSample> s((PartSample*)msg->getData(),
                            (PartSample*)msg->getData() + n);
  delete msg;
  std::sort(s.begin(), s.end(),
            [](const PartSample& a, const PartSample& b) {
              return a.order < b.order;
            });

  // Serial O(n^2) reference: the SAME kernel formulas the tree walk's
  // leaf case uses, evaluated in DOUBLE from the identical (float) input
  // positions. The build's Real is float, so the walk's accumulation
  // carries float roundoff that depends on summation order; the double
  // reference makes the comparison one-sided — the residual measures the
  // walk's own float-accumulation noise, not the reference's.
  std::vector<Vector3D<double>> ref(n, Vector3D<double>(0, 0, 0));
  for (long i = 0; i < n; i++) {
    for (long j = 0; j < n; j++) {
      if (i == j) continue;
      Vector3D<double> pj(s[j].position.x, s[j].position.y, s[j].position.z);
      Vector3D<double> pi(s[i].position.x, s[i].position.y, s[i].position.z);
      Vector3D<double> diff = pj - pi;
      double rsq = diff.lengthSquared();
      if (rsq == 0) continue;
      double twoh = (double)s[j].soft + (double)s[i].soft;
      double a, b;
      GravityVisitor::SPLINE(rsq, twoh, a, b);
      ref[i] += diff * (b * (double)s[j].mass);
    }
  }

  // Per-particle relative error against the reference — the maximum and
  // the root-mean-square ("rms" below) — plus the momentum diagnostics:
  // the reference is exactly antisymmetric (|sum m*a| ~ 0); the tree
  // sum's violation of Newton's third law is reported, not gated.
  double max_rel = 0, sum_sq_rel = 0;
  Vector3D<double> mom_tree(0, 0, 0), mom_ref(0, 0, 0);
  double mom_scale = 0;
  for (long i = 0; i < n; i++) {
    Vector3D<double> at(s[i].acceleration.x, s[i].acceleration.y,
                        s[i].acceleration.z);
    Vector3D<double> d = at - ref[i];
    double rel = std::sqrt(d.lengthSquared()) /
                 (std::sqrt(ref[i].lengthSquared()) + 1e-300);
    if (rel > max_rel) max_rel = rel;
    sum_sq_rel += rel * rel;
    mom_tree += (double)s[i].mass * at;
    mom_ref += (double)s[i].mass * ref[i];
    mom_scale += (double)s[i].mass * std::sqrt(ref[i].lengthSquared());
  }
  double rms_rel = std::sqrt(sum_sq_rel / n);
  CkPrintf("GRAVITY CHECK: n %ld theta %g rms_rel_err %.3e max_rel_err %.3e "
           "mom_ref %.3e mom_tree %.3e (relative to sum m|a|) ref_time %.3f s\n",
           n, theta, rms_rel, max_rel,
           std::sqrt(mom_ref.lengthSquared()) / (mom_scale + 1e-300),
           std::sqrt(mom_tree.lengthSquared()) / (mom_scale + 1e-300),
           CkWallTimer() - t0);

  // Gates. theta == 0 (always-open mode): the walk degenerates to the
  // same all-pairs softened sum as the reference, so the residual is
  // purely the walk's FLOAT accumulation noise against the double
  // reference (this build's Real is float) — gate at the float band,
  // ~100x below any structural error (one missed/duplicated 12-particle
  // leaf shifts a neighbor's acceleration by ~1e-2). Measured 2026-08-04:
  // max 7.9e-6 / rms 6.4e-7 at 1k. (No POSITIVE theta degenerates the
  // walk: the criterion is scale-relative, and the measured error follows
  // the theta^2 monopole law all the way down — 4.3e-3 at 0.7 vs 6.4e-7
  // at 0.005 on 1k.) Production theta gates on the recorded monopole
  // band.
  if (theta == 0.0) {
    if (max_rel < 1e-4 && rms_rel < 1e-5) {
      CkPrintf("GRAVITY TEST PASSED: exact-mode match within float "
               "accumulation band (max_rel %.3e rms %.3e)\n",
               max_rel, rms_rel);
    } else {
      CkAbort("GRAVITY TEST FAILED: exact mode (-o 0) outside the float "
              "accumulation band vs the double direct sum");
    }
  } else {
    // Recorded band: first measurements 2026-08-04 (1k/10k Plummer at
    // theta 0.7) — tighten once the values are frozen, FoF-style.
    if (rms_rel < 0.05) {
      CkPrintf("GRAVITY TEST PASSED: rms_rel %.3e within band\n", rms_rel);
    } else {
      CkAbort("GRAVITY TEST FAILED: rms error above the recorded band");
    }
  }
}

#include "templates.h"

#include "Main.def.h"
