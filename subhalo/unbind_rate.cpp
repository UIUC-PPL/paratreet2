// unbind_rate -- self-checks and pair-term rate of the unbinding kernel on
// synthetic particles (design/subhalo-checks.md, step 1).
//
//   unbind_rate [--seconds S] [--max-targets N] [--no-checks] [--no-rate]
//
// 1. Kernel check. The potential from unbind_kernel.h against a plain
//    double-precision direct sum on the host: all pairs, targets apart from
//    sources, and nearest-image separations in a small periodic box. Both
//    forms: potential() and potentialSplit() at chunks of 1, 7, 64 and 5,000
//    sources.
// 2. Unbinding check. unbindGroups (unbind_groups.h) against a separate
//    serial implementation of the same rules that works on absolute
//    positions, skips the self pair explicitly and computes every pass in
//    full. In double precision the two must select the same particles.
// 3. Rates. Pair terms per second of one kernel launch, for the cases the
//    cap table of the design note needs: T targets against S sources of one
//    group, and many groups of n particles each done all-pairs. Each case is
//    sized from short calibration launches to run about --seconds, then
//    timed three times; the best time is reported. After timing, eight
//    targets of the launch are recomputed on the host and must agree.
//
// Every particle is a point of a Hernquist sphere of scale radius 1 cut at
// radius 20, softening 0.02. Synthetic data cannot say whether the rules
// reproduce HBT-HERONS's bound sets; subhalo/unbind.cpp does that on a real
// snapshot.
//
// Plain Kokkos program: no Charm header, no HDF5.

#include "unbind_groups.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using unbind::Exec;

KOKKOS_INLINE_FUNCTION uint64_t mix(uint64_t z) {
  z += 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}
// Uniform in [0, 1), a pure function of the key.
KOKKOS_INLINE_FUNCTION double uniform(uint64_t key) {
  return double(mix(key) >> 11) * (1.0 / 9007199254740992.0);
}
// A point of a Hernquist sphere of scale radius 1, cut at radius 20.
KOKKOS_INLINE_FUNCTION void hernquist(uint64_t key, double p[3]) {
  const double umax = (20.0 / 21.0) * (20.0 / 21.0);
  const double q = Kokkos::sqrt(umax * uniform(3 * key));
  const double r = q / (1.0 - q);
  const double cz = 2.0 * uniform(3 * key + 1) - 1.0;
  const double sz = Kokkos::sqrt(1.0 - cz * cz);
  const double ph = 6.283185307179586 * uniform(3 * key + 2);
  p[0] = r * sz * Kokkos::cos(ph);
  p[1] = r * sz * Kokkos::sin(ph);
  p[2] = r * cz;
}

const double kEps = 0.02;

// ---- host reference --------------------------------------------------------

// One pair term in double, HBT-HERONS's formula written out again.
double pairRef(double m, double r, double eps) {
  const double h = 2.8 * eps;
  if (r >= h) return -m / r;
  const double u = r / h;
  double wp;
  if (u < 0.5)
    wp = -2.8 + u * u * (5.333333333333 + u * u * (6.4 * u - 9.6));
  else
    wp = -3.2 + 0.066666666667 / u +
         u * u * (10.666666666667 + u * (-16.0 + u * (9.6 - 2.133333333333 * u)));
  return m / h * wp;
}

struct Abs {  // particles at absolute positions in a periodic (or open) box
  std::vector<double> pos, vel, mass;
  std::vector<int64_t> gid;
  int64_t n() const { return (int64_t)mass.size(); }
};

