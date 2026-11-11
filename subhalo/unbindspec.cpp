// unbindspec -- a plain CPU run of the unbinding definition in
// design/subhalo-checks.md ("What HBT-HERONS computes"), in double precision
// with all pairs (unbindspec.h).
//
// It is not the kernel. It exists to check the written definition and the
// pass rule against HBT-HERONS: it passes unbindcmp against the exact
// reference, so the definition is the one HBT-HERONS implements and a
// correct kernel can pass too.
//
//   unbindspec <snapshot.hdf5> <out.particles.bin> [--g G] [--no-hubble] [--trace ID]
//              [--potential-ratio] [--cap C]
//
// <snapshot.hdf5> is the file tipsy2hbt wrote. The output has one 24-byte
// record per bound particle (unbindrec.h), the layout unbindcmp and
// unbindcheck read, in the snapshot's numbering.
//
// --g replaces HBT-HERONS's gravitational constant (43.0071), to show what a
// kernel with a slightly different constant would see. --no-hubble drops the
// Hubble term, a deliberately wrong result for testing the comparison tools.
// --trace prints one particle's energy at every pass of its group, to see
// where a difference between two results began. --potential-ratio prints
// how large the bound particles' potential term |U| is against S, their
// group's median |E|: a relative error eps in a kernel's potential moves an
// energy by eps * |U| / S in the units of the pass rule.
//
// --cap C applies the capped-sampling definition ("Capped sampling:
// definition and pass rule"): a group of more than C particles takes each
// pass's potential from a sample of C bound particles chosen by particle id.
// Groups of at most C particles go through the same code as without --cap.
//
// Build: make unbindspec (subhalo/Makefile; HDF5 and OpenMP).

#include "unbindrec.h"
#include "unbindspec.h"

#include <cstring>

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: unbindspec <snapshot.hdf5> <out.particles.bin> "
                    "[--g G] [--no-hubble] [--trace ID] [--potential-ratio] [--cap C]\n");
    return 2;
  }
  long long trace_id = -1, cap = 0;
  bool potential_ratio = false;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--g") && i + 1 < argc) ubs::G = atof(argv[++i]);
    else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_id = atoll(argv[++i]);
    else if (!strcmp(argv[i], "--no-hubble")) ubs::hubble_factor = 0;
    else if (!strcmp(argv[i], "--potential-ratio")) potential_ratio = true;
    else if (!strcmp(argv[i], "--cap") && i + 1 < argc) cap = atoll(argv[++i]);
    else { fprintf(stderr, "unbindspec: unknown option %s\n", argv[i]); return 2; }
  }
  const ubs::Snapshot s = ubs::readSnapshot(argv[1], "unbindspec");
  const size_t ng = s.groups.size();

  std::vector<std::vector<Rec>> out(ng);
  std::vector<std::vector<double>> ratio(ng);     // --potential-ratio: |U| / S
  std::vector<long> passes(ng, 0);
  std::vector<double> capped_terms(ng, 0.0);      // --cap: pair terms of a sampled group

  // One group. `inner` = parallelize over its particles (large groups).
  auto solve = [&](size_t gi, bool inner) {
    const ubs::Group& g = s.groups[gi];
    const size_t n = g.n;
    const size_t* idx = s.order.data() + g.begin;
    std::vector<double> pot, e(n, 0.0);
    std::vector<char> alive(n, 1);
    size_t traced = n;                        // local index of --trace, or n
    if (trace_id >= 0)
      for (size_t i = 0; i < n; i++)
        if (s.ID[idx[i]] == trace_id) traced = i;
    const bool sampled = cap > 0 && n > (size_t)cap;
    auto observe = [&](long pass, size_t entering, const std::vector<char>& al,
                       const std::vector<double>& en, const std::vector<size_t>& removed) {
      if (sampled)
        capped_terms[gi] += (double)entering * (double)std::min(entering, (size_t)cap);
      if (traced < n && al[traced])
        printf("trace: particle %lld group %lld pass %ld: %zu bound entering, "
               "%zu removed, E = %.9g\n", trace_id, g.id, pass, entering,
               removed.size(), en[traced]);
    };
    if (sampled) {
      const std::vector<size_t> order = ubs::sampleOrder(s, idx, n);
      passes[gi] = ubs::unbindCapped(s, idx, n, alive, pot, e, inner, (size_t)cap, order,
                                     observe);
    } else {
      ubs::potentialAll(s, idx, n, pot, inner);
      passes[gi] = ubs::unbind(s, idx, n, alive, pot, e, inner, observe);
    }
    for (size_t i = 0; i < n; i++)
      if (alive[i]) out[gi].push_back({s.ID[idx[i]], g.id, e[i]});
    if (potential_ratio && !out[gi].empty()) {
      std::vector<double> ae;
      for (const Rec& r : out[gi]) ae.push_back(std::fabs(r.e));
      const double S = percentile(ae, 0.5);
      for (size_t i = 0; i < n; i++)
        if (alive[i]) ratio[gi].push_back(std::fabs(pot[i] * ubs::G / s.a) / S);
    }
  };

  size_t nbig = 0;
  while (nbig < ng && s.groups[nbig].n >= ubs::PARALLEL_FROM) nbig++;
  for (size_t gi = 0; gi < nbig; gi++) solve(gi, true);
