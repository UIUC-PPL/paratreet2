// unbind_groups.h -- unbind every group, by HBT-HERONS's rules, with the
// pair terms on the device (design/subhalo-checks.md, step 1).
//
// Unbinding removes from a group every particle whose kinetic plus
// potential energy, in the group's own frame, is not negative, and repeats
// until the group stops changing. The rules below are HBT-HERONS's with
// sampling switched off (src/subhalo_unbind.cpp, Subhalo_t::Unbind, commit
// c728ee0, MaxSampleSizeOfPotentialEstimate 0):
//
//   first frame    mass-weighted mean position and velocity of the group
//   energy         0.5 |v - v_frame + H a (x - x_frame)|^2 + (G / a) phi,
//                  x comoving, v physical, phi from unbind_kernel.h
//   bound          energy < 0
//   dead           fewer than min_bound particles left: no sub-halo
//   later frames   the same means over the particles still bound
//   stop           a pass kept at least bound_fraction of its particles
//                  (compared in single precision, as HBT-HERONS does)
//   next pass      if the pass removed fewer particles than it kept, only
//                  the removed particles' potential is subtracted and the
//                  kinetic term is moved to the new frame; otherwise the
//                  energies are computed again in full
//
// All groups advance together: each pass is at most two kernel launches,
// one over the groups that need a full pass and one over those that need a
// subtraction.
//
// One known difference from HBT-HERONS: once it has switched to subtraction
// it never switches back, and a later pass that removes more than it keeps
// then moves the kinetic term from a frame two passes old. Here that pass is
// computed in full. The groups this happens to are counted (stale_frame).
//
// Capped sampling (Params::cap, design/subhalo-checks.md "Capped sampling:
// definition and pass rule"). With a cap C, a pass whose bound set has more
// than C particles takes each particle's potential from the first C bound
// particles in the order of sampleKey(id), their masses times
// f = M(bound) / M(sample); a sampled particle's own term is left out. The
// energies of a pass depend only on its bound set. Here each particle's sum
// over the sample is kept without f; after a pass the removed sample members
// are taken out of it and the particles that replace them added (one launch
// with the removed members' masses negated), or it is computed again when
// half the sample changed. A group at or below the cap goes by the rules
// above.
//
// Host code is serial. Needs unbind_kernel.h; no HDF5, no Charm header.

#ifndef SUBHALO_UNBIND_GROUPS_H_
#define SUBHALO_UNBIND_GROUPS_H_

#include "unbind_kernel.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

