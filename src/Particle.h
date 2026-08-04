#ifndef PARATREET_PARTICLE_H_
#define PARATREET_PARTICLE_H_

#include "common.h"
#include "OrientedBox.h" // (was BoundingBox.h, unused here; OrientedBox is
                         // standalone-safe — see common.h's non-Charm block)

struct Particle {
  Key key;
  // Serial number of particle, set by order in initial file (ID). 64-bit:
  // this is the GLOBAL particle id and the value domain of FoF tips; int
  // overflows at 2.147e9 particles (64-bit audit, 2026-07-25).
  long order;
  int partition_idx = 0; // Only used when Subtree and Partition have different decomp types

  Real mass;
  Real density;
  Real potential;
  //Real u;
  Real soft;
  Vector3D<Real> position;
  Vector3D<Real> acceleration;
  Vector3D<Real> velocity;
  Vector3D<Real> velocity_predicted;
  Real pressure_dVolume = 0.;
  using Effect = std::pair<Vector3D<Real>, Real>; // accel, pressure
  Real u_predicted;

  // FoF: add a field to store component number in UnionFind (groupId)
  // postIterationFn loo
  // Initialized to -1 ("never labeled") so pre-phase-1 FragData annotations
  // are deterministically min = max = -1, which the phase-3 visitor's
  // annotation-validity CkEnforce (min_frag >= 0) rejects; uninitialized
  // heap memory is often zero, which would masquerade as a valid uniform
  // fragment 0 and silently break the phase-3 certificates instead.
  long int group_number = -1;
  uint64_t vertex_id; // vertexID of current particle in unionFindLib

  enum class Type : char {
    eStar = 1,
    eGas  = 2,
    eDark = 3,
    eUnknown = 100
  };
  Type type = Type::eUnknown;

  Particle();

  bool isStar() const {return type == Type::eStar;}
  bool isGas()  const {return type == Type::eGas;}
  bool isDark() const {return type == Type::eDark;}

#ifdef __CHARMC__ // serialization only under Charm (same guard as Vector3D.h)
  void pup(PUP::er&) ;
#endif

  void reset();
  void finishInit();

  void kick(Real timestep);
  void perturb(Real timestep);
  void adjustNewUniverse(OrientedBox<Real> universe);

  bool operator==(const Particle&) const;
  bool operator<=(const Particle&) const;
  bool operator>(const Particle&) const;
  bool operator>=(const Particle&) const;
  bool operator<(const Particle&) const;
};


// Cached-particle type selection (design/cached-particle-slimming.md).
// An application's Data type may declare
//     using CachedParticle = <struct>;
// and the framework then STORES cache-shipped remote particles in that
// type (and ships them in it). The struct must be constructible from a
// full Particle (that constructor defines which fields survive) and be
// PUPable. Absent the declaration, CachedParticle is Particle and every
// code path is byte-identical to the unparameterized framework —
// non-opting applications are untouched. The old MultiData
// pupRemoteParticle wire-only mechanism is superseded by this.
template <typename D>
class CachedParticleOf {
  template <typename T> static typename T::CachedParticle test(int);
  template <typename T> static Particle test(...);
 public:
  using type = decltype(test<D>(0));
};

#endif // PARATREET_PARTICLE_H_