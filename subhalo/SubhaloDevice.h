#ifndef SUBHALO_SUBHALO_DEVICE_H_
#define SUBHALO_SUBHALO_DEVICE_H_

// Unbinding inside FoF3 (design/subhalo-checks.md, "Step 2"). THIS HEADER
// IS A CHARM/KOKKOS FIREWALL, like fof/gpu/FoFDevice.h: it names neither,
// so FoFPhase1.h (Charm) includes it and SubhaloDevice.cpp (hipcc, Kokkos)
// never sees a Charm header. The work is unbind_groups.h's, unchanged.
//
// Threading: one caller per process at a time, on the PE that bound the
// process's device (FoFPhase1::deviceBind, which also initializes Kokkos).

#include <cstddef>
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
  // The frame of the bound set (6 per group: x, y, z in the input's
  // coordinates, in the image of the group's first particle; vx, vy, vz as
  // input) and the bound mass; zeros for a group with no sub-halo. Filled
  // by unbind() only.
  std::vector<double> frame, bound_mass;
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

// ---- Groups that stay spread over processes (part 2c; unbind_spread.h) ----
// One holder per process keeps that process's particles of every spread
// group; one driver, on one process, keeps the groups' state. They work in
// rounds: the driver's command goes to every holder, and every holder's
// report comes back to the driver, which reads them in a fixed holder
// order. The caller carries the bytes. The holder needs the device; the
// driver is host code.
struct SpreadHolder;
struct SpreadDriver;

// in: this process's particles of the spread groups, sorted by (label, id),
// as for unbind(). Copied.
SpreadHolder* spreadHolderNew(const Input& in, const Params& p);
std::vector<char> spreadHolderStep(SpreadHolder* h, const char* cmd, size_t len);
// After the final round, in the input's order. Per group: nbound, passes,
// refined_group and stale_frame are the whole group's; centre is the input
// index of the most bound particle if this holder has it, otherwise -1.
// Per particle: bound, energy, refined. wall_s is the holder's time in its
// steps, kernel_s_later its kernels' time.
void spreadHolderResults(const SpreadHolder* h, Output& o);
void spreadHolderDelete(SpreadHolder* h);

struct SpreadStats {
  int64_t groups = 0, sampled = 0, refined = 0, subhaloes = 0, bound = 0, stale = 0;
  int64_t wide = 0, largest = 0, refetches = 0;
  int rounds = 0, passes_max = 0;
  double pairs_first = 0, pairs_later = 0, pairs_refine = 0;
  double bytes_out = 0, bytes_in = 0;  // commands written, reports read
};
SpreadDriver* spreadDriverNew(const Params& p);
std::vector<char> spreadDriverStart(SpreadDriver* d);
// reports: the last round's, one per holder, always in the same holder order.
std::vector<char> spreadDriverNext(SpreadDriver* d, const std::vector<std::vector<char>>& reports);
bool spreadDriverDone(const SpreadDriver* d);  // true once the final round's reports are read
SpreadStats spreadDriverStats(const SpreadDriver* d);
// After the end, per spread group in label order.
struct SpreadGroup {
  int64_t label = -1, n = 0, nbound = 0;
  int64_t centre = -1;   // id of the most bound particle; -1 = no sub-halo
  int passes = 0;
  int holders = 0, first_holder = -1;  // holders with its particles; the first (report order)
  uint8_t refined_group = 0, stale_frame = 0, wide = 0;
  double bound_mass = 0;
  double x[3] = {0, 0, 0}, v[3] = {0, 0, 0};  // frame of the bound set, as Output::frame
};
std::vector<SpreadGroup> spreadDriverGroups(const SpreadDriver* d);
void spreadDriverDelete(SpreadDriver* d);

}  // namespace subhalodev

#endif  // SUBHALO_SUBHALO_DEVICE_H_
