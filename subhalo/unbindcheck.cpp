// unbindcheck -- compare a kernel's unbinding result with the exact
// reference under the amended pass rule of design/subhalo-checks.md
// ("Pass rule, amended").
//
//   unbindcheck <snapshot.hdf5> <reference.particles.bin> <candidate> --tau T
//               [--single-against <double result>] [--cap C] [--list N]
//               [--first-origin FILE]
//
//   <snapshot.hdf5>            the file tipsy2hbt wrote, which both sides read
//   <reference.particles.bin>  hbtref's output for the exact HBT-HERONS run,
//                              in the snapshot's numbering (not the .fof3 file)
//   <candidate>                the file `unbind --dump` writes, or a
//                              .particles.bin file in the snapshot's numbering
//   --tau T                    the tolerance (0.00141 for 8m-lambb)
//   --single-against F         for a single-precision kernel: F is the same
//                              kernel's double-precision result on the same
//                              input (a --dump file or a .particles.bin
//                              file). The tolerance becomes
//                              min(T + 10 n, 10 T), n being the 99.9th
//                              percentile of |E_single - E_double| / S over
//                              the particles bound in both.
//   --cap C                    both results used capped sampling with cap C
//                              ("Capped sampling: definition and pass rule"):
//                              wherever the rule evaluates or replays the
//                              definition in a group of more than C
//                              particles, it uses the capped definition.
//                              The rule itself is unchanged.
//   --list N                   print at most N differing groups (default 20)
//   --first-origin FILE        both results took each group's first frame
//                              from the particle FILE names (lines "group
//                              id, particle id"; HBT-HERONS's first
//                              particle, see wideorigins.py). Wherever the
//                              rule evaluates or replays the definition's
//                              first pass, it uses that particle. The rule
//                              itself is unchanged. The passes are the
//                              definition's (every pass afresh), not
//                              unbindspec --carried. Not defined for a
//                              group sampled under --cap.
//
// Both sides must have run until a pass removes nothing: HBT-HERONS with
// BoundMassPrecision 1, `unbind --bound-fraction 1`. A result that stopped
// early is not an end state and fails 2a.
//
// Why the rule as first written (unbindcmp) is not enough: removing
// particles until none has E >= 0 can end on different bound sets. A
// particle within rounding of E = 0 on an early pass is removed by one side
// and kept by the other, and the two then go separate ways: the kept
// particle can end well inside, and its neighbours' energies move with it.
// Both end states satisfy the definition.
//
// The amended rule. S is the median of |E| over a group's bound particles in
// the reference.
//   1. Energies. In a group whose bound sets are equal, each particle is
//      compared with the reference. In a group whose bound sets differ, each
//      of the candidate's bound particles is compared with the definition
//      evaluated in double precision on the candidate's own bound set. Over
//      all of them, at least 99.9% within T*S and none beyond 10*T*S.
//   2. Groups whose bound sets differ. In each one:
//      a. the candidate's set is an end state: evaluated on that set, no
//         particle of it has E >= T*S;
//      b. the difference starts at the boundary. The definition is replayed
//         from the whole group. P is the first pass at which a differing
//         particle is within T*S of zero: one the reference removes and the
//         candidate keeps (0 <= E <= T*S when removed), or one the reference
//         keeps and the candidate removes (-T*S <= E < 0 at that pass). No
//         particle the candidate keeps may be removed before P, or at P with
//         E > T*S.
//   3. Groups. A group has a sub-halo in only one result only if its bound
//      count there is within 2 of the 20-particle minimum.
// It also prints the verdict of the rule as first written.
//
// Single-precision kernels get a looser tolerance (--single-against). T was
// set as ten times the difference between HBT-HERONS's single- and
// double-precision builds; a kernel's own single-precision rounding is
// measured the same way, against the same kernel in double precision, and
// ten times it is added. Neither measurement involves the comparison being
// judged. The double-precision kernel must pass at T by itself first, and
// the allowance stops at 10 T so that an error cannot buy its own tolerance.
//
// Exit status 0 = pass (amended rule), 1 = fail, 2 = usage or input error.
//
// Build: make unbindcheck (subhalo/Makefile; HDF5 and OpenMP).