#pragma omp parallel for schedule(dynamic, 8)
  for (size_t gi = nbig; gi < ng; gi++) solve(gi, false);

  FILE* o = fopen(argv[2], "wb");
  if (!o) { fprintf(stderr, "unbindspec: cannot write %s\n", argv[2]); return 2; }
  long long bound = 0, with_sub = 0, total_passes = 0;
  long max_passes = 0;
  double pair_terms = 0, sampled_terms = 0;
  long long sampled_groups = 0;
  for (size_t gi = 0; gi < ng; gi++) {
    if (cap > 0 && s.groups[gi].n > (size_t)cap) {
      sampled_groups++;
      sampled_terms += capped_terms[gi];
    } else {
      pair_terms += (double)s.groups[gi].n * (double)s.groups[gi].n;
    }
    total_passes += passes[gi];
    max_passes = std::max(max_passes, passes[gi]);
    if (out[gi].empty()) continue;
    with_sub++;
    bound += (long long)out[gi].size();
    fwrite(out[gi].data(), sizeof(Rec), out[gi].size(), o);
  }
  fclose(o);
  printf("unbindspec: %zu groups, %zu grouped particles, %.3e pair terms in the first pass"
         "%s\n", ng, s.order.size(), pair_terms, cap > 0 ? " of the groups not sampled" : "");
  if (cap > 0)
    printf("unbindspec: cap %lld: %lld groups sampled, %.3e pair terms in all their passes\n",
           cap, sampled_groups, sampled_terms);
  printf("unbindspec: %lld groups with a sub-halo, %lld bound particles (%.2f%%)\n",
         with_sub, bound, 100.0 * bound / (double)s.order.size());
  printf("unbindspec: passes per group: mean %.2f, largest %ld\n",
         (double)total_passes / ng, max_passes);
  printf("unbindspec: box %g Mpc, a %g, H %g km/s/Mpc, softening %g Mpc, G %.7g%s\n",
         s.L, s.a, s.H, s.eps, ubs::G, ubs::hubble_factor == 1.0 ? "" : ", NO Hubble term");
  if (potential_ratio) {
    std::vector<double> all;
    for (auto& r : ratio) all.insert(all.end(), r.begin(), r.end());
    const double p50 = percentile(all, 0.5), p99 = percentile(all, 0.99),
                 p999 = percentile(all, 0.999);
    const double top = all.empty() ? 0 : *std::max_element(all.begin(), all.end());
    printf("unbindspec: |U|/S over the bound particles: median %.3g, 99%% %.3g, "
           "99.9%% %.3g, largest %.3g\n", p50, p99, p999, top);
  }
  return 0;
}
