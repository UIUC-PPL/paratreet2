#ifndef EXAMPLE_MAIN_H
#define EXAMPLE_MAIN_H

#include "Main.decl.h"
#include "Paratreet.h"
#include "GravityData.h"

#include <vector>

// Flat per-particle record shipped through the concatenating reduction to
// processor 0 by the verification harness. Deliberately plain-old-data —
// just bytes, no pointers or constructors — so the reduction can
// concatenate the records without any packing/unpacking step (see
// GravityCheck::collect).
struct PartSample {
  long order;
  Real mass;
  Real soft;
  Vector3D<Real> position;
  Vector3D<Real> acceleration;
};

// Per-PE collection group for the direct-sum check: the per-leaf deposit
// fn (Main.C) pushes every leaf particle's (position, mass, soft,
// acceleration) here; collect() concat-reduces the flat records to PE 0
// where the O(n^2) reference is computed. Same harness shape as annotate's
// MinMaxTracker.
struct GravityCheck : public CBase_GravityCheck {
  std::vector<PartSample> samples;

  void collect(const CkCallback& cb) {
    this->contribute(samples.size() * sizeof(PartSample), samples.data(),
                     CkReduction::concat, cb);
    samples.clear();
  }
};

class ExMain : public paratreet::Main<GravityData> {
  virtual Real getTimestep(BoundingBox&, Real) override;
  virtual void preTraversalFn(ProxyPack<GravityData>&) override;
  virtual void traversalFn(BoundingBox&, ProxyPack<GravityData>&, int) override;
  virtual void postIterationFn(BoundingBox&, ProxyPack<GravityData>&, int) override;
  virtual void setDefaults(void) override;
  virtual void main(CkArgMsg*) override;
  virtual void run(void) override;

  // App flags (getopt in main(); see README):
  //   -o <theta>  opening angle (default 0.7, old paratreet's default)
  //   -T <dt>     FIXED timestep (leapfrog; default 0.01) — deterministic
  //               regressions (Kale 2026-08-04); getTimestep ignores the
  //               framework's adaptive max_velocity argument.
  //   -c <mode>   verification: full | off | auto (default auto = full
  //               when N <= check_auto_limit, at iteration 0)
  double theta = 0.7;
  double fixed_dt = 0.01;
  enum class CheckMode { Auto, Full, Off };
  CheckMode check_mode = CheckMode::Auto;
  static constexpr long check_auto_limit = 10000;

  void runDirectSumCheck(BoundingBox& universe,
                         ProxyPack<GravityData>& proxy_pack);
};

#endif  // EXAMPLE_MAIN_H