#include "unbindrec.h"
#include "unbindspec.h"

#include <climits>
#include <cstring>
#include <unordered_map>

static const char* TOOL = "unbindcheck";

static bool isHdf5(const char* path) {
  static const unsigned char sig[8] = {0x89, 'H', 'D', 'F', '\r', '\n', 0x1a, '\n'};
  unsigned char b[8] = {0};
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "%s: cannot open %s\n", TOOL, path); exit(2); }
  const size_t got = fread(b, 1, 8, f);
  fclose(f);
  return got == 8 && !memcmp(b, sig, 8);
}

// The bound particles of the file `unbind --dump` writes.
static std::vector<Rec> loadDump(const char* path) {
  hid_t f = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (f < 0) { fprintf(stderr, "%s: cannot open %s\n", TOOL, path); exit(2); }
  const auto id = ubs::detail::dset<long long>(f, "ParticleIDs", H5T_NATIVE_LLONG, 1, TOOL);
  const auto grp = ubs::detail::dset<long long>(f, "GroupIDs", H5T_NATIVE_LLONG, 1, TOOL);
  const auto bound = ubs::detail::dset<unsigned char>(f, "Bound", H5T_NATIVE_UINT8, 1, TOOL);
  const auto e = ubs::detail::dset<double>(f, "Energy", H5T_NATIVE_DOUBLE, 1, TOOL);
  H5Fclose(f);
  if (grp.size() != id.size() || bound.size() != id.size() || e.size() != id.size()) {
    fprintf(stderr, "%s: the datasets of %s have different lengths\n", TOOL, path);
    exit(2);
  }
  std::vector<Rec> v;
  for (size_t i = 0; i < id.size(); i++)
    if (bound[i]) v.push_back({id[i], grp[i], e[i]});
  sortRecords(v, path, TOOL);
  return v;
}

struct GroupResult {
  long long id = 0, nref = 0, ncand = 0, only_ref = 0, only_cand = 0;
  bool replay_ok = false;        // the replay ends on the reference's set
  long first_pass = 0;           // P, 0 if no differing particle is at the boundary
  long long first_id = -1;       // the particle that sets P
  double first_margin = 0;       // its E/S at that pass
  long long offenders = 0;       // kept by the candidate, removed before P or beyond T at P
  double worst_offender = 0;     // largest E/S at removal among them
  double max_self = 0;           // largest |E_candidate - E_definition| / S
  double max_estar = -1e300;     // largest E_definition / S over the candidate's set
  bool end_state = false, origin = false;
  std::vector<double> self;      // |E_candidate - E_definition| / S, every bound particle
  bool ok() const { return replay_ok && end_state && origin; }
};

