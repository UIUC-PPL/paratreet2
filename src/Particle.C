#include "Particle.h"

Particle::Particle() : key(Key(0)) {
  reset();
}

// Kick-drift-kick leapfrog step, restored 2026-08-04 for the gravity
// example (design/barnes-hut-app.md) — both halves were commented out in
// the inherited code, so perturb_particles apps computed forces but never
// moved anything. Driver calls kick then (after postIterationFn) perturb,
// so one iteration = half-kick + half-kick + drift; acceleration is zeroed
// here for the next iteration's traversal to accumulate afresh. The
// internal-energy (u) lines of the original body are dropped: the u field
// itself is commented out of Particle (SPH-only; restore both together).
void Particle::kick(Real timestep) {
  velocity += acceleration * timestep / 2;
}

void Particle::perturb(Real timestep) {
  velocity += (acceleration * timestep / 2);
  velocity_predicted = velocity + (acceleration * timestep);
  acceleration = Vector3D<Real>(0.0, 0.0, 0.0);
  position += (velocity * timestep);
  density = 0;
  pressure_dVolume = 0.;
}

void Particle::adjustNewUniverse(OrientedBox<Real> universe) {
  for (int dim = 0; dim < 3; dim++) {
    CkAssert(std::isfinite(position[dim]));
    while (position[dim] < universe.lesser_corner[dim]) {
      position[dim] += universe.greater_corner[dim] - universe.lesser_corner[dim];
    }
    while (position[dim] > universe.greater_corner[dim]) {
      position[dim] -= universe.greater_corner[dim] - universe.lesser_corner[dim];
    }
  }
  key = SFC::generateKey(position, universe);
  key |= (Key)1 << (KEY_BITS-1); // Add placeholder bit
}


void Particle::pup(PUP::er &p) {
  p|key;
  p|order;
  p|partition_idx;
  p|mass;
  p|density;
  p|potential;
  //p|u;
  p|u_predicted;
  p|pressure_dVolume;
  p|position;
  p|acceleration;
  p|velocity;
  p|velocity_predicted;
  p|soft;
  p|type;
  p|group_number;
  p|vertex_id;
}

void Particle::reset() {
  pressure_dVolume = 0.0;
  density       = 0.0;
  acceleration  = Vector3D<Real> (0.0, 0.0, 0.0);
}

bool Particle::operator==(const Particle& other) const {
  return key == other.key;
}

bool Particle::operator<=(const Particle& other) const {
  return key <= other.key;
}

bool Particle::operator>(const Particle& other) const {
  return !(*this <= other);
}

bool Particle::operator>=(const Particle& other) const {
  return key >= other.key;
}

bool Particle::operator<(const Particle& other) const {
  return !(*this >= other);
}
