// unbindspec.h -- the unbinding definition of design/subhalo-checks.md
// ("What HBT-HERONS computes") in plain double-precision C++ with all pairs.
// The last part of the file adds the capped-sampling definition ("Capped
// sampling: definition and pass rule") and the centre refinement of a
// sampled group ("Centre refinement"); without a cap nothing changes.
// Shared by unbindspec, which runs it on every group, and unbindcheck, which
// replays it on the groups where a kernel and the reference differ.
//
// It is not the kernel (that is unbind_kernel.h and unbind_groups.h). It is
// kept simple on purpose: it is the written definition, and it reproduces
// HBT-HERONS's exact run on 8m-lambb to 1.1e-6 of a group's median energy
// with the same bound particles in all 13,256 groups.
//
// Needs HDF5 (the snapshot tipsy2hbt writes) and, for speed, OpenMP.

#ifndef SUBHALO_UNBINDSPEC_H
#define SUBHALO_UNBINDSPEC_H

#include <hdf5.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace ubs {

// (km/s)^2 Mpc / 1e10 Msun, HBT-HERONS's value. unbindspec --g changes it.
inline double G = 43.0071;
// 1 = the definition. unbindspec --no-hubble sets 0 to make a deliberately
// wrong result for testing the comparison tools.
inline double hubble_factor = 1.0;
// HBT-HERONS's MinNumPartOfSub.
inline const long MIN_BOUND = 20;
// Groups of at least this many particles are worth an inner parallel loop.
inline const size_t PARALLEL_FROM = 4096;

struct Group {
  size_t begin, n;                 // its particles are order[begin .. begin+n)
  long long id;                    // the snapshot's FOFGroupIDs value
};

struct Snapshot {
  double L = 0, a = 1, H = 0, eps = 0;
  long long null_id = 0;
  std::vector<double> X, V, M;     // comoving Mpc, km/s, 1e10 Msun
  std::vector<long long> ID, FOF;
  std::vector<size_t> order;       // the grouped particles, group by group
  std::vector<Group> groups;       // in decreasing size
  double half = 0, h = 0, h_inv = 0;

  double nearest(double d) const { return d > half ? d - L : (d < -half ? d + L : d); }
  // Potential of a unit mass at distance r (Gadget spline reaching h = 2.8 eps).
  double phi(double r) const {
    if (r >= h) return -1.0 / r;
    const double u = r * h_inv;
    const double wp = u < 0.5
        ? -2.8 + u * u * (5.333333333333 + u * u * (6.4 * u - 9.6))
        : -3.2 + 0.066666666667 / u +
              u * u * (10.666666666667 + u * (-16.0 + u * (9.6 - 2.133333333333 * u)));
    return wp * h_inv;
  }
  double dist(size_t p, size_t q) const {
    const double dx = nearest(X[3 * p] - X[3 * q]), dy = nearest(X[3 * p + 1] - X[3 * q + 1]),
                 dz = nearest(X[3 * p + 2] - X[3 * q + 2]);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  }
};