// Unbind one group by the rules of unbind_groups.h, serially: absolute
// positions, nearest image per pair, the self pair skipped, every pass in
// full. Returns the bound count (0 = no sub-halo) and marks bound[].
int64_t unbindRef(const Abs& A, const std::vector<int64_t>& members,
                  const unbind::Params& P, std::vector<uint8_t>& bound, int& passes) {
  auto near = [&](double d) { return P.box > 0 ? unbind::nearestImage(d, P.box) : d; };
  std::vector<int64_t> cur = members, next;
  std::vector<double> E;
  passes = 0;
  double fx[3], fv[3];
  auto frame = [&]() {
    double msum = 0, sx[3] = {0, 0, 0}, sv[3] = {0, 0, 0};
    const double* ref = &A.pos[3 * cur[0]];
    for (int64_t i : cur) {
      msum += A.mass[i];
      for (int d = 0; d < 3; d++) {
        sx[d] += near(A.pos[3 * i + d] - ref[d]) * A.mass[i];
        sv[d] += A.vel[3 * i + d] * A.mass[i];
      }
    }
    for (int d = 0; d < 3; d++) {
      fx[d] = sx[d] / msum + ref[d];
      fv[d] = sv[d] / msum;
    }
  };
  frame();
  while (true) {
    passes++;
    E.assign(cur.size(), 0.0);
    for (size_t a = 0; a < cur.size(); a++) {
      const int64_t i = cur[a];
      double phi = 0;
      for (size_t b = 0; b < cur.size(); b++) {
        if (b == a) continue;
        const int64_t j = cur[b];
        const double dx = near(A.pos[3 * j] - A.pos[3 * i]);
        const double dy = near(A.pos[3 * j + 1] - A.pos[3 * i + 1]);
        const double dz = near(A.pos[3 * j + 2] - A.pos[3 * i + 2]);
        phi += pairRef(A.mass[j], std::sqrt(dx * dx + dy * dy + dz * dz), P.eps);
      }
      double k = 0;
      for (int d = 0; d < 3; d++) {
        const double dv =
            A.vel[3 * i + d] - fv[d] + P.H * P.a * near(A.pos[3 * i + d] - fx[d]);
        k += dv * dv;
      }
      E[a] = 0.5 * k + P.G / P.a * phi;
    }
    next.clear();
    for (size_t a = 0; a < cur.size(); a++)
      if (E[a] < 0) next.push_back(cur[a]);
    const int64_t nlast = (int64_t)cur.size(), nb = (int64_t)next.size();
    cur.swap(next);
    if (nb < P.min_bound || nb == 0) return 0;
    frame();
    if ((float)nb >= (float)nlast * (float)P.bound_fraction) break;
  }
  for (int64_t i : cur) bound[i] = 1;
  return (int64_t)cur.size();
}

// ---- check 1: the kernel ---------------------------------------------------

template <class Real, class Acc, bool Fast>
double kernelError(const std::vector<double>& tgt, const std::vector<int>& tgt_group,
                   const std::vector<double>& src, const std::vector<int64_t>& src_off,
                   const std::vector<double>& ref, double eps, double box, bool self,
                   int64_t chunk = 0) {
  const int64_t nt = (int64_t)tgt_group.size(), ns = (int64_t)src.size() / 4;
  unbind::ParticleView<Real> d_tgt("tgt", nt), d_src("src", ns);
  unbind::GroupView d_group("group", nt);
  unbind::OffsetView d_off("off", src_off.size());
  unbind::PotentialView d_phi("phi", nt);
  auto h_tgt = Kokkos::create_mirror_view(d_tgt);
  auto h_src = Kokkos::create_mirror_view(d_src);
  auto h_group = Kokkos::create_mirror_view(d_group);
  auto h_off = Kokkos::create_mirror_view(d_off);
  for (int64_t t = 0; t < nt; t++) {
    for (int c = 0; c < 4; c++) h_tgt(t, c) = Real(tgt[4 * t + c]);
    h_group(t) = tgt_group[t];
  }
  for (int64_t s = 0; s < ns; s++)
    for (int c = 0; c < 4; c++) h_src(s, c) = Real(src[4 * s + c]);
  for (size_t g = 0; g < src_off.size(); g++) h_off(g) = src_off[g];
  Kokkos::deep_copy(d_tgt, h_tgt);
  Kokkos::deep_copy(d_src, h_src);
  Kokkos::deep_copy(d_group, h_group);
  Kokkos::deep_copy(d_off, h_off);
  // self: the kernel is given one view as both targets and sources.
  // chunk > 0: the split form, each target's sum in chunks of that many.
  if (chunk > 0)
    unbind::potentialSplit<Real, Acc, Fast>(
        d_tgt, unbind::splitPlan(tgt_group.data(), nt, src_off.data(), chunk),
        self ? d_tgt : d_src, d_off, d_phi, eps, box, self);
  else
    unbind::potential<Real, Acc, Fast>(d_tgt, d_group, self ? d_tgt : d_src, d_off, d_phi,
                                       eps, box, self);
  auto h_phi = Kokkos::create_mirror_view(d_phi);
  Kokkos::deep_copy(h_phi, d_phi);
  double worst = 0;
  for (int64_t t = 0; t < nt; t++)
    worst = std::max(worst, std::fabs(h_phi(t) - ref[t]) / std::fabs(ref[t]));
  return worst;
}

