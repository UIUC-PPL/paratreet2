// uniform: generate N particles with positions uniform-random in the unit
// box [0,1)^3, equal masses (1/N), zero velocities, in the same raw .dat
// format plummer.cpp writes (preamble: int nbody, int ndims, Real tnow;
// then REALS_PER_PARTICLE=8 Reals per particle: pos xyz, vel xyz, mass,
// softening — see common.h and plummer.cpp::writeToDisk). Convert with
// ./tipsyPlummer out.dat out.tipsy exactly as for Plummer data.
//
// Usage: ./uniform <seed> <nbody> <output .dat file>
//
// RNG: std::mt19937_64 seeded from the CLI arg, mapped to [0,1) doubles via
// (x >> 11) * 2^-53 (implementation-independent, so a given seed reproduces
// the same file everywhere), then narrowed to Real (float).

#include "common.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <vector>

using namespace std;

int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: ./uniform <seed> <nbody> <output .dat file>\n");
    return 1;
  }
  unsigned long long seed = strtoull(argv[1], nullptr, 10);
  long nbody = strtol(argv[2], nullptr, 10);
  assert(nbody > 0 && nbody <= INT_MAX);

  ofstream out(argv[3], ios::out | ios::binary);
  assert(!out.bad() && !out.fail() && out.is_open());

  int nbody_i = (int)nbody;
  int ndims = 3;
  Real tnow = 0.0;
  out.write((char*)&nbody_i, sizeof(int));
  out.write((char*)&ndims, sizeof(int));
  out.write((char*)&tnow, sizeof(Real));

  mt19937_64 rng(seed);
  auto u01 = [&rng]() -> double {
    return (double)(rng() >> 11) * (1.0 / 9007199254740992.0); // 2^-53
  };

  const Real mass = (Real)(1.0 / (double)nbody);
  const Real soft = 0.001;
  const long bufParticles = 1L << 18;
  vector<Real> buf;
  buf.reserve(bufParticles * REALS_PER_PARTICLE);

  long remaining = nbody;
  while (remaining > 0) {
    long cur = remaining < bufParticles ? remaining : bufParticles;
    buf.clear();
    for (long i = 0; i < cur; i++) {
      buf.push_back((Real)u01()); // x
      buf.push_back((Real)u01()); // y
      buf.push_back((Real)u01()); // z
      buf.push_back(0.0);         // vx
      buf.push_back(0.0);         // vy
      buf.push_back(0.0);         // vz
      buf.push_back(mass);
      buf.push_back(soft);
    }
    out.write((char*)buf.data(), cur * REALS_PER_PARTICLE * sizeof(Real));
    remaining -= cur;
  }
  out.close();
  fprintf(stderr, "uniform: wrote %ld particles (seed %llu) to %s\n",
          nbody, seed, argv[3]);
  return 0;
}