namespace detail {
inline double attr_d(hid_t f, const char* group, const char* name, const char* tool) {
  double v = 0;
  hid_t a = H5Aopen_by_name(f, group, name, H5P_DEFAULT, H5P_DEFAULT);
  if (a < 0 || H5Aread(a, H5T_NATIVE_DOUBLE, &v) < 0) {
    fprintf(stderr, "%s: cannot read %s:%s\n", tool, group, name);
    exit(2);
  }
  H5Aclose(a);
  return v;
}
inline std::string attr_s(hid_t f, const char* group, const char* name, const char* tool) {
  char buf[1024] = {0};
  hid_t a = H5Aopen_by_name(f, group, name, H5P_DEFAULT, H5P_DEFAULT);
  hid_t t = H5Tcreate(H5T_STRING, sizeof buf);
  H5Tset_strpad(t, H5T_STR_NULLTERM);
  if (a < 0 || H5Aread(a, t, buf) < 0) {
    fprintf(stderr, "%s: cannot read %s:%s\n", tool, group, name);
    exit(2);
  }
  H5Tclose(t);
  H5Aclose(a);
  return buf;
}
template <class T>
inline std::vector<T> dset(hid_t f, const char* name, hid_t type, size_t per, const char* tool) {
  hid_t d = H5Dopen2(f, name, H5P_DEFAULT);
  if (d < 0) { fprintf(stderr, "%s: no dataset %s\n", tool, name); exit(2); }
  hid_t sp = H5Dget_space(d);
  hsize_t dims[2] = {0, 1};
  H5Sget_simple_extent_dims(sp, dims, NULL);
  std::vector<T> v(dims[0] * per);
  if (H5Dread(d, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, v.data()) < 0) {
    fprintf(stderr, "%s: cannot read %s\n", tool, name);
    exit(2);
  }
  H5Sclose(sp);
  H5Dclose(d);
  return v;
}
}  // namespace detail

// Read the file tipsy2hbt wrote and list every group's particles.
inline Snapshot readSnapshot(const char* path, const char* tool) {
  using namespace detail;
  Snapshot s;
  hid_t f = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (f < 0) { fprintf(stderr, "%s: cannot open %s\n", tool, path); exit(2); }
  double box3[3] = {0, 0, 0};
  {
    hid_t a = H5Aopen_by_name(f, "Header", "BoxSize", H5P_DEFAULT, H5P_DEFAULT);
    if (a < 0 || H5Aread(a, H5T_NATIVE_DOUBLE, box3) < 0) {
      fprintf(stderr, "%s: cannot read Header:BoxSize\n", tool);
      exit(2);
    }
    H5Aclose(a);
  }
  s.L = box3[0];
  s.half = 0.5 * s.L;
  s.a = attr_d(f, "Cosmology", "Scale-factor", tool);
  s.H = attr_d(f, "Cosmology", "H [internal units]", tool);
  s.eps = std::min(
      atof(attr_s(f, "Parameters", "Gravity:comoving_DM_softening", tool).c_str()),
      atof(attr_s(f, "Parameters", "Gravity:max_physical_DM_softening", tool).c_str()) / s.a);
  s.h = 2.8 * s.eps;
  s.h_inv = 1.0 / s.h;
  s.null_id = atoll(attr_s(f, "Parameters", "FOF:group_id_default", tool).c_str());
  s.X = dset<double>(f, "/PartType1/Coordinates", H5T_NATIVE_DOUBLE, 3, tool);
  s.V = dset<double>(f, "/PartType1/Velocities", H5T_NATIVE_DOUBLE, 3, tool);
  s.M = dset<double>(f, "/PartType1/Masses", H5T_NATIVE_DOUBLE, 1, tool);
  s.ID = dset<long long>(f, "/PartType1/ParticleIDs", H5T_NATIVE_LLONG, 1, tool);
  s.FOF = dset<long long>(f, "/PartType1/FOFGroupIDs", H5T_NATIVE_LLONG, 1, tool);
  H5Fclose(f);

  const size_t N = s.M.size();
  for (size_t i = 0; i < N; i++)
    if (s.FOF[i] != s.null_id) s.order.push_back(i);
  std::sort(s.order.begin(), s.order.end(), [&](size_t p, size_t q) {
    return s.FOF[p] != s.FOF[q] ? s.FOF[p] < s.FOF[q] : p < q;
  });
  for (size_t i = 0; i < s.order.size();) {
    size_t j = i;
    while (j < s.order.size() && s.FOF[s.order[j]] == s.FOF[s.order[i]]) j++;
    s.groups.push_back({i, j - i, s.FOF[s.order[i]]});
    i = j;
  }
  std::sort(s.groups.begin(), s.groups.end(), [](const Group& p, const Group& q) {
    return p.n != q.n ? p.n > q.n : p.id < q.id;
  });
  return s;
}

