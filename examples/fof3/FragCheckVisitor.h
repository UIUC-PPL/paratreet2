#ifndef PARATREET_FRAGCHECKVISITOR_H_
#define PARATREET_FRAGCHECKVISITOR_H_

#include "paratreet.decl.h"
#include "common.h"
#include "FoFData.h"

#include <limits>

// Visits every node (like the annotate example's CheckVisitor) and, at each
// source leaf, checks that the FragData annotation — recomputed by
// Subtree::upwardPass after FoF phase 1 wrote Particle::group_number —
// matches the leaf's particles exactly. Remote sources arrive through the
// CacheManager, so this validates cache-shipped leaves too. Target particles
// (Partition copies) are deliberately not checked: phase 1 relabels only the
// Subtree copies.
struct FragCheckVisitor {
public:
  static constexpr const bool CallSelfLeaf = true;

  void pup(PUP::er& p) {}

public:
  static bool open(const SpatialNode<FragData>& source, SpatialNode<FragData>& target) {
    return true;
  }

  static void node(const SpatialNode<FragData>& source, SpatialNode<FragData>& target) {}

  static void leaf(const SpatialNode<FragData>& source, SpatialNode<FragData>& target) {
    if (source.n_particles <= 0) return;
    long minf = std::numeric_limits<long>::max();
    long maxf = std::numeric_limits<long>::min();
    for (int i = 0; i < source.n_particles; i++) {
      long g = source.particles()[i].group_number;
      if (g < minf) minf = g;
      if (g > maxf) maxf = g;
    }
    // CkEnforce, not CkAssert: this Charm build is CMK_OPTIMIZE, which
    // compiles CkAssert out, and these checks are the point of the test.
    CkEnforce(minf == source.data.min_frag);
    CkEnforce(maxf == source.data.max_frag);
  }
};

#endif // PARATREET_FRAGCHECKVISITOR_H_
