#ifndef PARATREET_FOFDATA_H_
#define PARATREET_FOFDATA_H_

#include "common.h"
#include "Particle.h"
#include "OrientedBox.h"

#include <limits>

// FoF node annotation (design/phase1.md, "After phase 1"): the bounding box
// plus the min/max Particle::group_number (fragment/tip id) over the
// particles under the node.
//
// Build-time values are garbage — tree build runs before FoF phase 1 assigns
// group_number — and are never read; run Subtree::upwardPass after phase 1
// (relabel) to compute the real annotations. uniform() is the hereditary
// predicate phase 3 prunes on: every particle under the node belongs to the
// same fragment.
struct FragData {
  OrientedBox<Real> box; // required by the Data concept (grown from positions)
  long min_frag;
  long max_frag;

  FragData()
    : min_frag(std::numeric_limits<long>::max()),
      max_frag(std::numeric_limits<long>::min()) {}

  FragData(const Particle* particles, int n_particles, int depth) : FragData() {
    for (int i = 0; i < n_particles; i++) {
      box.grow(particles[i].position);
      long g = particles[i].group_number;
      if (g < min_frag) min_frag = g;
      if (g > max_frag) max_frag = g;
    }
  }

  // Empty nodes keep the identity values (min_frag > max_frag), so they are
  // never uniform.
  bool uniform() const { return min_frag == max_frag; }

  const FragData& operator+=(const FragData& other) { // upward traversal
    box.grow(other.box);
    if (other.min_frag < min_frag) min_frag = other.min_frag;
    if (other.max_frag > max_frag) max_frag = other.max_frag;
    return *this;
  }

  FragData& operator=(const FragData&) = default;

  void pup(PUP::er& p) {
    p | box;
    p | min_frag;
    p | max_frag;
  }
};

#endif // PARATREET_FOFDATA_H_