// Potential (without G) at each of the particles idx[0..n) from all the
// others: n^2 pair terms.
inline void potentialAll(const Snapshot& s, const size_t* idx, size_t n,
                         std::vector<double>& pot, bool parallel) {
  pot.assign(n, 0.0);
#pragma omp parallel for schedule(dynamic, 64) if (parallel)
  for (size_t i = 0; i < n; i++) {
    double sum = 0;
    for (size_t j = 0; j < n; j++)
      if (j != i) sum += s.M[idx[j]] * s.phi(s.dist(idx[i], idx[j]));
    pot[i] = sum;
  }
}

// Take the potential of the particles `src` out of pot[i], for every i with
// keep[i].
inline void subtractPotential(const Snapshot& s, const size_t* idx, size_t n,
                              const std::vector<char>& keep, const std::vector<size_t>& src,
                              std::vector<double>& pot, bool parallel) {
#pragma omp parallel for schedule(dynamic, 64) if (parallel)
  for (size_t i = 0; i < n; i++) {
    if (!keep[i]) continue;
    double sum = 0;
    for (size_t r : src) sum += s.M[idx[r]] * s.phi(s.dist(idx[i], idx[r]));
    pot[i] -= sum;
  }
}

// Energy of every particle with keep[i], in the frame of those particles:
// mass-weighted mean position (nearest-image offsets from the first of them)
// and velocity. pot[] must be the potential from exactly those particles.
//
// origin, if given, is the particle (0..n) the nearest-image offsets of the
// mean position are taken from instead of the first kept one. It changes
// the frame only in a group at least half the box wide. HBT-HERONS's first
// frame of a group takes them from the first particle of a list that is not
// sorted yet ("The first frame of a wide group depends on its first
// particle").
inline const size_t NO_ORIGIN = (size_t)-1;
inline void energies(const Snapshot& s, const size_t* idx, size_t n,
                     const std::vector<char>& keep, const std::vector<double>& pot,
                     std::vector<double>& e, size_t origin = NO_ORIGIN) {
  size_t first = 0;
  while (first < n && !keep[first]) first++;
  if (first == n) return;
  if (origin != NO_ORIGIN) first = origin;
  double ms = 0, xs[3] = {0, 0, 0}, vs[3] = {0, 0, 0};
  for (size_t i = 0; i < n; i++) {
    if (!keep[i]) continue;
    const double m = s.M[idx[i]];
    ms += m;
    for (int k = 0; k < 3; k++) {
      xs[k] += s.nearest(s.X[3 * idx[i] + k] - s.X[3 * idx[first] + k]) * m;
      vs[k] += s.V[3 * idx[i] + k] * m;
    }
  }
  double xc[3], vc[3];
  for (int k = 0; k < 3; k++) {
    xc[k] = xs[k] / ms + s.X[3 * idx[first] + k];
    vc[k] = vs[k] / ms;
  }
  const double Ha = s.H * s.a * hubble_factor;
  for (size_t i = 0; i < n; i++) {
    if (!keep[i]) continue;
    double k2 = 0;
    for (int k = 0; k < 3; k++) {
      const double dv = s.V[3 * idx[i] + k] - vc[k] + Ha * s.nearest(s.X[3 * idx[i] + k] - xc[k]);
      k2 += dv * dv;
    }
    e[i] = 0.5 * k2 + pot[i] * G / s.a;
  }
}

