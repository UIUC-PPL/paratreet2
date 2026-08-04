#ifndef PARATREET_GRAVITYVISITOR_H_
#define PARATREET_GRAVITYVISITOR_H_

#include "paratreet.decl.h"
#include "common.h"
#include "Space.h"
#include "GravityData.h"

#include <cmath>

// Monopole Barnes-Hut gravity visitor (design/barnes-hut-app.md), ported
// from old paratreet's GravityVisitor in its #ifdef BARNESHUT (monopole)
// configuration:
//   open(): the standard acceptance test — open a source node for a target
//     box iff the box intersects the sphere of radius^2 * (4/3)/theta^2
//     around the source's center of mass (plus a small-node always-open
//     rule). The non-hexadecapole path never consults softening here.
//   node(): accepted internal node -> plain monopole acceleration from
//     (cm, totalMass), unsoftened (acceptance guarantees separation).
//   leaf(): full pairwise interaction with cubic-spline softening (SPLINE,
//     ported verbatim; twoh = soft_i + soft_j).
// Conventions kept from old code: G = 1; nMinParticleNode = 6;
// opening_geometry_factor_squared = 4/3. Periodic replicas (the offset
// member) and Ewald are out of scope — see the design note.
class GravityVisitor {
 public:
  // A target bucket must interact with its own source leaf.
  static constexpr const bool CallSelfLeaf = true;
  static constexpr const Real opening_geometry_factor_squared = 4.0 / 3.0;
  static constexpr const int nMinParticleNode = 6;

  GravityVisitor() {}
  // theta > 0: the standard criterion. theta == 0: ALWAYS OPEN — the walk
  // degenerates to an all-pairs leaf computation with the same softened
  // kernel as the direct-sum reference, which must then agree to roundoff
  // (the verification harness's exactness mode; no positive theta can do
  // this, because the criterion is scale-relative — a deep small node is
  // always accepted against a distant enough target).
  explicit GravityVisitor(Real theta)
      : gravity_factor(
            theta > 0 ? opening_geometry_factor_squared / (theta * theta)
                      : 0) {}

  void pup(PUP::er& p) { p | gravity_factor; }

 private:
  Real gravity_factor = 0;

 public:
  /// Softened force term from the cubic-spline density profile (ported
  /// verbatim from old paratreet; b is the force coefficient such that
  /// accel += diff * b * mass; the potential term a is unused here).
  /// Public, static, and templated over the float type: the walk runs it
  /// at Real (float in this build), and the direct-sum verification
  /// harness (Main.C) runs the SAME formulas in double as its reference.
  template <typename F>
  static inline void SPLINE(F r2, F twoh, F& a, F& b) {
    F r = sqrt(r2);
    if (r < twoh) {
      F dih = 2.0 / twoh;
      F u = r * dih;
      if (u < 1.0) {
        a = dih * (7.0 / 5.0 - 2.0 / 3.0 * u * u + 3.0 / 10.0 * u * u * u * u
                   - 1.0 / 10.0 * u * u * u * u * u);
        b = dih * dih * dih * (4.0 / 3.0 - 6.0 / 5.0 * u * u
                               + 1.0 / 2.0 * u * u * u);
      } else {
        F dir = 1.0 / r;
        a = -1.0 / 15.0 * dir
            + dih * (8.0 / 5.0 - 4.0 / 3.0 * u * u + u * u * u
                     - 3.0 / 10.0 * u * u * u * u
                     + 1.0 / 30.0 * u * u * u * u * u);
        b = -1.0 / 15.0 * dir * dir * dir
            + dih * dih * dih * (8.0 / 3.0 - 3.0 * u + 6.0 / 5.0 * u * u
                                 - 1.0 / 6.0 * u * u * u);
      }
    } else {
      a = 1.0 / r;
      b = a * a * a;
    }
  }

 public:
  bool open(const SpatialNode<GravityData>& source,
            SpatialNode<GravityData>& target) {
    if (gravity_factor <= 0) return true;  // exact mode (theta == 0)
    if (source.data.count <= nMinParticleNode) return true;
    Real dataRsq = source.data.radius * source.data.radius * gravity_factor;
    return Space::intersect(target.data.box, source.data.cm(), dataRsq);
  }

  void node(const SpatialNode<GravityData>& source,
            SpatialNode<GravityData>& target) {
    if (source.data.count == 0) return;
    // Monopole: treat the node as a point (cm, totalMass). Unsoftened —
    // the acceptance test keeps accepted nodes far from the target box.
    Vector3D<Real> cm = source.data.cm();
    for (int i = 0; i < target.n_particles; i++) {
      Vector3D<Real> diff = cm - target.particles()[i].position;
      Real rsq = diff.lengthSquared();
      if (rsq != 0) {
        Vector3D<Real> accel =
            diff * (source.data.totalMass / (rsq * sqrt(rsq)));
        target.applyAcceleration(i, accel);
      }
    }
  }

  void leaf(const SpatialNode<GravityData>& source,
            SpatialNode<GravityData>& target) {
    for (int i = 0; i < target.n_particles; i++) {
      Vector3D<Real> accel(0.0);
      for (int j = 0; j < source.n_particles; j++) {
        Vector3D<Real> diff =
            source.particles()[j].position - target.particles()[i].position;
        Real rsq = diff.lengthSquared();
        Real twoh = source.particles()[j].soft + target.particles()[i].soft;
        if (rsq != 0) {
          Real a, b;
          SPLINE(rsq, twoh, a, b);
          accel += diff * (b * source.particles()[j].mass);
        }
      }
      target.applyAcceleration(i, accel);
    }
  }

  bool cell(const SpatialNode<GravityData>& source,
            SpatialNode<GravityData>& target) {
    // Dual-walk-only hook (inert in the shipped transposed walk).
    return !Space::enclose(source.data.box, target.data.box);
  }
};

#endif  // PARATREET_GRAVITYVISITOR_H_
