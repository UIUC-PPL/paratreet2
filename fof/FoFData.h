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
// group_number — and are never read; run TreePiece::upwardPass after phase 1
// (relabel) to compute the real annotations. uniform() is the hereditary
// predicate phase 3 prunes on: every particle under the node belongs to the
// same fragment.
struct FoFCachedParticle {
  // The 20 bytes the FoF walk reads from REMOTE particles
  // (design/cached-particle-slimming.md): stored AND shipped form of
  // cache-copied particles for this application.
  Vector3D<Real> position;
  long group_number = -1;
  FoFCachedParticle() = default;
  explicit FoFCachedParticle(const Particle& p)
      : position(p.position), group_number(p.group_number) {}
  void pup(PUP::er& p) {
    p | position;
    p | group_number;
  }
};

struct FragData {
  using CachedParticle = FoFCachedParticle;
  OrientedBox<Real> box; // required by the Data concept (grown from positions)
  long min_frag;
  long max_frag;
  // Particles beneath this node. Node::n_particles is -1 on internal
  // nodes by design, so this is the only count available above the leaf
  // level — and unit cost is driven by particles in the interaction
  // region, not by box geometry (measured 2026-08-07: a geometric split
  // criterion multiplied units 24x at 2B without touching the largest
  // unit, because a small box in a dense core holds enormous work).
  long n_below = 0;

  FragData()
    : min_frag(std::numeric_limits<long>::max()),
      max_frag(std::numeric_limits<long>::min()) {}

  // Remote-particle slimming opt-in (MultiData.h): the FoF walk reads only
  // position (distance tests) and group_number (the tip, for edge emission)
  // from cache-shipped particles — ship exactly those (~20 of ~112 bytes).

  FragData(const Particle* particles, int n_particles, int depth) : FragData() {
    n_below = n_particles > 0 ? n_particles : 0;
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
    n_below += other.n_below;
    return *this;
  }

  FragData& operator=(const FragData&) = default;

  void pup(PUP::er& p) {
    p | box;
    p | min_frag;
    p | max_frag;
    p | n_below;
  }
};

#endif // PARATREET_FOFDATA_H_