bool checkKernel() {
  printf("\n== check 1: kernel against a double-precision direct sum ==\n");
  bool ok = true;
  // Tolerances: double is limited by the order of summation; single by the
  // rounding of the offsets (up to 40 from the reference point) against
  // pair separations down to the softening.
  const double tol_f64 = 1e-12, tol_f32 = 1e-4;
  // Softening 0.02 is the rate runs' value; with 2 a good share of the
  // pairs falls in each of the kernel's three branches (printed, and
  // required to be at least 1% each).
  struct Case {
    const char* name;
    double eps;
    double box;   // 0 = open
    bool self;
  };
  const Case cases[5] = {{"all pairs", 0.02, 0, true},
                         {"all pairs", 2.0, 0, true},
                         {"targets apart from sources", 2.0, 0, false},
                         {"all pairs, periodic box of side 30", 0.02, 30, true},
                         {"all pairs, periodic box of side 30", 2.0, 30, true}};
  const int sizes[5] = {20, 37, 100, 1000, 3000};
  for (const Case& c : cases) {
    std::vector<double> tgt, src, ref;
    std::vector<int> tgt_group;
    std::vector<int64_t> src_off{0};
    double branch[3] = {0, 0, 0};   // pairs with r < h/2, h/2 <= r < h, r >= h
    for (int g = 0; g < 5; g++) {
      // The group's particles as offsets from its first particle.
      const int n = sizes[g];
      std::vector<double> p(4 * (size_t)n);
      double first[3];
      hernquist(1000003ull * (g + 1), first);
      for (int i = 0; i < n; i++) {
        double q[3];
        hernquist(1000003ull * (g + 1) + i, q);
        for (int d = 0; d < 3; d++) {
          double x = (0.5 + g) * (q[d] - first[d]);
          if (c.box > 0) x = unbind::nearestImage(std::fmod(x, c.box), c.box);
          p[4 * i + d] = x;
        }
        p[4 * i + 3] = 0.5 + uniform(77777ull * (g + 1) + i);
      }
      // All pairs: every particle is target and source. Apart: the first
      // half are targets, the second half sources.
      const int nt = c.self ? n : n / 2, s0 = c.self ? 0 : n / 2;
      for (int t = 0; t < nt; t++) {
        double phi = 0;
        for (int s = s0; s < n; s++) {
          if (s == t) continue;
          double d[3];
          for (int k = 0; k < 3; k++) {
            d[k] = p[4 * s + k] - p[4 * t + k];
            if (c.box > 0) d[k] = unbind::nearestImage(d[k], c.box);
          }
          const double r = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
          branch[r < 1.4 * c.eps ? 0 : (r < 2.8 * c.eps ? 1 : 2)]++;
          phi += pairRef(p[4 * s + 3], r, c.eps);
        }
        ref.push_back(phi);
        tgt.insert(tgt.end(), p.begin() + 4 * t, p.begin() + 4 * t + 4);
        tgt_group.push_back(g);
      }
      src.insert(src.end(), p.begin() + 4 * s0, p.end());
      src_off.push_back((int64_t)src.size() / 4);
    }
    const double all = branch[0] + branch[1] + branch[2];
    const bool covered = c.eps < 0.1 || (branch[0] >= 0.01 * all && branch[1] >= 0.01 * all);
    printf("%-36s softening %.2f (pairs inside h/2: %.2f%%, h/2 to h: %.2f%%)%s\n", c.name,
           c.eps, 100 * branch[0] / all, 100 * branch[1] / all,
           covered ? "" : "  FAIL: too few softened pairs");
    ok = ok && covered;
    // chunk 0: one thread per target (potential). Otherwise the split form
    // (potentialSplit): 1 and 7 give many chunks and a short last one, 64
    // some, 5,000 one chunk per target (the largest group has 3,000).
    for (const int64_t chunk : {0, 1, 7, 64, 5000}) {
      auto err = [&](auto real, auto acc, auto fast) {
        using R = decltype(real);
        using A = decltype(acc);
        return kernelError<R, A, decltype(fast)::value>(tgt, tgt_group, src, src_off, ref,
                                                        c.eps, c.box, c.self, chunk);
      };
      using F = std::false_type;
      using T = std::true_type;
      const double e[6] = {err(0.0, 0.0, F()), err(0.0, 0.0, T()),  err(0.0f, 0.0, F()),
                           err(0.0f, 0.0, T()), err(0.0f, 0.0f, F()), err(0.0f, 0.0f, T())};
      const bool pass = e[0] < tol_f64 && e[1] < tol_f64 && e[2] < tol_f32 && e[3] < tol_f32 &&
                        e[4] < tol_f32 && e[5] < tol_f32;
      ok = ok && pass;
      char form[32];
      if (chunk == 0) snprintf(form, sizeof form, "one thread per target");
      else snprintf(form, sizeof form, "split, chunks of %lld", (long long)chunk);
      printf("    %-22s largest relative error: f64 %.1e (rsqrt %.1e)  f32/f64 %.1e "
             "(rsqrt %.1e)  f32/f32 %.1e (rsqrt %.1e)  %s\n",
             form, e[0], e[1], e[2], e[3], e[4], e[5], pass ? "ok" : "FAIL");
    }
  }
  printf("CHECK kernel %s (tolerance: f64 %.0e, f32 %.0e)\n", ok ? "PASS" : "FAIL",
         tol_f64, tol_f32);
  return ok;
}