namespace unbind {

// Groups packed one after another. Positions are comoving offsets from the
// group's first particle (nearest image in a periodic box).
struct Groups {
  std::vector<int64_t> off;       // group g holds packed particles off[g] .. off[g+1]
  std::vector<int64_t> id;        // the caller's id of each group
  std::vector<int64_t> particle;  // the caller's index of each packed particle
  std::vector<double> pos;        // 3 per particle
  std::vector<double> vel;        // 3 per particle, physical
  std::vector<double> mass;
  std::vector<int64_t> pid;       // the id each particle is sampled by (Params::cap)
  bool wrap = false;       // a group is at least half the box wide in some direction
  int64_t wide_groups = 0; // how many
  // Optional, per group: the packed particle the FIRST frame's nearest-image
  // offsets are taken from; -1 or an empty vector = the group's first
  // particle. It matters only for a group at least half the box wide.
  // HBT-HERONS takes the first particle of its merged list there, before it
  // sorts the list by id (design/subhalo-checks.md, "Groups at least half
  // the box wide").
  std::vector<int64_t> first_origin;
  int ng() const { return (int)id.size(); }
  int64_t n() const { return off.empty() ? 0 : off.back(); }
};

struct Params {
  double G = 1;      // in the units of the positions, velocities and masses
  double a = 1;      // scale factor
  double H = 0;      // Hubble rate at a, velocity per physical length
  double eps = 0;    // comoving softening length (> 0)
  double box = 0;    // comoving periodic box side; 0 = open
  double bound_fraction = 0.995;
  int min_bound = 20;
  int64_t cap = 0;   // most sources per particle; 0 = no cap (all pairs)
};

struct Stats {
  int passes = 0;                // rounds over all groups still active
  int64_t pairs_first = 0;       // pair terms of the first pass (all pairs)
  int64_t pairs_full_later = 0;  // later passes computed in full
  int64_t pairs_subtract = 0;    // later passes done by subtraction
  int64_t pairs_sample_update = 0;  // later passes of sampled groups: the sample's change
  int64_t sampled_groups = 0;    // groups above the cap
  double kernel_s_first = 0, kernel_s_later = 0;
  double host_s = 0;             // packing, copies to and from the device, removal
};

struct Result {
  std::vector<int64_t> nbound;  // per group; 0 = no sub-halo
  std::vector<int> passes;      // per group, energy evaluations
  std::vector<uint8_t> stale_frame;  // per group, see the header comment
  std::vector<uint8_t> bound;   // per packed particle
  std::vector<double> energy;   // per packed particle, as last evaluated
  Stats stats;
};

inline double nearestImage(double d, double box) {
  const double half = 0.5 * box;
  return d > half ? d - box : (d < -half ? d + box : d);
}

// Pack particles into groups. gid[i] < 0 or == null_id means "no group";
// groups smaller than min_size are dropped. pos is 3 doubles per particle,
// comoving, anywhere within one box length of each other; vel and mass are
// any arithmetic type. pid, if given, is each particle's id for sampling;
// otherwise the particle's index is used.
template <class V, class M>
Groups makeGroups(int64_t n, const double* pos, const V* vel, const M* mass,
                  const int64_t* gid, int64_t null_id, double box, int64_t min_size,
                  const int64_t* pid = nullptr) {
  std::vector<int64_t> order;
  order.reserve((size_t)n);
  for (int64_t i = 0; i < n; i++)
    if (gid[i] >= 0 && gid[i] != null_id) order.push_back(i);
  std::sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
    return gid[a] != gid[b] ? gid[a] < gid[b] : a < b;
  });
  Groups G;
  G.off.push_back(0);
  for (size_t b = 0; b < order.size();) {
    size_t e = b;
    while (e < order.size() && gid[order[e]] == gid[order[b]]) e++;
    if ((int64_t)(e - b) >= min_size) {
      const double* ref = pos + 3 * order[b];
      double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
      for (size_t k = b; k < e; k++) {
        const int64_t i = order[k];
        G.particle.push_back(i);
        for (int d = 0; d < 3; d++) {
          double x = pos[3 * i + d] - ref[d];
          if (box > 0) x = nearestImage(x, box);
          lo[d] = std::min(lo[d], x);
          hi[d] = std::max(hi[d], x);
          G.pos.push_back(x);
          G.vel.push_back((double)vel[3 * i + d]);
        }
        G.mass.push_back((double)mass[i]);
        G.pid.push_back(pid ? pid[i] : i);
      }
      if (box > 0 && (hi[0] - lo[0] >= 0.5 * box || hi[1] - lo[1] >= 0.5 * box ||
                      hi[2] - lo[2] >= 0.5 * box)) {
        G.wrap = true;
        G.wide_groups++;
      }
      G.id.push_back(gid[order[b]]);
      G.off.push_back((int64_t)G.particle.size());
    }
    b = e;
  }
  return G;
}

