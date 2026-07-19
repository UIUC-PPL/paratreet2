#ifndef PARATREET_CHECKVISITOR_H_
#define PARATREET_CHECKVISITOR_H_

#include "paratreet.decl.h"
#include "common.h"
#include "MinMaxData.h"
#include <cmath>

// Per-PE stats group tracking the global density range seen at source leaves.
struct MinMaxTracker : public CBase_MinMaxTracker {
  Real min_seen = HUGE_VAL;
  Real max_seen = -HUGE_VAL;

  void collect(const CkCallback& cb) {
    // Reduce {-min, max} under max so one reduction carries both bounds.
    double vals[2] = { -(double)min_seen, (double)max_seen };
    this->contribute(2 * sizeof(double), vals, CkReduction::max_double, cb);
  }

  void reset(const CkCallback& cb) {
    min_seen = HUGE_VAL;
    max_seen = -HUGE_VAL;
    this->contribute(cb);
  }
};

extern CProxy_MinMaxTracker min_max_tracker;

// Visits every node (like VisitAllVisitor) and, at each source leaf, checks
// that the MinMaxData annotation matches the (mutated) particle densities.
// Target particles are deliberately NOT checked: only subtree copies are
// mutated, partition copies keep their original densities.
struct CheckVisitor {
public:
  static constexpr const bool CallSelfLeaf = true;

  void pup(PUP::er& p) {}

public:
  static bool open(const SpatialNode<MinMaxData>& source, SpatialNode<MinMaxData>& target) {
    return true;
  }

  static void node(const SpatialNode<MinMaxData>& source, SpatialNode<MinMaxData>& target) {}

  static void leaf(const SpatialNode<MinMaxData>& source, SpatialNode<MinMaxData>& target) {
    if (source.n_particles <= 0) return;
    Real minv = HUGE_VAL, maxv = -HUGE_VAL;
    for (int i = 0; i < source.n_particles; i++) {
      const Particle& p = source.particles()[i];
      // The per-leaf mutation must be visible here, including for remote
      // sources shipped through the CacheManager.
      CkEnforce(p.density == (Real)(3.0 * p.order));
      if (p.density < minv) minv = p.density;
      if (p.density > maxv) maxv = p.density;
    }
    // The upwardPass annotation must match the mutated particles exactly.
    CkEnforce(minv == source.data.minv);
    CkEnforce(maxv == source.data.maxv);

    auto tracker = min_max_tracker.ckLocalBranch();
    if (minv < tracker->min_seen) tracker->min_seen = minv;
    if (maxv > tracker->max_seen) tracker->max_seen = maxv;
  }
};

#endif // PARATREET_CHECKVISITOR_H_