// ---- check 2: the unbinding loop ------------------------------------------

// Groups that exercise every rule: velocity dispersions from well bound to
// unbound, and in half of the groups a second clump flying through, so the
// frame moves between passes.
Abs syntheticGroups(int ngroups, double box, unbind::Params& P) {
  P.G = 1;
  P.a = 0.5;
  P.H = 0.1;
  P.eps = 0.1;   // softened pairs are common in the cores
  P.box = box;
  Abs A;
  uint64_t key = 424242;
  for (int g = 0; g < ngroups; g++) {
    const uint64_t gk = 9176ull * (g + 1);
    int n = (int)std::exp(std::log(20.0) + uniform(gk) * std::log(1500.0 / 20.0));
    if (g < 3) n = 6000;
    const double scale = 0.5 + 1.5 * uniform(gk + 1);
    const double gmass = 0.5 + 1.5 * uniform(gk + 2);
    const double hot = 0.4 * std::exp(uniform(gk + 3) * std::log(2.5 / 0.4));
    // 1D dispersion of a Hernquist sphere in virial equilibrium, times hot.
    const double sigma = hot * std::sqrt(P.G * gmass / (18.0 * P.a * scale));
    const double clump = uniform(gk + 4) < 0.5 ? 0.45 * uniform(gk + 5) : 0.0;
    double centre[3], cdir[3];
    for (int d = 0; d < 3; d++) {
      centre[d] = box * uniform(gk + 6 + d);
      cdir[d] = 2 * uniform(gk + 9 + d) - 1;
    }
    for (int i = 0; i < n; i++, key++) {
      double q[3];
      hernquist(key, q);
      const bool in_clump = uniform(5 * key + 1) < clump;
      for (int d = 0; d < 3; d++) {
        // Box-Muller, one normal deviate per component.
        const double u1 = 1.0 - uniform(11 * key + 2 * d), u2 = uniform(11 * key + 2 * d + 1);
        const double gauss = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
        double x = centre[d] + scale * q[d] + (in_clump ? 4.0 * scale * cdir[d] : 0.0);
        x -= box * std::floor(x / box);   // into [0, box)
        A.pos.push_back(x);
        A.vel.push_back(sigma * gauss + (in_clump ? 3.0 * sigma * cdir[d] : 0.0));
      }
      A.mass.push_back(gmass / n * (0.5 + uniform(7 * key + 3)));
      A.gid.push_back(g + 1);
    }
  }
  return A;
}

