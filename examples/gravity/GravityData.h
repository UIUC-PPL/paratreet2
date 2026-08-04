#ifndef PARATREET_GRAVITYDATA_H_
#define PARATREET_GRAVITYDATA_H_

#include "common.h"
#include "Particle.h"
#include "OrientedBox.h"

#include <algorithm>

// Monopole node payload for the gravity example (design/barnes-hut-app.md):
// bounding box (a requirement of the framework's Data concept — the load
// balancer's load estimate reads it), total mass, mass-weighted position
// sum (the center of mass is cm() = moment / totalMass), particle count,
// and the opening radius read by GravityVisitor's acceptance test. This is old paratreet's CentroidData with the multipole
// tensor and SPH/collision baggage removed — exactly what the monopole
// (#ifdef BARNESHUT) gravity path consumed.
//
// Computed entirely at BUILD time (leaf ctor + operator+= up the tree);
// each iteration's rebuild refreshes it after the particles move. No
// post-build upwardPass involved.
struct GravityData {
  OrientedBox<Real> box;
  Vector3D<Real> moment = Vector3D<Real>(0, 0, 0);
  Real totalMass = 0;
  long count = 0;
  Real radius = 0;

  GravityData() = default;

  /// Construct from a leaf's particles.
  GravityData(const Particle* particles, int n_particles, int depth)
      : GravityData() {
    for (int i = 0; i < n_particles; i++) {
      moment += particles[i].mass * particles[i].position;
      totalMass += particles[i].mass;
      box.grow(particles[i].position);
    }
    count = n_particles;
    // Leaf rule = old calculateRadiusBox (half the box diagonal); the
    // singleton convention (radius = 1.0, "single particle boxes don't
    // need scaling") is old code's, kept verbatim.
    if (count > 1) {
      radius = 0.5 * (box.greater_corner - box.lesser_corner).length();
    } else {
      radius = 1.0;
    }
  }

  const GravityData& operator+=(const GravityData& d) {  // upward traversal
    box.grow(d.box);
    moment += d.moment;
    totalMass += d.totalMass;
    count += d.count;
    // Internal-node rule = old calculateRadiusFarthestCorner: distance
    // from the center of mass to the farthest box corner.
    if (count > 1) {
      Vector3D<Real> c = cm();
      Vector3D<Real> d1 = c - box.lesser_corner;
      Vector3D<Real> d2 = box.greater_corner - c;
      d1.x = std::max(d1.x, d2.x);
      d1.y = std::max(d1.y, d2.y);
      d1.z = std::max(d1.z, d2.z);
      radius = d1.length();
    } else {
      radius = 1.0;
    }
    return *this;
  }

  GravityData& operator=(const GravityData&) = default;

  Vector3D<Real> cm() const {
    return totalMass > 0 ? moment / totalMass : box.center();
  }

  void pup(PUP::er& p) {
    p | box;
    p | moment;
    p | totalMass;
    p | count;
    p | radius;
  }
};

#endif  // PARATREET_GRAVITYDATA_H_
