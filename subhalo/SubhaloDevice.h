#ifndef SUBHALO_SUBHALO_DEVICE_H_
#define SUBHALO_SUBHALO_DEVICE_H_

// Unbinding inside FoF3 (design/subhalo-checks.md, "Step 2"). THIS HEADER
// IS A CHARM/KOKKOS FIREWALL, like fof/gpu/FoFDevice.h: it names neither,
// so FoFPhase1.h (Charm) includes it and SubhaloDevice.cpp (hipcc, Kokkos)
// never sees a Charm header. The work is unbind_groups.h's, unchanged.
//
// Threading: one caller per process at a time, on the PE that bound the
// process's device (FoFPhase1::deviceBind, which also initializes Kokkos).

#include <cstdint>
#include <vector>

namespace subhalodev {

struct Params {
  double G = 1;      // in the units of the positions, velocities and masses
  double a = 1;      // scale factor
  double H = 0;      // Hubble rate at a, velocity per comoving length
  double eps = 0;    // comoving softening length (> 0)
  double box = 0;    // comoving periodic box side; 0 = open
  double bound_fraction = 0.995;
  int min_bound = 20;
  int64_t cap = 10000;         // most sources per particle; 0 = all pairs
  int64_t refine = 1000;       // centre refinement candidates; 0 = off
  int64_t refine_chunk = 1024; // the refinement kernel's chunk (unbind_kernel.h)
  int kernel = 0;              // 0 f64, 1 f32 summed in f64, 2 f32 summed in f32
  bool rsqrt = false;
};

// Whole groups, sorted by (label, id): a group is a run of equal labels.
// Labels and ids are >= 0. pos: comoving, 3 per particle, any one image
// (offsets are taken by nearest image within the group); vel: physical
// peculiar, 3 per particle; mass: 1 per particle.
struct Input {
  int64_t n = 0;
  const int64_t* label = nullptr;
  const int64_t* id = nullptr;
  const double* pos = nullptr;
  const double* vel = nullptr;
  const double* mass = nullptr;
};

struct Output {
  // Per group of the input, in input order (groups smaller than min_bound
  // are listed with no sub-halo).
  std::vector<int64_t> label, first, count;  // first, count: its run in the input
  std::vector<int64_t> nbound;               // 0 = no sub-halo
  std::vector<int64_t> centre;               // input index of the most bound particle; -1
  std::vector<int> passes;
  std::vector<uint8_t> refined_group, stale_frame;
  // Per particle of the input, in input order.
  std::vector<uint8_t> bound;
  std::vector<double> energy;   // as last evaluated
  std::vector<double> refined;  // centre refinement's energy; NaN = not refined
  // Totals.
  int64_t wide_groups = 0, sampled_groups = 0, refined_groups = 0;
  double pairs_first = 0, pairs_later = 0, pairs_refine = 0;
  double kernel_s_first = 0, kernel_s_later = 0, kernel_s_refine = 0;
  double host_s = 0, wall_s = 0;
  int rounds = 0;
};

// True when a real device backend is compiled in.
bool available();
const char* backendName();
// Kokkos must be initialized (FoFPhase1::deviceBind). Aborts on bad input.
void unbind(const Input& in, const Params& p, Output& out);

}  // namespace subhalodev

#endif  // SUBHALO_SUBHALO_DEVICE_H_