// The passes. On entry alive[] marks the starting set and pot[] holds its
// potential (potentialAll). Each pass computes the energies, removes every
// particle with E >= 0, takes the removed particles' potential out of the
// rest and moves the frame. It stops when a pass removes nothing; if fewer
// than MIN_BOUND particles remain, the group has no sub-halo and alive[] is
// cleared. Returns the number of passes.
//
// obs(pass, bound_entering, alive, e, removed) is called once per pass,
// after the energies are known and before the removal: alive[] is the set
// entering the pass, e[] its energies, removed the particles about to go.
template <class Observer>
inline long unbind(const Snapshot& s, const size_t* idx, size_t n, std::vector<char>& alive,
                   std::vector<double>& pot, std::vector<double>& e, bool parallel,
                   Observer&& obs, size_t first_origin = NO_ORIGIN) {
  size_t nalive = 0;
  for (size_t i = 0; i < n; i++) nalive += alive[i] ? 1 : 0;
  long npass = 0;
  std::vector<size_t> removed;
  for (;;) {
    energies(s, idx, n, alive, pot, e, npass == 0 ? first_origin : NO_ORIGIN);
    removed.clear();
    for (size_t i = 0; i < n; i++)
      if (alive[i] && !(e[i] < 0)) removed.push_back(i);
    npass++;
    obs(npass, nalive, alive, e, removed);
    if (removed.empty()) break;
    for (size_t r : removed) alive[r] = 0;
    nalive -= removed.size();
    if ((long)nalive < MIN_BOUND) {
      std::fill(alive.begin(), alive.end(), 0);
      break;
    }
    subtractPotential(s, idx, n, alive, removed, pot, parallel);
  }
  return npass;
}

// ---- capped sampling -------------------------------------------------------
// design/subhalo-checks.md, "Capped sampling: definition and pass rule". With
// a cap C, a pass whose bound set has more than C particles takes each
// particle's potential from the first C bound particles in the order below,
// their masses multiplied by f = M(bound) / M(sample).

// The order of a group's particles for sampling: by this hash of the
// particle id, ties by id.
inline unsigned long long sampleKey(long long id) {
  unsigned long long z = (unsigned long long)id + 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}
// Local indices 0..n of the group's particles in that order.
inline std::vector<size_t> sampleOrder(const Snapshot& s, const size_t* idx, size_t n) {
  std::vector<size_t> o(n);
  for (size_t i = 0; i < n; i++) o[i] = i;
  std::sort(o.begin(), o.end(), [&](size_t p, size_t q) {
    const unsigned long long kp = sampleKey(s.ID[idx[p]]), kq = sampleKey(s.ID[idx[q]]);
    return kp != kq ? kp < kq : s.ID[idx[p]] < s.ID[idx[q]];
  });
  return o;
}

// Potential (without G) at each particle with alive[i], by the capped
// definition applied to the alive set: from all the other alive particles if
// there are at most `cap` of them, otherwise from the sample, scaled by f.
inline void potentialCapped(const Snapshot& s, const size_t* idx, size_t n,
                            const std::vector<char>& alive, const std::vector<size_t>& order,
                            size_t cap, std::vector<double>& pot, bool parallel) {
  size_t nalive = 0;
  double mall = 0;
  for (size_t i = 0; i < n; i++)
    if (alive[i]) { nalive++; mall += s.M[idx[i]]; }
  const size_t want = nalive <= cap ? nalive : cap;
  std::vector<size_t> src;
  double msrc = 0;
  for (size_t k = 0; k < n && src.size() < want; k++)
    if (alive[order[k]]) { src.push_back(order[k]); msrc += s.M[idx[order[k]]]; }
  const double f = nalive <= cap ? 1.0 : mall / msrc;
  pot.assign(n, 0.0);
#pragma omp parallel for schedule(dynamic, 64) if (parallel)
  for (size_t i = 0; i < n; i++) {
    if (!alive[i]) continue;
    double sum = 0;
    for (size_t r : src)
      if (r != i) sum += s.M[idx[r]] * s.phi(s.dist(idx[i], idx[r]));
    pot[i] = f * sum;
  }
}

