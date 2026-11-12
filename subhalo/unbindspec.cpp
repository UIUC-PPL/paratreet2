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
//              [--potential-ratio] [--cap C [--refine K]] [--first-origin file]
//              [--carried | --carried-kernel]
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
// --first-origin <file> has lines "group id, particle id" (the format of
// unbind --first-origin): the particle a group's FIRST frame takes its
// nearest-image offsets from, instead of the group's lowest particle id. It
// matters only for a group at least half the box wide. HBT-HERONS's own
// choice is the first particle of a list it has not sorted yet.
//
// --refine K (with --cap) adds the centre refinement ("Centre refinement"):
// in each group whose bound set ends above the cap, the K bound particles of
// lowest energy get their energy again from all the bound particles, and the
// lowest of those is the most bound particle. The output then has each
// group's most bound particle as the group's first record (every group: the
// particle of lowest energy where nothing was refined), as HBT-HERONS orders
// its own; the energies written are still the last pass's. A second file,
// the output's name with .particles.bin replaced by .centre.bin, holds the
// K refined particles of each refined group, 24-byte records with the new
// energies, most bound first.
//
// --carried runs the passes as HBT-HERONS's source carries them
// (ubs::unbindCarried): after the first pass that removes fewer particles
// than it keeps, energies are corrected from pass to pass instead of computed
// afresh. Not with --cap. --carried-kernel is the same with the one
// difference the kernel keeps on purpose: after a pass that removes at least
// as many particles as it keeps, the next pass is computed in full, where
// HBT-HERONS corrects from a frame two passes old.
//
// Build: make unbindspec (subhalo/Makefile; HDF5 and OpenMP).

#include "unbindrec.h"
#include "unbindspec.h"