bool checkUnbinding() {
  printf("\n== check 2: unbinding loop against a serial reference ==\n");
  unbind::Params P;
  const int ngroups = 1000;
  const Abs A = syntheticGroups(ngroups, 2000.0, P);

  // Reference.
  std::vector<std::vector<int64_t>> members(ngroups);
  for (int64_t i = 0; i < A.n(); i++) members[A.gid[i] - 1].push_back(i);
  std::vector<uint8_t> ref_bound((size_t)A.n(), 0);
  std::vector<int64_t> ref_nb(ngroups);
  std::vector<int> ref_passes(ngroups);
  Kokkos::Timer timer;
  for (int g = 0; g < ngroups; g++)
    ref_nb[g] = unbindRef(A, members[g], P, ref_bound, ref_passes[g]);
  const double ref_s = timer.seconds();
  int64_t ref_total = 0, alive = 0, multi = 0;
  for (int g = 0; g < ngroups; g++) {
    ref_total += ref_nb[g];
    alive += ref_nb[g] > 0;
    multi += ref_passes[g] > 2;
  }
  printf("%d groups, %lld particles; reference: %lld groups keep a sub-halo, "
         "%lld bound particles (%.1f%%), %lld groups took more than 2 passes, %.1f s\n",
         ngroups, (long long)A.n(), (long long)alive, (long long)ref_total,
         100.0 * ref_total / (double)A.n(), (long long)multi, ref_s);

  unbind::Groups G = unbind::makeGroups(A.n(), A.pos.data(), A.vel.data(), A.mass.data(),
                                        A.gid.data(), (int64_t)-1, P.box, 1);
  bool ok = G.ng() == ngroups && !G.wrap;
  if (!ok) printf("FAIL: makeGroups gave %d groups, wrap %d\n", G.ng(), (int)G.wrap);

  auto compare = [&](const char* name, const unbind::Result& R, bool must_match) {
    int64_t groups_differ = 0, particles_differ = 0, passes_differ = 0;
    for (int g = 0; g < ngroups; g++) {
      groups_differ += R.nbound[g] != ref_nb[g];
      passes_differ += R.passes[g] != ref_passes[g];
    }
    for (int64_t k = 0; k < G.n(); k++)
      particles_differ += R.bound[k] != ref_bound[G.particle[k]];
    const unbind::Stats& s = R.stats;
    int64_t stale = 0;
    for (int g = 0; g < ngroups; g++) stale += R.stale_frame[g];
    const bool pass = !must_match || (groups_differ == 0 && particles_differ == 0);
    printf("%-22s groups with a different bound count: %lld, particles differing: %lld, "
           "groups with a different pass count: %lld%s\n",
           name, (long long)groups_differ, (long long)particles_differ,
           (long long)passes_differ, must_match ? (pass ? "  ok" : "  FAIL") : "");
    printf("%-22s pair terms: first pass %.3g, later in full %.3g, later by subtraction "
           "%.3g (later / first = %.2f); %d rounds; %lld groups where HBT-HERONS would "
           "use a stale frame\n",
           "", (double)s.pairs_first, (double)s.pairs_full_later, (double)s.pairs_subtract,
           (double)(s.pairs_full_later + s.pairs_subtract) / (double)s.pairs_first,
           s.passes, (long long)stale);
    return pass;
  };
  ok = compare("f64, sums in f64", unbind::unbindGroups<double, double>(G, P), true) && ok;
  // The same with nearest-image separations forced on: no group is wide, so
  // the answer must not change.
  G.wrap = true;
  ok = compare("f64, nearest image", unbind::unbindGroups<double, double>(G, P), true) && ok;
  G.wrap = false;
  compare("f32, sums in f64", unbind::unbindGroups<float, double>(G, P), false);
  compare("f32, sums in f32", unbind::unbindGroups<float, float>(G, P), false);
  compare("f32 rsqrt, sums in f64", unbind::unbindGroups<float, double, true>(G, P), false);
  printf("CHECK unbinding %s (the two f64 runs must select the reference's particles)\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

// ---- rates -----------------------------------------------------------------

template <class Real>
void fill(const unbind::ParticleView<Real>& v, uint64_t key0) {
  Kokkos::parallel_for(
      "unbind_rate::fill",
      Kokkos::RangePolicy<Exec, Kokkos::IndexType<int64_t>>(0, (int64_t)v.extent(0)),
      KOKKOS_LAMBDA(const int64_t i) {
        double p[3];
        hernquist(key0 + (uint64_t)i, p);
        v(i, 0) = Real(p[0]);
        v(i, 1) = Real(p[1]);
        v(i, 2) = Real(p[2]);
        v(i, 3) = Real(1);
      });
}

struct RateRow {
  std::string layout, prec;
  long long targets, sources, groups;
  double pairs, best_s, rate, spot_error;
};
std::vector<RateRow> g_rows;
bool g_spot_ok = true;

// One case. group_n == 0: `sources` sources of one group against as many
// targets as the time allows. group_n > 0: groups of group_n particles, each
// done all-pairs, as many groups as the time allows.
template <class Real, class Acc, bool Fast>
void rate(const char* prec, int64_t sources, int64_t group_n, double box, double seconds,
          int64_t max_targets) {
  const bool batched = group_n > 0;
  const double pairs_per_unit = batched ? (double)group_n * group_n : (double)sources;
  const int64_t unit_targets = batched ? group_n : 1;
  const int64_t max_units = std::max<int64_t>(1, max_targets / unit_targets);
  double spot_error = 0;
  auto run = [&](int64_t units, int reps, double& best, bool spot_check) {
    const int64_t nt = units * unit_targets;
    unbind::ParticleView<Real> tgt(Kokkos::view_alloc(Kokkos::WithoutInitializing, "tgt"), nt);
    unbind::ParticleView<Real> src;
    unbind::GroupView group(Kokkos::view_alloc(Kokkos::WithoutInitializing, "group"), nt);
    unbind::OffsetView off(Kokkos::view_alloc(Kokkos::WithoutInitializing, "off"),
                           (batched ? units : 1) + 1);
    unbind::PotentialView phi(Kokkos::view_alloc(Kokkos::WithoutInitializing, "phi"), nt);
    fill(tgt, 1);
    if (batched) {
      src = tgt;
      const int64_t n = group_n;
      Kokkos::parallel_for(
          "unbind_rate::groups",
          Kokkos::RangePolicy<Exec, Kokkos::IndexType<int64_t>>(0, nt),
          KOKKOS_LAMBDA(const int64_t t) { group(t) = (int)(t / n); });
      Kokkos::parallel_for(
          "unbind_rate::offsets",
          Kokkos::RangePolicy<Exec, Kokkos::IndexType<int64_t>>(0, units + 1),
          KOKKOS_LAMBDA(const int64_t g) { off(g) = g * n; });
    } else {
      src = unbind::ParticleView<Real>(
          Kokkos::view_alloc(Kokkos::WithoutInitializing, "src"), sources);
      fill(src, 1ull << 40);
      Kokkos::deep_copy(group, 0);
      const int64_t ns = sources;
      Kokkos::parallel_for(
          "unbind_rate::offsets",
          Kokkos::RangePolicy<Exec, Kokkos::IndexType<int64_t>>(0, 2),
          KOKKOS_LAMBDA(const int64_t g) { off(g) = g * ns; });
    }
    best = 1e300;
    for (int r = 0; r < reps; r++)
      best = std::min(best, unbind::potential<Real, Acc, Fast>(tgt, group, src, off, phi,
                                                               kEps, box, batched));
    // Spot check: eight targets spread over the launch against a double
    // direct sum on the host over the device's own particle values, so that
    // a rate cannot come from work that was not done.
    if (spot_check) {
      const int64_t ns = batched ? group_n : sources;
      for (int k = 0; k < 8; k++) {
        const int64_t t = (int64_t)((double)k / 7.0 * (double)(nt - 1));
        const int64_t s0 = batched ? (t / group_n) * group_n : 0;
        auto h_src = Kokkos::create_mirror_view_and_copy(
            Kokkos::HostSpace(),
            Kokkos::subview(src, Kokkos::make_pair(s0, s0 + ns), Kokkos::ALL));
        auto h_tgt = Kokkos::create_mirror_view_and_copy(
            Kokkos::HostSpace(),
            Kokkos::subview(tgt, Kokkos::make_pair(t, t + 1), Kokkos::ALL));
        auto h_phi = Kokkos::create_mirror_view_and_copy(
            Kokkos::HostSpace(), Kokkos::subview(phi, Kokkos::make_pair(t, t + 1)));
        double ref = 0;
        for (int64_t j = 0; j < ns; j++) {
          if (batched && s0 + j == t) continue;
          double d[3];
          for (int c = 0; c < 3; c++) {
            d[c] = (double)h_src(j, c) - (double)h_tgt(0, c);
            if (box > 0) d[c] = unbind::nearestImage(d[c], box);
          }
          ref += pairRef((double)h_src(j, 3), std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]),
                         kEps);
        }
        spot_error = std::max(spot_error, std::fabs(h_phi(0) - ref) / std::fabs(ref));
      }
    }
    return (double)units * pairs_per_unit;
  };
  // Size the run from short launches, starting at about 2e9 pair terms (the
  // first launch also pays the one-off costs), until one lasts half the
  // wanted time; then time it three times.
  double best = 0, pairs = 0;
  int64_t units = std::min(max_units, std::max<int64_t>(1, (int64_t)(2e9 / pairs_per_unit)));
  for (int round = 0; round < 4; round++) {
    pairs = run(units, round == 0 ? 2 : 1, best, false);
    if (best >= 0.5 * seconds || units >= max_units) break;
    const int64_t want = (int64_t)(pairs / best * seconds / pairs_per_unit);
    units = std::min(max_units, std::max(units + 1, want));
  }
  pairs = run(units, 3, best, true);
  RateRow row{batched ? "groups, all pairs" : "targets x sources",
              std::string(prec) + (Fast ? " rsqrt" : "") + (box > 0 ? " nearest-image" : ""),
              (long long)(units * unit_targets),
              (long long)(batched ? group_n : sources),
              (long long)(batched ? units : 1),
              pairs, best, pairs / best, spot_error};
  // Summed in double the kernel must agree with the host to 1e-4 (single
  // precision pair terms) or 1e-12 (double); sums in single are reported.
  const double tol = sizeof(Acc) == 4 ? 1.0 : (sizeof(Real) == 4 ? 1e-4 : 1e-12);
  if (!(spot_error < tol)) g_spot_ok = false;
  g_rows.push_back(row);
  printf("RATE layout=\"%s\" kernel=\"%s\" targets=%lld sources_per_group=%lld groups=%lld "
         "pair_terms=%.4g best_s=%.4f rate=%.4g spot_error=%.1e\n",
         row.layout.c_str(), row.prec.c_str(), row.targets, row.sources, row.groups,
         row.pairs, row.best_s, row.rate, row.spot_error);
  fflush(stdout);
}

