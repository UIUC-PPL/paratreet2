// unbindcmp -- compare two unbinding results particle by particle and apply
// the pass rule of design/subhalo-checks.md ("Exact reference for the
// unbinding kernel").
//
//   unbindcmp <reference.particles.bin> <candidate.particles.bin> [--tau T]
//
// Both files hold one 24-byte record per BOUND particle, native byte order,
// no header: int64 particle id, int64 group id, float64 energy per unit mass
// in (km/s)^2 (unbindrec.h). hbtref writes the reference from an HBT-HERONS
// output; a kernel writes the same layout. Plain C++, no libraries.
//
// This is the rule as first written. unbindcheck applies the amended rule,
// which also judges groups whose bound sets differ, and reads the file
// `unbind --dump` writes.
//
// For each group, S is the median of |E| over the reference's bound
// particles. Differences are reported in units of S.
//
// Without --tau it only reports: the 99.9th percentile n of |dE|/S over the
// particles bound in both, and tau = max(1e-4, 10 n). That is how tau is set
// from two HBT-HERONS runs (single against double precision).
//
// With --tau T it gives a verdict on the three criteria:
//   1. energies: of the particles bound in both, at least 99.9% within T*S
//      and none beyond 10*T*S;
//   2. membership: every particle bound in only one result has |E| <= T*S
//      (E from the side that has it bound), and there are no more of them
//      than reference particles with |E| <= T*S;
//   3. groups: a group has a sub-halo in only one result only if its bound
//      count there is within 2 of the 20-particle minimum.
// Exit status 0 = pass, 1 = fail, 2 = usage or input error.
//
// Build: make unbindcmp, or c++ -O2 -std=c++17 unbindcmp.cpp -o unbindcmp

#include "unbindrec.h"

#include <cstring>
#include <string>

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: unbindcmp <reference.particles.bin> "
                    "<candidate.particles.bin> [--tau T]\n");
    return 2;
  }
  double tau = -1;
  for (int i = 3; i + 1 < argc; i++)
    if (!strcmp(argv[i], "--tau")) tau = atof(argv[i + 1]);

  const std::vector<Rec> ref = loadRecords(argv[1], "unbindcmp"),
                         cand = loadRecords(argv[2], "unbindcmp");
  std::unordered_map<long long, long long> nref, ncand;
  const auto sref = groupScales(ref, nref);
  const auto scand = groupScales(cand, ncand);
  auto scale = [&](long long g) {
    auto it = sref.find(g);
    if (it != sref.end()) return it->second;
    return scand.at(g);                       // group only in the candidate
  };

  // Merge by particle id.
  std::vector<double> both, ref_only, cand_only;
  long long moved = 0;                        // bound in both, different group
  size_t i = 0, j = 0;
  while (i < ref.size() || j < cand.size()) {
    if (j == cand.size() || (i < ref.size() && ref[i].id < cand[j].id)) {
      ref_only.push_back(std::fabs(ref[i].e) / scale(ref[i].group));
      i++;
    } else if (i == ref.size() || cand[j].id < ref[i].id) {
      cand_only.push_back(std::fabs(cand[j].e) / scale(cand[j].group));
      j++;
    } else {
      if (ref[i].group != cand[j].group) moved++;
      both.push_back(std::fabs(cand[j].e - ref[i].e) / scale(ref[i].group));
      i++; j++;
    }
  }

  printf("reference: %zu bound particles in %zu groups\n", ref.size(), nref.size());
  printf("candidate: %zu bound particles in %zu groups\n", cand.size(), ncand.size());
  printf("bound in both: %zu; only in reference: %zu; only in candidate: %zu\n",
         both.size(), ref_only.size(), cand_only.size());
  if (moved) printf("bound in both but in different groups: %lld\n", moved);
  const double p50 = percentile(both, 0.5), p99 = percentile(both, 0.99),
               p999 = percentile(both, 0.999);
  const double pmax = both.empty() ? 0 : *std::max_element(both.begin(), both.end());
  printf("|dE|/S over particles bound in both: median %.3g, 99%% %.3g, "
         "99.9%% %.3g, largest %.3g\n", p50, p99, p999, pmax);
  auto worst = [](const std::vector<double>& v) {
    return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
  };
  printf("|E|/S of particles bound in only one: largest %.3g (reference side), "
         "%.3g (candidate side)\n", worst(ref_only), worst(cand_only));

  // Groups with a sub-halo on one side only.
  long long g_ref_only = 0, g_cand_only = 0, g_bad = 0;
  for (auto& kv : nref)
    if (!ncand.count(kv.first)) { g_ref_only++; if (kv.second > 22) g_bad++; }
  for (auto& kv : ncand)
    if (!nref.count(kv.first)) { g_cand_only++; if (kv.second > 22) g_bad++; }
  printf("groups with a sub-halo only in reference: %lld; only in candidate: "
         "%lld; of these, with more than 22 bound particles: %lld\n",
         g_ref_only, g_cand_only, g_bad);

  if (tau < 0) {
    printf("\nnoise n = %.3g (99.9th percentile); tau = max(1e-4, 10 n) = %.3g\n",
           p999, std::max(1e-4, 10 * p999));
    return 0;
  }

  long long within = 0, beyond = 0, band = 0, flips_out = 0;
  for (double d : both) { if (d <= tau) within++; if (d > 10 * tau) beyond++; }
  for (const Rec& r : ref) if (std::fabs(r.e) <= tau * scale(r.group)) band++;
  for (double d : ref_only) if (d > tau) flips_out++;
  for (double d : cand_only) if (d > tau) flips_out++;
  const long long flips = (long long)(ref_only.size() + cand_only.size());
  const double frac = both.empty() ? 1.0 : (double)within / both.size();
  const bool c1 = frac >= 0.999 && beyond == 0 && moved == 0;
  const bool c2 = flips_out == 0 && flips <= band;
  const bool c3 = g_bad == 0;
  printf("\ntau = %.3g\n", tau);
  printf("1. energies:   %.4f%% within tau*S, %lld beyond 10*tau*S: %s\n",
         100 * frac, beyond, c1 ? "pass" : "FAIL");
  printf("2. membership: %lld particles bound in only one, %lld of them "
         "outside the band; %lld reference particles in the band: %s\n",
         flips, flips_out, band, c2 ? "pass" : "FAIL");
  printf("3. groups:     %lld one-sided groups above 22 bound particles: %s\n",
         g_bad, c3 ? "pass" : "FAIL");
  printf("%s\n", c1 && c2 && c3 ? "PASS" : "FAIL");
  return c1 && c2 && c3 ? 0 : 1;
}