// The order of a group's particles for sampling: by this hash of the
// particle id, ties by id (the same function as ubs::sampleKey).
inline uint64_t sampleKey(int64_t id) {
  uint64_t z = (uint64_t)id + 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

namespace detail {

// A sampled group's bookkeeping.
struct Sampled {
  std::vector<int64_t> order;           // its packed particles in sampling order
  std::vector<int64_t> sample;          // the sample of the current bound set
  std::vector<int64_t> added, dropped;  // how the last removal changed the sample
  int64_t next = 0;                     // first position of `order` not yet looked at
  double f = 1;                         // M(bound) / M(sample)
};

struct Frame {
  double x[3], v[3];
};

// Mass-weighted mean position and velocity of the listed particles, the
// position taken relative to the first of them (HBT-HERONS's
// CentreOfMassReferenceFrame), or to the packed particle `origin` if given.
inline Frame meanFrame(const Groups& G, const int64_t* list, int64_t count, bool wrap,
                       double box, int64_t origin = -1) {
  Frame f{{0, 0, 0}, {0, 0, 0}};
  const double* ref = &G.pos[3 * (origin >= 0 ? origin : list[0])];
  double msum = 0;
  for (int64_t k = 0; k < count; k++) {
    const int64_t i = list[k];
    const double m = G.mass[i];
    msum += m;
    for (int d = 0; d < 3; d++) {
      double dx = G.pos[3 * i + d] - ref[d];
      if (wrap) dx = nearestImage(dx, box);
      f.x[d] += dx * m;
      f.v[d] += G.vel[3 * i + d] * m;
    }
  }
  for (int d = 0; d < 3; d++) {
    f.x[d] = f.x[d] / msum + ref[d];
    f.v[d] /= msum;
  }
  return f;
}

// Velocity of a point (x comoving, v physical) relative to a frame,
// Hubble flow included (HBT-HERONS's Snapshot_t::RelativeVelocity).
inline void relativeVelocity(const double* x, const double* v, const Frame& f,
                             double Ha, bool wrap, double box, double* dv) {
  for (int d = 0; d < 3; d++) {
    double dx = x[d] - f.x[d];
    if (wrap) dx = nearestImage(dx, box);
    dv[d] = v[d] - f.v[d] + Ha * dx;
  }
}

}  // namespace detail

// Unbind every group. Real and Acc choose the kernel's pair arithmetic and
// summation types; fast chooses its rsqrt form.
template <class Real, class Acc, bool Fast = false>
Result unbindGroups(const Groups& G, const Params& P) {
  using detail::Frame;
  const int ng = G.ng();
  const int64_t n = G.n();
  if (!(P.eps > 0)) {
    fprintf(stderr, "unbindGroups: softening must be positive\n");
    exit(1);
  }
  const bool wrap = G.wrap && P.box > 0;
  const double kernel_box = wrap ? P.box : 0.0;
  const double Ha = P.H * P.a;
  const double Ga = P.G / P.a;
  const float keep = (float)P.bound_fraction;

  Result R;
  R.nbound.assign(ng, 0);
  R.passes.assign(ng, 0);
  R.stale_frame.assign(ng, 0);
  R.bound.assign((size_t)n, 0);
  R.energy.assign((size_t)n, 0.0);

  // Per group: slots off[g] .. off[g] + nb[g] of perm are the particles still
  // bound, the next nlast[g] - nb[g] those the last pass removed.
  std::vector<int64_t> perm((size_t)n), nb(ng), nlast(ng);
  std::iota(perm.begin(), perm.end(), (int64_t)0);
  enum : uint8_t { Done, Full, Subtract, SampleFull, SampleUpdate };
  std::vector<uint8_t> state(ng, Full), subtracted(ng, 0);
  std::vector<Frame> frame(ng), old_frame(ng);
  for (int g = 0; g < ng; g++) {
    nb[g] = nlast[g] = G.off[g + 1] - G.off[g];
    frame[g] = detail::meanFrame(G, &perm[G.off[g]], nb[g], wrap, P.box,
                                 G.first_origin.empty() ? -1 : G.first_origin[g]);
    old_frame[g] = frame[g];
  }

  // Capped sampling. Per packed particle: alive = still bound; in_sample =
  // 1 for a member of its group's sample, 2 for one that joined at the last
  // removal; phi_s = its sum over the sample, true masses, without f.
  const bool capped = P.cap > 0;
  std::vector<detail::Sampled> samp(capped ? ng : 0);
  std::vector<uint8_t> alive(capped ? (size_t)n : 0, 1), in_sample(capped ? (size_t)n : 0, 0);
  std::vector<double> phi_s(capped ? (size_t)n : 0, 0.0);
  if (capped) {
    for (int g = 0; g < ng; g++) {
      if (nb[g] <= P.cap) continue;
      detail::Sampled& S = samp[g];
      S.order.resize((size_t)nb[g]);
      std::iota(S.order.begin(), S.order.end(), G.off[g]);
      std::sort(S.order.begin(), S.order.end(), [&](int64_t x, int64_t y) {
        const uint64_t kx = sampleKey(G.pid[x]), ky = sampleKey(G.pid[y]);
        return kx != ky ? kx < ky : G.pid[x] < G.pid[y];
      });
      S.sample.assign(S.order.begin(), S.order.begin() + P.cap);
      S.next = P.cap;
      double mb = 0, ms = 0;
      for (int64_t i = G.off[g]; i < G.off[g + 1]; i++) mb += G.mass[i];
      for (int64_t i : S.sample) {
        ms += G.mass[i];
        in_sample[i] = 1;
      }
      S.f = mb / ms;
      state[g] = SampleFull;
      R.stats.sampled_groups++;
    }
  }

  // One launch from host rows (x, y, z, w per particle): targets with w =
  // the mass of their own term among the sources (0 if they are not a
  // source), sources with w = mass. Fills out[] with each target's sum and
  // returns the kernel's seconds.
  auto launch = [&](const std::vector<Real>& tgt, const std::vector<int>& grp,
                    const std::vector<Real>& src, const std::vector<int64_t>& off,
                    std::vector<double>& out) {
    using Kokkos::view_alloc;
    using Kokkos::WithoutInitializing;
    using HostRows = Kokkos::View<const Real* [4], Kokkos::LayoutRight, Kokkos::HostSpace,
                                  Kokkos::MemoryTraits<Kokkos::Unmanaged>>;
    const size_t nt = grp.size(), ns = src.size() / 4;
    ParticleView<Real> d_tgt(view_alloc(WithoutInitializing, "unbind_tgt"), nt);
    ParticleView<Real> d_src(view_alloc(WithoutInitializing, "unbind_src"), ns);
    GroupView d_grp(view_alloc(WithoutInitializing, "unbind_group"), nt);
    OffsetView d_off(view_alloc(WithoutInitializing, "unbind_off"), off.size());
    PotentialView d_phi(view_alloc(WithoutInitializing, "unbind_phi"), nt);
    Kokkos::deep_copy(d_tgt, HostRows(tgt.data(), nt));
    Kokkos::deep_copy(d_src, HostRows(src.data(), ns));
    Kokkos::deep_copy(d_grp, Kokkos::View<const int*, Kokkos::HostSpace,
                                          Kokkos::MemoryTraits<Kokkos::Unmanaged>>(
                                 grp.data(), nt));
    Kokkos::deep_copy(d_off, Kokkos::View<const int64_t*, Kokkos::HostSpace,
                                          Kokkos::MemoryTraits<Kokkos::Unmanaged>>(
                                 off.data(), off.size()));
    const double ks =
        potential<Real, Acc, Fast>(d_tgt, d_grp, d_src, d_off, d_phi, P.eps, kernel_box, true);
    out.resize(nt);
    Kokkos::deep_copy(Kokkos::View<double*, Kokkos::HostSpace,
                                   Kokkos::MemoryTraits<Kokkos::Unmanaged>>(out.data(), nt),
                      d_phi);
    return ks;
  };
  auto row = [&](std::vector<Real>& rows, int64_t i, double w) {
    rows.push_back(Real(G.pos[3 * i]));
    rows.push_back(Real(G.pos[3 * i + 1]));
    rows.push_back(Real(G.pos[3 * i + 2]));
    rows.push_back(Real(w));
  };

  std::vector<int> full, sub, sfull, supd;
  std::vector<int64_t> scratch;
  Kokkos::Timer host_timer;
  double kernel_total = 0;
  for (int pass = 0;; pass++) {
    full.clear();
    sub.clear();
    sfull.clear();
    supd.clear();
    for (int g = 0; g < ng; g++) {
      if (state[g] == Full) full.push_back(g);
      if (state[g] == Subtract) sub.push_back(g);
      if (state[g] == SampleFull) sfull.push_back(g);
      if (state[g] == SampleUpdate) supd.push_back(g);
    }
    if (full.empty() && sub.empty() && sfull.empty() && supd.empty()) break;
    R.stats.passes++;

    // Sampled groups, in full: every bound particle against the sample.
    if (!sfull.empty()) {
      std::vector<Real> tgt, src;
      std::vector<int> grp;
      std::vector<int64_t> off;
      std::vector<double> out;
      int64_t pairs = 0;
      for (size_t b = 0; b < sfull.size(); b++) {
        const int g = sfull[b];
        off.push_back((int64_t)src.size() / 4);
        for (int64_t k = 0; k < nb[g]; k++) {
          const int64_t i = perm[G.off[g] + k];
          row(tgt, i, in_sample[i] ? G.mass[i] : 0.0);
          grp.push_back((int)b);
        }
        for (int64_t i : samp[g].sample) row(src, i, G.mass[i]);
        pairs += nb[g] * (int64_t)samp[g].sample.size();
      }
      off.push_back((int64_t)src.size() / 4);
      const double ks = launch(tgt, grp, src, off, out);
      kernel_total += ks;
      if (pass == 0) {
        R.stats.pairs_first += pairs;
        R.stats.kernel_s_first += ks;
      } else {
        R.stats.pairs_full_later += pairs;
        R.stats.kernel_s_later += ks;
      }
      size_t t = 0;
      for (int g : sfull)
        for (int64_t k = 0; k < nb[g]; k++) phi_s[perm[G.off[g] + k]] = out[t++];
    }

    // Sampled groups, by the sample's change: the members that joined (their
    // own term cancelled for themselves) and, with negated masses, the
    // members that were removed.
    if (!supd.empty()) {
      std::vector<Real> tgt, src;
      std::vector<int> grp;
      std::vector<int64_t> off;
      std::vector<double> out;
      std::vector<int> changed;
      int64_t pairs = 0;
      for (int g : supd) {
        detail::Sampled& S = samp[g];
        if (S.added.empty() && S.dropped.empty()) continue;
        off.push_back((int64_t)src.size() / 4);
        for (int64_t k = 0; k < nb[g]; k++) {
          const int64_t i = perm[G.off[g] + k];
          row(tgt, i, in_sample[i] == 2 ? G.mass[i] : 0.0);
          grp.push_back((int)changed.size());
        }
        for (int64_t i : S.added) {
          row(src, i, G.mass[i]);
          in_sample[i] = 1;
        }
        for (int64_t i : S.dropped) row(src, i, -G.mass[i]);
        pairs += nb[g] * (int64_t)(S.added.size() + S.dropped.size());
        changed.push_back(g);
      }
      if (!changed.empty()) {
        off.push_back((int64_t)src.size() / 4);
        const double ks = launch(tgt, grp, src, off, out);
        kernel_total += ks;
        R.stats.pairs_sample_update += pairs;
        R.stats.kernel_s_later += ks;
        size_t t = 0;
        for (int g : changed)
          for (int64_t k = 0; k < nb[g]; k++) phi_s[perm[G.off[g] + k]] += out[t++];
      }
    }

    // Energies of the sampled groups from their sums, in the current frame.
    for (const std::vector<int>* list : {&sfull, &supd}) {
      for (int g : *list) {
        const double scale = Ga * samp[g].f;
        for (int64_t k = 0; k < nb[g]; k++) {
          const int64_t i = perm[G.off[g] + k];
          double dv[3];
          detail::relativeVelocity(&G.pos[3 * i], &G.vel[3 * i], frame[g], Ha, wrap, P.box,
                                   dv);
          R.energy[i] =
              0.5 * (dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]) + scale * phi_s[i];
        }
      }
    }

    // Full passes: the bound particles are both targets and sources.
    if (!full.empty()) {
      int64_t nt = 0;
      for (int g : full) nt += nb[g];
      ParticleView<Real> part(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_part"), nt);
      GroupView group(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_group"), nt);
      OffsetView off(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_off"), full.size() + 1);
      PotentialView phi(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_phi"), nt);
      auto h_part = Kokkos::create_mirror_view(part);
      auto h_group = Kokkos::create_mirror_view(group);
      auto h_off = Kokkos::create_mirror_view(off);
      int64_t t = 0, pairs = 0;
      for (size_t b = 0; b < full.size(); b++) {
        const int g = full[b];
        h_off(b) = t;
        for (int64_t k = 0; k < nb[g]; k++, t++) {
          const int64_t i = perm[G.off[g] + k];
          h_part(t, 0) = Real(G.pos[3 * i]);
          h_part(t, 1) = Real(G.pos[3 * i + 1]);
          h_part(t, 2) = Real(G.pos[3 * i + 2]);
          h_part(t, 3) = Real(G.mass[i]);
          h_group(t) = (int)b;
        }
        pairs += nb[g] * nb[g];
      }
      h_off(full.size()) = t;
      Kokkos::deep_copy(part, h_part);
      Kokkos::deep_copy(group, h_group);
      Kokkos::deep_copy(off, h_off);
      const double ks = potential<Real, Acc, Fast>(part, group, part, off, phi, P.eps,
                                                  kernel_box, true);
      kernel_total += ks;
      if (pass == 0) {
        R.stats.pairs_first += pairs;
        R.stats.kernel_s_first += ks;
      } else {
        R.stats.pairs_full_later += pairs;
        R.stats.kernel_s_later += ks;
      }
      auto h_phi = Kokkos::create_mirror_view(phi);
      Kokkos::deep_copy(h_phi, phi);
      t = 0;
      for (int g : full) {
        for (int64_t k = 0; k < nb[g]; k++, t++) {
          const int64_t i = perm[G.off[g] + k];
          double dv[3];
          detail::relativeVelocity(&G.pos[3 * i], &G.vel[3 * i], frame[g], Ha, wrap,
                                   P.box, dv);
          R.energy[i] = 0.5 * (dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]) +
                        Ga * h_phi(t);
        }
      }
    }

    // Subtractions: targets are the bound particles, sources the particles
    // the last pass removed.
    if (!sub.empty()) {
      int64_t nt = 0, ns = 0;
      for (int g : sub) {
        nt += nb[g];
        ns += nlast[g] - nb[g];
      }
      ParticleView<Real> tgt(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_tgt"), nt);
      ParticleView<Real> src(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_src"), ns);
      GroupView group(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_group"), nt);
      OffsetView off(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_off"), sub.size() + 1);
      PotentialView phi(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_phi"), nt);
      auto h_tgt = Kokkos::create_mirror_view(tgt);
      auto h_src = Kokkos::create_mirror_view(src);
      auto h_group = Kokkos::create_mirror_view(group);
      auto h_off = Kokkos::create_mirror_view(off);
      int64_t t = 0, s = 0, pairs = 0;
      for (size_t b = 0; b < sub.size(); b++) {
        const int g = sub[b];
        h_off(b) = s;
        for (int64_t k = 0; k < nlast[g]; k++) {
          const int64_t i = perm[G.off[g] + k];
          auto& view = k < nb[g] ? h_tgt : h_src;
          int64_t& row = k < nb[g] ? t : s;
          view(row, 0) = Real(G.pos[3 * i]);
          view(row, 1) = Real(G.pos[3 * i + 1]);
          view(row, 2) = Real(G.pos[3 * i + 2]);
          view(row, 3) = Real(G.mass[i]);
          if (k < nb[g]) h_group(row) = (int)b;
          row++;
        }
        pairs += nb[g] * (nlast[g] - nb[g]);
      }
      h_off(sub.size()) = s;
      Kokkos::deep_copy(tgt, h_tgt);
      Kokkos::deep_copy(src, h_src);
      Kokkos::deep_copy(group, h_group);
      Kokkos::deep_copy(off, h_off);
      const double ks = potential<Real, Acc, Fast>(tgt, group, src, off, phi, P.eps,
                                                  kernel_box, false);
      kernel_total += ks;
      R.stats.pairs_subtract += pairs;
      R.stats.kernel_s_later += ks;
      auto h_phi = Kokkos::create_mirror_view(phi);
      Kokkos::deep_copy(h_phi, phi);
      t = 0;
      for (int g : sub) {
        // The new frame as seen from the old one.
        double shift[3];
        detail::relativeVelocity(old_frame[g].x, old_frame[g].v, frame[g], Ha, wrap,
                                 P.box, shift);
        const double dK =
            0.5 * (shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]);
        for (int64_t k = 0; k < nb[g]; k++, t++) {
          const int64_t i = perm[G.off[g] + k];
          double dv[3];
          detail::relativeVelocity(&G.pos[3 * i], &G.vel[3 * i], old_frame[g], Ha, wrap,
                                   P.box, dv);
          R.energy[i] += dv[0] * shift[0] + dv[1] * shift[1] + dv[2] * shift[2] + dK -
                         Ga * h_phi(t);
        }
      }
    }

    // Remove the unbound particles and decide what each group does next.
    for (int g = 0; g < ng; g++) {
      if (state[g] == Done) continue;
      R.passes[g]++;
      int64_t* p = &perm[G.off[g]];
      nlast[g] = nb[g];
      scratch.clear();
      int64_t kept = 0;
      for (int64_t k = 0; k < nlast[g]; k++) {   // stable: bound first
        if (R.energy[p[k]] < 0)
          p[kept++] = p[k];
        else
          scratch.push_back(p[k]);
      }
      std::copy(scratch.begin(), scratch.end(), p + kept);
      nb[g] = kept;
      if (capped)
        for (int64_t i : scratch) alive[i] = 0;
      if (nb[g] < P.min_bound || nb[g] == 0) {
        nb[g] = 0;
        state[g] = Done;
        continue;
      }
      const int64_t removed = nlast[g] - nb[g];
      const bool subtract_next = removed < nb[g];
      if (subtract_next) old_frame[g] = frame[g];
      frame[g] = detail::meanFrame(G, p, nb[g], wrap, P.box);
      if ((float)nb[g] >= (float)nlast[g] * keep) {
        state[g] = Done;
        continue;
      }
      if (state[g] == SampleFull || state[g] == SampleUpdate) {
        if (nb[g] <= P.cap) {
          state[g] = Full;   // at or below the cap: the exact rules from here on
          continue;
        }
        // The sample of the new bound set: without the removed members, with
        // the next bound particles in the order.
        detail::Sampled& S = samp[g];
        S.added.clear();
        S.dropped.clear();
        size_t w = 0;
        for (int64_t i : S.sample) {
          if (alive[i]) {
            S.sample[w++] = i;
          } else {
            S.dropped.push_back(i);
            in_sample[i] = 0;
          }
        }
        S.sample.resize(w);
        while ((int64_t)S.sample.size() < P.cap) {
          const int64_t i = S.order[S.next++];
          if (!alive[i]) continue;
          S.sample.push_back(i);
          S.added.push_back(i);
          in_sample[i] = 2;
        }
        double mb = 0, ms = 0;
        for (int64_t k = 0; k < nb[g]; k++) mb += G.mass[p[k]];
        for (int64_t i : S.sample) ms += G.mass[i];
        S.f = mb / ms;
        if (2 * (int64_t)S.dropped.size() < P.cap) {
          state[g] = SampleUpdate;
        } else {
          state[g] = SampleFull;
          for (int64_t i : S.added) in_sample[i] = 1;
        }
        continue;
      }
      if (!subtract_next && subtracted[g]) R.stale_frame[g] = 1;
      if (subtract_next) subtracted[g] = 1;
      state[g] = subtract_next ? Subtract : Full;
    }
  }

  for (int g = 0; g < ng; g++) {
    R.nbound[g] = nb[g];
    for (int64_t k = 0; k < nb[g]; k++) R.bound[perm[G.off[g] + k]] = 1;
  }
  R.stats.host_s = host_timer.seconds() - kernel_total;
  return R;
}

}  // namespace unbind

#endif  // SUBHALO_UNBIND_GROUPS_H_