#include <cstring>
#include <unordered_map>

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: unbindspec <snapshot.hdf5> <out.particles.bin> "
                    "[--g G] [--no-hubble] [--trace ID] [--potential-ratio] [--cap C "
                    "[--refine K]] [--first-origin file] [--carried | --carried-kernel]\n");
    return 2;
  }
  long long trace_id = -1, cap = 0, refine = 0;
  bool potential_ratio = false, carried = false, kernel_rule = false;
  const char* origin_path = nullptr;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--g") && i + 1 < argc) ubs::G = atof(argv[++i]);
    else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_id = atoll(argv[++i]);
    else if (!strcmp(argv[i], "--no-hubble")) ubs::hubble_factor = 0;
    else if (!strcmp(argv[i], "--potential-ratio")) potential_ratio = true;
    else if (!strcmp(argv[i], "--cap") && i + 1 < argc) cap = atoll(argv[++i]);
    else if (!strcmp(argv[i], "--refine") && i + 1 < argc) refine = atoll(argv[++i]);
    else if (!strcmp(argv[i], "--first-origin") && i + 1 < argc) origin_path = argv[++i];
    else if (!strcmp(argv[i], "--carried")) carried = true;
    else if (!strcmp(argv[i], "--carried-kernel")) carried = kernel_rule = true;
    else { fprintf(stderr, "unbindspec: unknown option %s\n", argv[i]); return 2; }
  }
  if (carried && cap > 0) {
    fprintf(stderr, "unbindspec: --carried is not defined with --cap\n");
    return 2;
  }
  if (refine < 0 || (refine > 0 && cap <= 0)) {
    fprintf(stderr, "unbindspec: --refine needs a positive K and --cap\n");
    return 2;
  }
  std::string centre_path;
  if (refine > 0) {
    centre_path = argv[2];
    const std::string suffix = ".particles.bin";
    if (centre_path.size() > suffix.size() &&
        centre_path.compare(centre_path.size() - suffix.size(), suffix.size(), suffix) == 0)
      centre_path.resize(centre_path.size() - suffix.size());
    centre_path += ".centre.bin";
  }
  const ubs::Snapshot s = ubs::readSnapshot(argv[1], "unbindspec");
  const size_t ng = s.groups.size();
  std::unordered_map<long long, long long> origin_of;   // group id -> particle id
  if (origin_path) {
    FILE* f = fopen(origin_path, "r");
    if (!f) { fprintf(stderr, "unbindspec: cannot open %s\n", origin_path); return 2; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
      long long g, id;
      if (line[0] != '#' && sscanf(line, "%lld %lld", &g, &id) == 2) origin_of[g] = id;
    }
    fclose(f);
  }
  std::vector<char> origin_found(ng, 0), stale(ng, 0);

  std::vector<std::vector<Rec>> out(ng);
  std::vector<std::vector<double>> ratio(ng);     // --potential-ratio: |U| / S
  std::vector<long> passes(ng, 0);
  std::vector<double> capped_terms(ng, 0.0);      // --cap: pair terms of a sampled group
  std::vector<std::vector<Rec>> centre(ng);       // --refine: the refined particles
  std::vector<long> centre_rank(ng, -1);          // --refine: the winner's rank in e[]

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
    size_t origin = ubs::NO_ORIGIN;
    if (auto it = origin_of.find(g.id); it != origin_of.end()) {
      for (size_t i = 0; i < n; i++)
        if (s.ID[idx[i]] == it->second) origin = i;
      origin_found[gi] = origin != ubs::NO_ORIGIN;
    }
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
      if (origin != ubs::NO_ORIGIN) {
        fprintf(stderr, "unbindspec: --first-origin is not defined for a sampled group\n");
        exit(2);
      }
      passes[gi] = ubs::unbindCapped(s, idx, n, alive, pot, e, inner, (size_t)cap, order,
                                     observe);
    } else {
      ubs::potentialAll(s, idx, n, pot, inner);
      bool st = false;
      passes[gi] = carried
          ? ubs::unbindCarried(s, idx, n, alive, pot, e, inner, observe, origin, &st,
                               kernel_rule)
          : ubs::unbind(s, idx, n, alive, pot, e, inner, observe, origin);
      stale[gi] = st;
    }
    size_t nalive = 0, first = n;   // first: the most bound particle (--refine)
    for (size_t i = 0; i < n; i++) nalive += alive[i] ? 1 : 0;
    if (refine > 0 && nalive > (size_t)cap) {
      std::vector<size_t> cand, rank;
      std::vector<double> ecand;
      ubs::refineCentre(s, idx, n, alive, e, (size_t)refine, cand, ecand, rank, inner);
      for (size_t r = 0; r < cand.size(); r++)
        centre[gi].push_back({s.ID[idx[cand[r]]], g.id, ecand[r]});
      centre_rank[gi] = (long)rank[0];
      first = cand[0];
    } else if (refine > 0) {
      for (size_t i = 0; i < n; i++)
        if (alive[i] && (first == n || e[i] < e[first] ||
                         (e[i] == e[first] && s.ID[idx[i]] < s.ID[idx[first]])))
          first = i;
    }
    if (first < n) out[gi].push_back({s.ID[idx[first]], g.id, e[first]});
    for (size_t i = 0; i < n; i++)
      if (alive[i] && i != first) out[gi].push_back({s.ID[idx[i]], g.id, e[i]});
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

  if (origin_path) {
    size_t found = 0;
    for (char c : origin_found) found += c;
    if (found != origin_of.size()) {
      fprintf(stderr, "unbindspec: --first-origin names %zu groups, %zu found with that "
                      "particle\n", origin_of.size(), found);
      return 2;
    }
    printf("unbindspec: first-frame origin given for %zu groups\n", found);
  }
  if (carried) {
    size_t nstale = 0;
    for (char c : stale) nstale += c;
    printf("unbindspec: passes carried as in %s; groups where a pass removed at least as "
           "many particles as it kept after a correction: %zu\n",
           kernel_rule ? "the kernel (--carried-kernel: the next pass in full)"
                       : "HBT-HERONS's source (--carried: the next correction starts from "
                         "a frame two passes old)", nstale);
    size_t shown = 0;
    for (size_t gi = 0; gi < ng && shown < 10; gi++)
      if (stale[gi]) {
        printf("unbindspec:   group %lld (%zu particles, %zu bound at the end)\n",
               s.groups[gi].id, s.groups[gi].n, out[gi].size());
        shown++;
      }
  }
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
  if (refine > 0) {
    FILE* c = fopen(centre_path.c_str(), "wb");
    if (!c) { fprintf(stderr, "unbindspec: cannot write %s\n", centre_path.c_str()); return 2; }
    long long refined = 0, moved = 0, upper = 0;
    long top = 0;
    double terms = 0;
    for (size_t gi = 0; gi < ng; gi++) {
      if (centre[gi].empty()) continue;
      refined++;
      terms += (double)centre[gi].size() * (double)out[gi].size();
      top = std::max(top, centre_rank[gi]);
      moved += centre_rank[gi] > 0;
      upper += 2 * centre_rank[gi] >= (long)centre[gi].size();
      fwrite(centre[gi].data(), sizeof(Rec), centre[gi].size(), c);
    }
    fclose(c);
    printf("unbindspec: centre refinement of %lld candidates: %lld groups, %.3e pair terms; "
           "the most bound particle is not the last pass's lowest energy in %lld of them; "
           "its rank in the last pass's order: largest %ld, in the upper half of the "
           "candidates in %lld groups; wrote %s\n",
           refine, refined, terms, moved, top, upper, centre_path.c_str());
  }
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