// The passes with a cap: as unbind() above, but every pass takes its
// potential from potentialCapped on the current bound set, so nothing is
// carried from one pass to the next. pot[] need not be set on entry; on
// return it holds the last pass's potential.
template <class Observer>
inline long unbindCapped(const Snapshot& s, const size_t* idx, size_t n,
                         std::vector<char>& alive, std::vector<double>& pot,
                         std::vector<double>& e, bool parallel, size_t cap,
                         const std::vector<size_t>& order, Observer&& obs) {
  size_t nalive = 0;
  for (size_t i = 0; i < n; i++) nalive += alive[i] ? 1 : 0;
  long npass = 0;
  std::vector<size_t> removed;
  for (;;) {
    potentialCapped(s, idx, n, alive, order, cap, pot, parallel);
    energies(s, idx, n, alive, pot, e);
    removed.clear();
    for (size_t i = 0; i < n; i++)
      if (alive[i] && !(e[i] < 0)) removed.push_back(i);
    npass++;
    obs(npass, nalive, alive, e, removed);
    if (removed.empty()) break;
    for (size_t r : removed) alive[r] = 0;
    nalive -= removed.size();
    if ((long)nalive < MIN_BOUND) {
      std::fill(alive.begin(), alive.end(), 0);
      break;
    }
  }
  return npass;
}

// ---- the passes as HBT-HERONS's source carries them -------------------------
// subhalo_unbind.cpp (c728ee0), the loop of Subhalo_t::Unbind. unbind() above
// computes every pass's energies afresh from the current frame. HBT-HERONS
// does that only until the first pass that removes fewer particles than it
// keeps. From then on every pass is a correction of the stored energies:
//
//   E_i += w_i . d + |d|^2 / 2 - (G / a) * (potential of the removed particles)
//
// with w_i the particle's velocity in the OLD frame (Hubble term included,
// nearest image taken with respect to the old frame) and d the old frame's
// velocity in the new one. That equals the fresh value unless a particle's
// nearest image changes when the frame moves, which needs the particle half a
// box from the frame: a group at least half the box wide.
//
// Two things are kept exactly as the source has them:
//   - once a pass was a correction, every later pass is one;
//   - the old frame is stored only after a pass that removes fewer particles
//     than it keeps. After a later pass that removes at least as many as it
//     keeps, the next correction still uses the frame stored before, which
//     is then not the frame the stored energies are in.
//
// Arguments as unbind(). pot[] is kept equal to the potential from the alive
// particles; e[] holds the carried energies. *stale_frame is set if the
// second case above happened in this group. full_after_large_removal leaves
// HBT-HERONS there and does what the kernel (unbind_groups.h) does on
// purpose: the pass after one that removed at least as many particles as it
// kept is computed in full.
inline void meanFrame(const Snapshot& s, const size_t* idx, size_t n,
                      const std::vector<char>& keep, size_t origin, double xc[3],
                      double vc[3]) {
  size_t first = 0;
  while (first < n && !keep[first]) first++;
  if (first == n) return;
  if (origin != NO_ORIGIN) first = origin;
  double ms = 0, xs[3] = {0, 0, 0}, vs[3] = {0, 0, 0};
  for (size_t i = 0; i < n; i++) {
    if (!keep[i]) continue;
    const double m = s.M[idx[i]];
    ms += m;
    for (int k = 0; k < 3; k++) {
      xs[k] += s.nearest(s.X[3 * idx[i] + k] - s.X[3 * idx[first] + k]) * m;
      vs[k] += s.V[3 * idx[i] + k] * m;
    }
  }
  for (int k = 0; k < 3; k++) {
    xc[k] = xs[k] / ms + s.X[3 * idx[first] + k];
    vc[k] = vs[k] / ms;
  }
}