void rates(double seconds, int64_t max_targets) {
  printf("\n== rates: pair terms per second, one kernel launch ==\n");
  const double s = seconds;
  const int64_t m = max_targets;
  // T targets against S sources of one group.
  for (int64_t S : {1000LL, 10000LL, 100000LL, 1000000LL}) {
    rate<float, double, false>("f32, sums in f64", S, 0, 0, s, m);
    rate<double, double, false>("f64", S, 0, 0, s, m);
  }
  // The other forms of the kernel, at 10,000 sources.
  rate<float, float, false>("f32, sums in f32", 10000, 0, 0, s, m);
  rate<float, double, true>("f32, sums in f64", 10000, 0, 0, s, m);
  rate<float, float, true>("f32, sums in f32", 10000, 0, 0, s, m);
  rate<double, double, true>("f64", 10000, 0, 0, s, m);
  rate<float, double, false>("f32, sums in f64", 10000, 0, 100.0, s, m);
  // Many groups in one launch, each all-pairs.
  for (int64_t n : {32LL, 128LL, 1024LL, 8192LL, 65536LL, 262144LL})
    rate<float, double, false>("f32, sums in f64", 0, n, 0, s, m);
  rate<double, double, false>("f64", 0, 1024, 0, s, m);

  printf("\n%-18s %-34s %12s %12s %10s %11s %9s %10s %11s\n", "layout", "kernel", "targets",
         "sources/grp", "groups", "pair terms", "best (s)", "rate (/s)", "spot error");
  for (const RateRow& r : g_rows)
    printf("%-18s %-34s %12lld %12lld %10lld %11.3g %9.3f %10.3g %11.1e\n", r.layout.c_str(),
           r.prec.c_str(), r.targets, r.sources, r.groups, r.pairs, r.best_s, r.rate,
           r.spot_error);
  printf("CHECK rate spot checks %s (eight targets per case against a host direct sum)\n",
         g_spot_ok ? "PASS" : "FAIL");
}

}  // namespace

int main(int argc, char** argv) {
  Kokkos::ScopeGuard kokkos(argc, argv);
  double seconds = 2.0;
  int64_t max_targets = 100000000;
  bool do_checks = true, do_rate = true;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
    else if (a == "--max-targets" && i + 1 < argc) max_targets = (int64_t)atof(argv[++i]);
    else if (a == "--no-checks") do_checks = false;
    else if (a == "--no-rate") do_rate = false;
    else {
      fprintf(stderr, "usage: unbind_rate [--seconds S] [--max-targets N] [--no-checks] "
                      "[--no-rate]\n");
      return 2;
    }
  }
  printf("unbind_rate: execution space %s, concurrency %d\n", Exec().name(),
         Exec().concurrency());
  bool ok = true;
  if (do_checks) {
    ok = checkKernel() && ok;
    ok = checkUnbinding() && ok;
  }
  if (do_rate) {
    rates(seconds, max_targets);
    ok = ok && g_spot_ok;
  }
  printf("\nUNBIND_RATE %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