int main(int argc, char** argv) {
  double tau = -1;
  long list = 20;
  long long cap = 0;
  const char* single_against = nullptr;
  const char* origin_path = nullptr;
  for (int i = 4; i < argc; i++) {
    if (!strcmp(argv[i], "--tau") && i + 1 < argc) tau = atof(argv[++i]);
    else if (!strcmp(argv[i], "--single-against") && i + 1 < argc) single_against = argv[++i];
    else if (!strcmp(argv[i], "--list") && i + 1 < argc) list = atol(argv[++i]);
    else if (!strcmp(argv[i], "--cap") && i + 1 < argc) cap = atoll(argv[++i]);
    else if (!strcmp(argv[i], "--first-origin") && i + 1 < argc) origin_path = argv[++i];
    else { fprintf(stderr, "%s: unknown option %s\n", TOOL, argv[i]); return 2; }
  }
  if (argc < 4 || tau <= 0) {
    fprintf(stderr, "usage: unbindcheck <snapshot.hdf5> <reference.particles.bin> "
                    "<candidate: unbind --dump file or .particles.bin> --tau T "
                    "[--single-against <double-precision result>] [--cap C] [--list N] "
                    "[--first-origin FILE]\n");
    return 2;
  }

  const std::vector<Rec> ref = loadRecords(argv[2], TOOL);
  const std::vector<Rec> cand = isHdf5(argv[3]) ? loadDump(argv[3]) : loadRecords(argv[3], TOOL);
  std::unordered_map<long long, long long> nref, ncand;
  const auto sref = groupScales(ref, nref);
  const auto scand = groupScales(cand, ncand);
  auto scale = [&](long long g) {
    auto it = sref.find(g);
    return it != sref.end() ? it->second : scand.at(g);
  };

  // A single-precision kernel: widen the tolerance by ten times its own
  // rounding, measured against the same kernel in double precision.
  if (single_against) {
    const std::vector<Rec> dbl = isHdf5(single_against) ? loadDump(single_against)
                                                        : loadRecords(single_against, TOOL);
    std::vector<double> d;
    for (size_t i = 0, j = 0; i < dbl.size() && j < cand.size();) {
      if (dbl[i].id < cand[j].id) i++;
      else if (cand[j].id < dbl[i].id) j++;
      else {
        d.push_back(std::fabs(cand[j].e - dbl[i].e) / scale(cand[j].group));
        i++; j++;
      }
    }
    const double p50 = percentile(d, 0.5), n = percentile(d, 0.999);
    const double top = d.empty() ? 0 : *std::max_element(d.begin(), d.end());
    const double widened = std::min(tau + 10 * n, 10 * tau);
    printf("single-precision allowance, against %s: %zu bound particles there, %zu "
           "bound in both;\n  |dE|/S median %.3g, 99.9%% n = %.3g, largest %.3g; "
           "tolerance = min(%.3g + 10 n, 10 * %.3g) = %.3g%s\n\n", single_against,
           dbl.size(), d.size(), p50, n, top, tau, tau, widened,
           tau + 10 * n > 10 * tau ? " (stopped at 10 tau)" : "");
    tau = widened;
  }

  // First merge by particle id: which groups differ.
  struct Diff { long long only_ref = 0, only_cand = 0; };
  std::unordered_map<long long, Diff> diff;
  long long moved = 0, n_both = 0, n_only_ref = 0, n_only_cand = 0;
  for (size_t i = 0, j = 0; i < ref.size() || j < cand.size();) {
    if (j == cand.size() || (i < ref.size() && ref[i].id < cand[j].id)) {
      diff[ref[i].group].only_ref++; n_only_ref++; i++;
    } else if (i == ref.size() || cand[j].id < ref[i].id) {
      diff[cand[j].group].only_cand++; n_only_cand++; j++;
    } else {
      if (ref[i].group != cand[j].group) moved++;
      n_both++; i++; j++;
    }
  }
  auto differing = [&](long long g) {
    return diff.count(g) && nref.count(g) && ncand.count(g);
  };

  // Second merge: the differences. `all` is the rule as written (every
  // particle bound in both); `pool` keeps only the groups with equal sets.
  std::vector<double> all, pool, ref_only, cand_only;
  for (size_t i = 0, j = 0; i < ref.size() || j < cand.size();) {
    if (j == cand.size() || (i < ref.size() && ref[i].id < cand[j].id)) {
      ref_only.push_back(std::fabs(ref[i].e) / scale(ref[i].group)); i++;
    } else if (i == ref.size() || cand[j].id < ref[i].id) {
      cand_only.push_back(std::fabs(cand[j].e) / scale(cand[j].group)); j++;
    } else {
      const double d = std::fabs(cand[j].e - ref[i].e) / scale(ref[i].group);
      all.push_back(d);
      if (!diff.count(ref[i].group)) pool.push_back(d);
      i++; j++;
    }
  }

  long long g_ref_only = 0, g_cand_only = 0, g_bad = 0, g_equal = 0, g_differ = 0;
  for (auto& kv : nref) {
    if (!ncand.count(kv.first)) { g_ref_only++; if (kv.second > 22) g_bad++; }
    else if (diff.count(kv.first)) g_differ++;
    else g_equal++;
  }
  for (auto& kv : ncand)
    if (!nref.count(kv.first)) { g_cand_only++; if (kv.second > 22) g_bad++; }

  printf("reference: %zu bound particles in %zu groups\n", ref.size(), nref.size());
  printf("candidate: %zu bound particles in %zu groups\n", cand.size(), ncand.size());
  printf("bound in both: %lld; only in reference: %lld; only in candidate: %lld\n",
         n_both, n_only_ref, n_only_cand);
  if (moved) printf("bound in both but in different groups: %lld\n", moved);
  printf("groups: %lld with equal bound sets, %lld with different bound sets, "
         "%lld with a sub-halo on one side only\n", g_equal, g_differ,
         g_ref_only + g_cand_only);
  {
    const double p50 = percentile(all, 0.5), p99 = percentile(all, 0.99),
                 p999 = percentile(all, 0.999);
    const double top = all.empty() ? 0 : *std::max_element(all.begin(), all.end());
    printf("|dE|/S over particles bound in both: median %.3g, 99%% %.3g, 99.9%% %.3g, "
           "largest %.3g\n", p50, p99, p999, top);
  }

  // ---- the rule as first written (what unbindcmp prints) -------------------
  {
    long long within = 0, beyond = 0, band = 0, flips_out = 0;
    for (double d : all) { if (d <= tau) within++; if (d > 10 * tau) beyond++; }
    for (const Rec& r : ref) if (std::fabs(r.e) <= tau * scale(r.group)) band++;
    for (double d : ref_only) if (d > tau) flips_out++;
    for (double d : cand_only) if (d > tau) flips_out++;
    const long long flips = (long long)(ref_only.size() + cand_only.size());
    const double frac = all.empty() ? 1.0 : (double)within / all.size();
    const bool c1 = frac >= 0.999 && beyond == 0 && moved == 0;
    const bool c2 = flips_out == 0 && flips <= band;
    const bool c3 = g_bad == 0;
    printf("\nrule as first written, tau = %.3g: %s (energies %s: %.4f%% within, %lld "
           "beyond 10 tau; membership %s: %lld of %lld one-sided particles outside the "
           "band; groups %s)\n", tau, c1 && c2 && c3 ? "PASS" : "FAIL",
           c1 ? "pass" : "FAIL", 100 * frac, beyond, c2 ? "pass" : "FAIL", flips_out,
           flips, c3 ? "pass" : "FAIL");
  }

  // ---- the groups whose bound sets differ ----------------------------------
  std::vector<GroupResult> res;
  if (g_differ > 0) {
    const ubs::Snapshot s = ubs::readSnapshot(argv[1], TOOL);
    std::unordered_map<long long, long long> origins;
    if (origin_path) origins = ubs::readFirstOrigins(origin_path, TOOL);
    std::unordered_map<long long, size_t> gindex;
    for (size_t gi = 0; gi < s.groups.size(); gi++) gindex[s.groups[gi].id] = gi;
    std::unordered_map<long long, std::vector<Rec>> refRecs, candRecs;
    for (const Rec& r : ref) if (differing(r.group)) refRecs[r.group].push_back(r);
    for (const Rec& r : cand) if (differing(r.group)) candRecs[r.group].push_back(r);

    std::vector<size_t> todo;                 // snapshot group indices, largest first
    for (auto& kv : refRecs) {
      auto it = gindex.find(kv.first);
      if (it == gindex.end()) {
        fprintf(stderr, "%s: group %lld is not in %s\n", TOOL, kv.first, argv[1]);
        return 2;
      }
      todo.push_back(it->second);
    }
    std::sort(todo.begin(), todo.end());
    res.resize(todo.size());

    auto judge = [&](size_t t, bool par) {
      const ubs::Group& g = s.groups[todo[t]];
      const size_t n = g.n;
      const size_t* idx = s.order.data() + g.begin;
      GroupResult& R = res[t];
      R.id = g.id;
      R.nref = nref.at(g.id);
      R.ncand = ncand.at(g.id);
      R.only_ref = diff.at(g.id).only_ref;
      R.only_cand = diff.at(g.id).only_cand;
      const double S = sref.at(g.id);
      // --first-origin: the particle this group's first frame starts from.
      const size_t first_origin = ubs::firstOrigin(s, g, origins, TOOL);

      std::unordered_map<long long, size_t> local;
      local.reserve(2 * n);
      for (size_t i = 0; i < n; i++) local[s.ID[idx[i]]] = i;
      std::vector<char> inRef(n, 0), inCand(n, 0);
      std::vector<double> eCand(n, 0.0);
      auto place = [&](const Rec& r) {
        auto it = local.find(r.id);
        if (it == local.end()) {
          fprintf(stderr, "%s: particle %lld is not in group %lld of the snapshot\n",
                  TOOL, r.id, g.id);
          exit(2);
        }
        return it->second;
      };
      for (const Rec& r : refRecs.at(g.id)) inRef[place(r)] = 1;
      for (const Rec& r : candRecs.at(g.id)) {
        const size_t i = place(r);
        inCand[i] = 1;
        eCand[i] = r.e;
      }

      // With --cap, a group of more than `cap` particles is evaluated and
      // replayed by the capped definition; every other group as before.
      const bool sampled = cap > 0 && n > (size_t)cap;
      std::vector<size_t> order;
      std::vector<double> pot0;
      if (sampled) order = ubs::sampleOrder(s, idx, n);
      else ubs::potentialAll(s, idx, n, pot0, par);

      // 2b: replay the definition from the whole group.
      std::vector<double> pot = pot0, e(n, 0.0), rem_e(n, 0.0), marg_e(n, 0.0);
      std::vector<char> alive(n, 1);
      std::vector<long> rem_pass(n, 0), marg_pass(n, 0);
      auto watch = [&](long pass, size_t, const std::vector<char>& al,
                       const std::vector<double>& en, const std::vector<size_t>& removed) {
        for (size_t r : removed) { rem_pass[r] = pass; rem_e[r] = en[r]; }
        for (size_t i = 0; i < n; i++)
          if (al[i] && inRef[i] && !inCand[i] && marg_pass[i] == 0 &&
              en[i] < 0 && -en[i] <= tau * S) {
            marg_pass[i] = pass;
            marg_e[i] = en[i];
          }
      };
      if (sampled && first_origin != ubs::NO_ORIGIN) {
        // As in unbindspec: the capped definition has no first-frame origin.
        fprintf(stderr, "%s: --first-origin is not defined for a sampled group (%lld)\n",
                TOOL, g.id);
        exit(2);
      }
      if (sampled)
        ubs::unbindCapped(s, idx, n, alive, pot, e, par, (size_t)cap, order, watch);
      else
        ubs::unbind(s, idx, n, alive, pot, e, par, watch, first_origin);
      R.replay_ok = true;
      for (size_t i = 0; i < n; i++)
        if ((alive[i] != 0) != (inRef[i] != 0)) R.replay_ok = false;
      long P = LONG_MAX;
      for (size_t i = 0; i < n; i++) {
        if (inCand[i] && !inRef[i] && rem_pass[i] > 0 && rem_e[i] <= tau * S &&
            rem_pass[i] < P) {
          P = rem_pass[i]; R.first_id = s.ID[idx[i]]; R.first_margin = rem_e[i] / S;
        }
        if (inRef[i] && !inCand[i] && marg_pass[i] > 0 && marg_pass[i] < P) {
          P = marg_pass[i]; R.first_id = s.ID[idx[i]]; R.first_margin = marg_e[i] / S;
        }
      }
      for (size_t i = 0; i < n; i++) {
        if (!(inCand[i] && !inRef[i]) || rem_pass[i] == 0) continue;
        if (rem_pass[i] < P || (rem_pass[i] == P && rem_e[i] > tau * S)) {
          R.offenders++;
          R.worst_offender = std::max(R.worst_offender, rem_e[i] / S);
        }
      }
      R.first_pass = P == LONG_MAX ? 0 : P;
      R.origin = P != LONG_MAX && R.offenders == 0;

      // 1 and 2a: the definition on the candidate's own bound set.
      std::vector<size_t> outside;
      for (size_t i = 0; i < n; i++) if (!inCand[i]) outside.push_back(i);
      std::vector<double> potk = pot0, estar(n, 0.0);
      if (sampled)
        ubs::potentialCapped(s, idx, n, inCand, order, (size_t)cap, potk, par);
      else
        ubs::subtractPotential(s, idx, n, inCand, outside, potk, par);
      // A candidate that kept the whole group is still in its first frame.
      ubs::energies(s, idx, n, inCand, potk, estar,
                    outside.empty() ? first_origin : ubs::NO_ORIGIN);
      for (size_t i = 0; i < n; i++) {
        if (!inCand[i]) continue;
        const double d = std::fabs(eCand[i] - estar[i]) / S;
        R.self.push_back(d);
        R.max_self = std::max(R.max_self, d);
        R.max_estar = std::max(R.max_estar, estar[i] / S);
      }
      R.end_state = R.max_estar < tau;
    };

    size_t nbig = 0;
    while (nbig < todo.size() && s.groups[todo[nbig]].n >= ubs::PARALLEL_FROM) nbig++;
    for (size_t t = 0; t < nbig; t++) judge(t, true);
#pragma omp parallel for schedule(dynamic, 4)
    for (size_t t = nbig; t < todo.size(); t++) judge(t, false);
  }

  // ---- the amended rule ------------------------------------------------------
  long long within = 0, beyond = 0, total = (long long)pool.size();
  for (double d : pool) { if (d <= tau) within++; if (d > 10 * tau) beyond++; }
  long long bad_groups = 0, not_end = 0, not_origin = 0, not_replayed = 0;
  for (const GroupResult& R : res) {
    total += (long long)R.self.size();
    for (double d : R.self) { if (d <= tau) within++; if (d > 10 * tau) beyond++; }
    if (!R.ok()) bad_groups++;
    if (!R.end_state) not_end++;
    if (!R.origin) not_origin++;
    if (!R.replay_ok) not_replayed++;
  }
  const double frac = total ? (double)within / total : 1.0;
  const bool a1 = frac >= 0.999 && beyond == 0 && moved == 0;
  const bool a2 = bad_groups == 0;
  const bool a3 = g_bad == 0;
  printf("\namended rule, tau = %.3g\n", tau);
  printf("1. energies:  %.4f%% of %lld within tau*S, %lld beyond 10*tau*S: %s\n",
         100 * frac, total, beyond, a1 ? "pass" : "FAIL");
  printf("2. groups with different bound sets: %lld, of which %lld fail (%lld not an "
         "end state, %lld do not start at the boundary, %lld where the replay does not "
         "end on the reference's set): %s\n", g_differ, bad_groups, not_end, not_origin,
         not_replayed, a2 ? "pass" : "FAIL");
  printf("3. groups with a sub-halo on one side only: %lld, of which %lld have more "
         "than 22 bound particles: %s\n", g_ref_only + g_cand_only, g_bad,
         a3 ? "pass" : "FAIL");
  printf("%s\n", a1 && a2 && a3 ? "PASS" : "FAIL");

  if (!res.empty() && list > 0) {
    printf("\ngroups with different bound sets, largest first (energies as fractions of "
           "the group's median |E|):\n");
    printf("%10s %9s %9s %8s %8s %6s %12s %10s %10s %10s  %s\n", "group", "ref", "cand",
           "only ref", "only cnd", "pass P", "particle", "E/S at P", "max |dE|", "max E own",
           "verdict");
    long shown = 0;
    // Failures first, then the rest, each largest first (res is in that order).
    for (int want_ok = 0; want_ok <= 1; want_ok++)
      for (const GroupResult& R : res) {
        if ((R.ok() ? 1 : 0) != want_ok || shown >= list) continue;
        const char* why = R.ok() ? "pass"
                          : !R.replay_ok ? "FAIL: replay does not end on the reference's set"
                          : !R.origin ? "FAIL: does not start at the boundary"
                          : "FAIL: not an end state";
        printf("%10lld %9lld %9lld %8lld %8lld %6ld %12lld %10.3g %10.3g %10.3g  %s",
               R.id, R.nref, R.ncand, R.only_ref, R.only_cand, R.first_pass, R.first_id,
               R.first_margin, R.max_self, R.max_estar, why);
        if (R.offenders)
          printf(" (%lld kept particles removed off the boundary, up to E/S %.3g)",
                 R.offenders, R.worst_offender);
        printf("\n");
        shown++;
      }
    if ((long)res.size() > shown) printf("... %zu more\n", res.size() - shown);
  }
  return a1 && a2 && a3 ? 0 : 1;
}