template <class Observer>
inline long unbindCarried(const Snapshot& s, const size_t* idx, size_t n,
                          std::vector<char>& alive, std::vector<double>& pot,
                          std::vector<double>& e, bool parallel, Observer&& obs,
                          size_t first_origin = NO_ORIGIN, bool* stale_frame = nullptr,
                          bool full_after_large_removal = false) {
  if (stale_frame) *stale_frame = false;
  size_t nalive = 0;
  for (size_t i = 0; i < n; i++) nalive += alive[i] ? 1 : 0;
  const double Ha = s.H * s.a * hubble_factor, Ga = G / s.a;
  // Velocity of (x, v) in the frame (rx, rv), Hubble term included.
  auto relative = [&](const double* x, const double* v, const double* rx, const double* rv,
                      double* out) {
    for (int k = 0; k < 3; k++) out[k] = v[k] - rv[k] + Ha * s.nearest(x[k] - rx[k]);
  };
  double ref_x[3] = {0, 0, 0}, ref_v[3] = {0, 0, 0}, old_x[3] = {0, 0, 0}, old_v[3] = {0, 0, 0};
  meanFrame(s, idx, n, alive, first_origin, ref_x, ref_v);
  bool correction = false;
  long npass = 0;
  std::vector<size_t> removed;
  std::vector<double> gone(n, 0.0);   // potential of the last pass's removed particles
  for (;;) {
    if (correction) {
      double d[3];
      relative(old_x, old_v, ref_x, ref_v, d);
      const double dK = 0.5 * (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      for (size_t i = 0; i < n; i++) {
        if (!alive[i]) continue;
        double w[3];
        relative(&s.X[3 * idx[i]], &s.V[3 * idx[i]], old_x, old_v, w);
        e[i] += w[0] * d[0] + w[1] * d[1] + w[2] * d[2] + dK - Ga * gone[i];
      }
    } else {
      for (size_t i = 0; i < n; i++) {
        if (!alive[i]) continue;
        double w[3];
        relative(&s.X[3 * idx[i]], &s.V[3 * idx[i]], ref_x, ref_v, w);
        e[i] = 0.5 * (w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) + Ga * pot[i];
      }
    }
    removed.clear();
    for (size_t i = 0; i < n; i++)
      if (alive[i] && !(e[i] < 0)) removed.push_back(i);
    npass++;
    obs(npass, nalive, alive, e, removed);
    if (removed.empty()) break;
    for (size_t r : removed) alive[r] = 0;
    nalive -= removed.size();
    if ((long)nalive < MIN_BOUND) {
      std::fill(alive.begin(), alive.end(), 0);
      break;
    }
    std::fill(gone.begin(), gone.end(), 0.0);
#pragma omp parallel for schedule(dynamic, 64) if (parallel)
    for (size_t i = 0; i < n; i++) {
      if (!alive[i]) continue;
      double sum = 0;
      for (size_t r : removed) sum += s.M[idx[r]] * s.phi(s.dist(idx[i], idx[r]));
      gone[i] = sum;
      pot[i] -= sum;
    }
    if (removed.size() < nalive) {
      correction = true;
      for (int k = 0; k < 3; k++) { old_x[k] = ref_x[k]; old_v[k] = ref_v[k]; }
    } else if (correction) {
      if (stale_frame) *stale_frame = true;
      // HBT-HERONS: the next pass is still a correction, from a frame two
      // passes old. unbind (the kernel): the next pass is computed in full.
      if (full_after_large_removal) correction = false;
    }
    meanFrame(s, idx, n, alive, NO_ORIGIN, ref_x, ref_v);
  }
  return npass;
}

// ---- centre refinement --------------------------------------------------------
// design/subhalo-checks.md, "Centre refinement". A sampled group's energies
// are estimates, so its particle of lowest energy need not be the most bound
// one. After the last pass of a group whose bound set has more than the cap,
// the k bound particles of lowest energy (ties by particle id; all of them if
// there are fewer) get their energy again, from ALL the bound particles with
// their true masses, own term excluded, in the frame of the bound set (the
// frame of energies(): mass-weighted means, offsets from the first bound
// particle). The most bound particle is the one of lowest such energy, ties
// by id.
//
// alive[] is the final bound set and e[] the last pass's energies. On return
// cand[] holds the k particles (local indices) in the order of their new
// energies, ecand[] those energies and rank[] each one's position in the
// order of e[] (0 = lowest). k * (bound count) pair terms.
inline void refineCentre(const Snapshot& s, const size_t* idx, size_t n,
                         const std::vector<char>& alive, const std::vector<double>& e,
                         size_t k, std::vector<size_t>& cand, std::vector<double>& ecand,
                         std::vector<size_t>& rank, bool parallel) {
  std::vector<size_t> b;
  for (size_t i = 0; i < n; i++)
    if (alive[i]) b.push_back(i);
  const size_t m = std::min(k, b.size());
  std::partial_sort(b.begin(), b.begin() + m, b.end(), [&](size_t p, size_t q) {
    return e[p] != e[q] ? e[p] < e[q] : s.ID[idx[p]] < s.ID[idx[q]];
  });
  std::vector<size_t> bound;
  for (size_t i = 0; i < n; i++)
    if (alive[i]) bound.push_back(i);
  double xc[3] = {0, 0, 0}, vc[3] = {0, 0, 0};
  meanFrame(s, idx, n, alive, NO_ORIGIN, xc, vc);
  const double Ha = s.H * s.a * hubble_factor;
  std::vector<double> er(m);
#pragma omp parallel for schedule(dynamic, 1) if (parallel)
  for (size_t c = 0; c < m; c++) {
    const size_t i = b[c];
    double sum = 0;
    for (size_t j : bound)
      if (j != i) sum += s.M[idx[j]] * s.phi(s.dist(idx[i], idx[j]));
    double k2 = 0;
    for (int d = 0; d < 3; d++) {
      const double dv = s.V[3 * idx[i] + d] - vc[d] + Ha * s.nearest(s.X[3 * idx[i] + d] - xc[d]);
      k2 += dv * dv;
    }
    er[c] = 0.5 * k2 + sum * G / s.a;
  }
  std::vector<size_t> o(m);
  for (size_t c = 0; c < m; c++) o[c] = c;
  std::sort(o.begin(), o.end(), [&](size_t p, size_t q) {
    return er[p] != er[q] ? er[p] < er[q] : s.ID[idx[b[p]]] < s.ID[idx[b[q]]];
  });
  cand.resize(m);
  ecand.resize(m);
  rank.resize(m);
  for (size_t r = 0; r < m; r++) {
    cand[r] = b[o[r]];
    ecand[r] = er[o[r]];
    rank[r] = o[r];
  }
}

// ---- the first frame's origin, for unbindcheck ------------------------------
// A file of lines "group id, particle id" (unbind --first-origin, unbindspec
// --first-origin; wideorigins.py hbt or first_origins.py write it from an
// HBT-HERONS run) names, per group, the particle the first frame's offsets
// are taken from.
inline std::unordered_map<long long, long long> readFirstOrigins(const char* path,
                                                                 const char* tool) {
  std::unordered_map<long long, long long> m;
  FILE* f = fopen(path, "r");
  if (!f) { fprintf(stderr, "%s: cannot open %s\n", tool, path); exit(2); }
  char line[256];
  while (fgets(line, sizeof line, f)) {
    long long g, id;
    if (line[0] != '#' && sscanf(line, "%lld %lld", &g, &id) == 2) m[g] = id;
  }
  fclose(f);
  return m;
}
// The named particle's position in its group, NO_ORIGIN if the group is not
// named; exits if the particle is not in the group.
inline size_t firstOrigin(const Snapshot& s, const Group& g,
                          const std::unordered_map<long long, long long>& origins,
                          const char* tool) {
  auto it = origins.find(g.id);
  if (it == origins.end()) return NO_ORIGIN;
  const size_t* idx = s.order.data() + g.begin;
  for (size_t i = 0; i < g.n; i++)
    if (s.ID[idx[i]] == it->second) return i;
  fprintf(stderr, "%s: --first-origin: particle %lld is not in group %lld\n", tool,
          it->second, g.id);
  exit(2);
}

}  // namespace ubs

#endif
