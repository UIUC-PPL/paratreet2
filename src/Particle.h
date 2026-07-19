#ifndef PARATREET_PARTICLE_H_
#define PARATREET_PARTICLE_H_

#include "common.h"
#include "BoundingBox.h"

struct Particle {
  Key key;
  int order; // serial number of particle, set by order in initial file (ID)
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

  void pup(PUP::er&) ;

  void reset();
  void finishInit();

  //void kick(Real timestep);
  //void perturb(Real timestep);
  void adjustNewUniverse(OrientedBox<Real> universe);

  bool operator==(const Particle&) const;
  bool operator<=(const Particle&) const;
  bool operator>(const Particle&) const;
  bool operator>=(const Particle&) const;
  bool operator<(const Particle&) const;
};

#endif // PARATREET_PARTICLE_H_
