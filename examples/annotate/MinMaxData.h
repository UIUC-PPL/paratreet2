#ifndef PARATREET_MINMAXDATA_H_
#define PARATREET_MINMAXDATA_H_

#include "common.h"
#include "Particle.h"
#include "OrientedBox.h"
#include <cmath>

// Node annotation carrying the min/max particle density under the node.
// Used by the annotate example to prove that post-build particle mutation
// plus Subtree::upwardPass leaves node annotations consistent with the
// particles, all the way through the CacheManager.
struct MinMaxData {
  Real minv;
  Real maxv;
  OrientedBox<Real> box; // required by the Data concept (LB load estimation)

  MinMaxData() : minv(HUGE_VAL), maxv(-HUGE_VAL) {}

  /// Construct min/max range from particle densities.
  MinMaxData(const Particle* particles, int n_particles, int depth) : MinMaxData() {
    for (int i = 0; i < n_particles; i++) {
      Real d = particles[i].density;
      if (d < minv) minv = d;
      if (d > maxv) maxv = d;
      box.grow(particles[i].position);
    }
  }

  const MinMaxData& operator+=(const MinMaxData& other) { // needed for upward traversal
    if (other.minv < minv) minv = other.minv;
    if (other.maxv > maxv) maxv = other.maxv;
    box.grow(other.box);
    return *this;
  }

  MinMaxData& operator=(const MinMaxData&) = default;

  void pup(PUP::er& p) {
    p | minv;
    p | maxv;
    p | box;
  }
};

#endif // PARATREET_MINMAXDATA_H_
