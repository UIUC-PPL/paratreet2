#ifndef PARATREET_PARTICLEMSG_H_
#define PARATREET_PARTICLEMSG_H_

#include "Particle.h"
#include "common.h"
#include "paratreet.decl.h"

struct ParticleMsg : public CMessage_ParticleMsg {
  Particle* particles;
  int n_particles;
  // PE of the Reader group branch owed an ack for these particles, or -1 for
  // "no ack expected". Only Reader::flush's windowed path sets it; every other
  // producer (Reader::request, Partition::flush) leaves it -1 and is unchanged.
  int sender;

  ParticleMsg();
  ParticleMsg(Particle* p, int n);
};

inline ParticleMsg::ParticleMsg() {
  particles = nullptr;
  n_particles = 0;
  sender = -1;
}

inline ParticleMsg::ParticleMsg(Particle* p, int n) {
  memcpy(particles, p, n * sizeof(Particle));
  n_particles = n;
  sender = -1;
}

#endif // PARATREET_PARTICLEMSG_H_
