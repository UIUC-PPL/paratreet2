// unbind -- unbind every FoF group of a snapshot written by tipsy2hbt, and
// compare the bound counts with HBT-HERONS's (design/subhalo-checks.md,
// step 1).
//
//   unbind <snapshot.hdf5> [options]
//     --ref <table>       per-group table to compare with: hbtstat's third
//                         argument, or another run's --out
//     --out <table>       write this run's per-group table, same columns
//     --dump <file.hdf5>  write every grouped particle: ParticleIDs,
//                         GroupIDs, Bound (0/1), Energy (as last evaluated)
//     --kernel f64|f32|f32f32   pair arithmetic / summation type
//                         (default f64; f32 sums in f64, f32f32 in f32)
//     --rsqrt             the kernel's -m * rsqrt(r2) form
//     --float-positions   round positions to single precision first, as
//                         HBT-HERONS built without HBT_REAL8 stores them
//     --open              ignore the periodic box
//     --cap C             capped sampling: a group of more than C particles
//                         takes each potential from C sources chosen by
//                         particle id ("Capped sampling: definition and
//                         pass rule" in the design note). Default: no cap
//     --refine K          with --cap: centre refinement. In each group whose
//                         bound set ends above the cap, the K bound particles
//                         of lowest energy get their energy again from all
//                         the bound particles; the lowest of those is the most
//                         bound particle ("Centre refinement" in the design
//                         note). The dump then also has MostBound (0/1, one
//                         per group with a sub-halo) and RefinedEnergy (the
//                         new energy of a refined particle, NaN otherwise)
//     --first-origin <file>   lines "group id, particle id": the particle a
//                         group's FIRST frame takes its nearest-image offsets
//                         from (default: the group's lowest particle id). It
//                         matters only for a group at least half the box
//                         wide, where HBT-HERONS's own choice depends on the
//                         order of its particle list
//     --min-bound N       smallest sub-halo (default 20)
//     --bound-fraction F  stop when a pass keeps this share (default 0.995)
//
// The rules are HBT-HERONS's with sampling off (unbind_groups.h): for the
// comparison HBT-HERONS has to be run with
//   MaxSampleSizeOfPotentialEstimate 0
// and a small TreeNodeOpenAngle, because its default potential is a sample
// of 1,000 sources walked through a tree with opening angle 0.45.
//
// What is read, all from the file, as HBT-HERONS's reader does
// (src/io/swiftsim_io/swiftsim_io.cpp): box side, scale factor, h and
// "H [internal units]"; the unit system, from which G = 43.0071 in units of
// (km/s)^2 Mpc/h per 1e10 Msun/h is scaled to the file's units; the dark
// matter softening (comoving, capped by the physical maximum over a); and
// PartType1 Coordinates (comoving), Velocities, Masses, ParticleIDs,
// FOFGroupIDs. Everything stays in the file's units.
//
// Serial host code, all in memory: for inputs of up to a few hundred
// million particles, like tipsy2hbt.
//
// Build: make unbind (subhalo/Makefile; Kokkos and HDF5).

#include "unbind_groups.h"

#include <hdf5.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

[[noreturn]] void die(const std::string& m) {
  fprintf(stderr, "unbind: %s\n", m.c_str());
  exit(1);
}

// A numeric attribute as doubles (obj may be a group or a dataset).
std::vector<double> attrDoubles(hid_t file, const char* obj, const char* name) {
  hid_t a = H5Aopen_by_name(file, obj, name, H5P_DEFAULT, H5P_DEFAULT);
  if (a < 0) die(std::string("no attribute ") + obj + ":" + name);
  hid_t sp = H5Aget_space(a);
  const hssize_t n = H5Sget_simple_extent_npoints(sp);
  std::vector<double> v((size_t)std::max<hssize_t>(n, 1));
  if (H5Aread(a, H5T_NATIVE_DOUBLE, v.data()) < 0)
    die(std::string("cannot read attribute ") + obj + ":" + name);
  H5Sclose(sp);
  H5Aclose(a);
  return v;
}
double attrDouble(hid_t file, const char* obj, const char* name) {
  return attrDoubles(file, obj, name)[0];
}
// A string attribute, fixed or variable length.
std::string attrString(hid_t file, const char* obj, const char* name) {
  hid_t a = H5Aopen_by_name(file, obj, name, H5P_DEFAULT, H5P_DEFAULT);
  if (a < 0) die(std::string("no attribute ") + obj + ":" + name);
  hid_t t = H5Aget_type(a);
  std::string out;
  if (H5Tis_variable_str(t) > 0) {
    char* p = nullptr;
    hid_t mt = H5Tcopy(H5T_C_S1);
    H5Tset_size(mt, H5T_VARIABLE);
    if (H5Aread(a, mt, &p) < 0 || !p) die(std::string("cannot read ") + name);
    out = p;
    H5free_memory(p);
    H5Tclose(mt);
  } else {
    const size_t len = H5Tget_size(t);
    std::vector<char> buf(len + 1, 0);
    hid_t mt = H5Tcopy(H5T_C_S1);
    H5Tset_size(mt, len);
    if (H5Aread(a, mt, buf.data()) < 0) die(std::string("cannot read ") + name);
    out = buf.data();
    H5Tclose(mt);
  }
  H5Tclose(t);
  H5Aclose(a);
  return out;
}
// A dataset of n rows by cols columns, converted to the memory type.
template <class T>
std::vector<T> dataset(hid_t file, const char* name, hid_t memtype, int cols) {
  hid_t d = H5Dopen2(file, name, H5P_DEFAULT);
  if (d < 0) die(std::string("no dataset ") + name);
  hid_t sp = H5Dget_space(d);
  hsize_t dims[2] = {0, 1};
  const int rank = H5Sget_simple_extent_ndims(sp);
  if (rank < 1 || rank > 2) die(std::string("unexpected rank for ") + name);
  H5Sget_simple_extent_dims(sp, dims, NULL);
  if ((int)dims[1] != cols) die(std::string("unexpected shape for ") + name);
  std::vector<T> v((size_t)dims[0] * cols);
  if (H5Dread(d, memtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, v.data()) < 0)
    die(std::string("cannot read ") + name);
  H5Sclose(sp);
  H5Dclose(d);
  return v;
}

struct Row {
  long long particles = 0, bound = 0;
  int passes = 0;
};
// hbtstat's table: host group id, particles in the group, bound particles,
// passes, time. Groups that are absent have no sub-halo.
std::unordered_map<long long, Row> readTable(const std::string& path) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f) die("cannot open " + path);
  std::unordered_map<long long, Row> t;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    long long id, n, b;
    int passes;
    double secs;
    if (sscanf(line, "%lld %lld %lld %d %lf", &id, &n, &b, &passes, &secs) != 5) continue;
    Row& r = t[id];
    r.particles = n;
    r.bound += b;
    r.passes = std::max(r.passes, passes);
  }
  fclose(f);
  return t;
}

}  // namespace

int main(int argc, char** argv) {
  Kokkos::ScopeGuard kokkos(argc, argv);
  std::string snap, ref_path, out_path, dump_path, origin_path, kernel = "f64";
  bool rsqrt = false, float_positions = false, open = false;
  unbind::Params P;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) die("missing value for " + a);
      return argv[++i];
    };
    if (a == "--ref") ref_path = next();
    else if (a == "--out") out_path = next();
    else if (a == "--dump") dump_path = next();
    else if (a == "--kernel") kernel = next();
    else if (a == "--rsqrt") rsqrt = true;
    else if (a == "--float-positions") float_positions = true;
    else if (a == "--open") open = true;
    else if (a == "--first-origin") origin_path = next();
    else if (a == "--min-bound") P.min_bound = atoi(next());
    else if (a == "--cap") P.cap = atoll(next());
    else if (a == "--refine") P.refine = atoll(next());
    else if (a == "--bound-fraction") P.bound_fraction = atof(next());
    else if (a[0] != '-' && snap.empty()) snap = a;
    else die("unknown argument " + a);
  }
  if (snap.empty() || (kernel != "f64" && kernel != "f32" && kernel != "f32f32")) {
    fprintf(stderr,
            "usage: unbind <snapshot.hdf5> [--ref table] [--out table] [--dump file.hdf5]\n"
            "              [--kernel f64|f32|f32f32] [--rsqrt] [--float-positions] [--open]\n"
            "              [--cap C [--refine K]] [--min-bound N] [--bound-fraction F]\n"
            "              [--first-origin file]\n");
    return 2;
  }

  // ---- read ----------------------------------------------------------------
  Kokkos::Timer timer;
  hid_t file = H5Fopen(snap.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file < 0) die("cannot open " + snap);
  const double box = attrDoubles(file, "Header", "BoxSize")[0];
  const double a = attrDouble(file, "Cosmology", "Scale-factor");
  const double h = attrDouble(file, "Cosmology", "h");
  const double Hz = attrDouble(file, "Cosmology", "H [internal units]");
  const double pc = attrDouble(file, "PhysicalConstants/CGS", "parsec");
  const double msun = attrDouble(file, "PhysicalConstants/CGS", "solar_mass");
  const double UL = attrDouble(file, "Units", "Unit length in cgs (U_L)");
  const double UM = attrDouble(file, "Units", "Unit mass in cgs (U_M)");
  const double Ut = attrDouble(file, "Units", "Unit time in cgs (U_t)");
  const double mass_msunh = UM / msun * h, length_mpch = UL / (1e6 * pc) * h;
  const double vel_kms = UL / Ut / 1e5;
  const long long null_id = std::stoll(attrString(file, "Parameters", "FOF:group_id_default"));
  // Parsed in single precision, as HBT-HERONS does.
  const double soft_comoving =
      std::stof(attrString(file, "Parameters", "Gravity:comoving_DM_softening"));
  const double soft_physical =
      std::stof(attrString(file, "Parameters", "Gravity:max_physical_DM_softening"));
  if (attrDouble(file, "PartType1/Coordinates", "a-scale exponent") != 1.0)
    die("Coordinates are not comoving");
  const double vexp = attrDouble(file, "PartType1/Velocities", "a-scale exponent");
  const double mexp = attrDouble(file, "PartType1/Masses", "a-scale exponent");
  std::vector<double> pos = dataset<double>(file, "PartType1/Coordinates", H5T_NATIVE_DOUBLE, 3);
  std::vector<double> vel = dataset<double>(file, "PartType1/Velocities", H5T_NATIVE_DOUBLE, 3);
  std::vector<double> mass = dataset<double>(file, "PartType1/Masses", H5T_NATIVE_DOUBLE, 1);
  std::vector<int64_t> gid = dataset<int64_t>(file, "PartType1/FOFGroupIDs", H5T_NATIVE_INT64, 1);
  std::vector<int64_t> pid = dataset<int64_t>(file, "PartType1/ParticleIDs", H5T_NATIVE_INT64, 1);
  H5Fclose(file);
  const int64_t n = (int64_t)mass.size();
  if ((int64_t)pos.size() != 3 * n || (int64_t)vel.size() != 3 * n ||
      (int64_t)gid.size() != n || (int64_t)pid.size() != n)
    die("datasets of PartType1 differ in length");
  const double vfac = std::pow(a, vexp), mfac = std::pow(a, mexp);
  for (double& v : vel) v *= vfac;
  for (double& m : mass) m *= mfac;
  if (float_positions)
    for (double& x : pos) x = (double)(float)x;

  P.G = 43.0071 * (mass_msunh / 1e10) / vel_kms / vel_kms / length_mpch;
  P.a = a;
  P.H = Hz;
  P.eps = soft_physical == -1 ? soft_comoving : std::min(soft_comoving, soft_physical / a);
  P.box = open ? 0.0 : box;
  const double read_s = timer.seconds();

  timer.reset();
  unbind::Groups G = unbind::makeGroups(n, pos.data(), vel.data(), mass.data(),
                                        gid.data(), (int64_t)null_id, P.box,
                                        (int64_t)P.min_bound, pid.data());
  if (!origin_path.empty()) {
    FILE* f = fopen(origin_path.c_str(), "r");
    if (!f) die("cannot open " + origin_path);
    std::unordered_map<long long, long long> want;
    char line[256];
    while (fgets(line, sizeof line, f)) {
      long long g, id;
      if (line[0] != '#' && sscanf(line, "%lld %lld", &g, &id) == 2) want[g] = id;
    }
    fclose(f);
    G.first_origin.assign(G.id.size(), -1);
    size_t found = 0;
    for (size_t g = 0; g < G.id.size(); g++) {
      auto it = want.find(G.id[g]);
      if (it == want.end()) continue;
      for (int64_t k = G.off[g]; k < G.off[g + 1]; k++)
        if (G.pid[k] == it->second) G.first_origin[g] = k;
      if (G.first_origin[g] < 0)
        die("--first-origin: particle " + std::to_string(it->second) + " is not in group " +
            std::to_string(it->first));
      found++;
    }
    if (found != want.size()) die("--first-origin names a group that is not in the snapshot");
    printf("unbind: first-frame origin given for %zu groups\n", found);
  }
  const double group_s = timer.seconds();
  const int ng = G.ng();
  int64_t largest = 0;
  for (int g = 0; g < ng; g++) largest = std::max(largest, G.off[g + 1] - G.off[g]);
  printf("unbind: %s\n", snap.c_str());
  printf("unbind: %lld particles; %d groups of %d or more holding %lld, largest %lld\n",
         (long long)n, ng, P.min_bound, (long long)G.n(), (long long)largest);
  printf("unbind: box %g, a %g, H %.6g, G %.6g, softening %.6g (file units); %s\n",
         box, P.a, P.H, P.G, P.eps, open ? "open boundaries" : "periodic");
  printf("unbind: groups at least half the box wide: %lld%s\n", (long long)G.wide_groups,
         G.wrap ? " (every pair separation takes its nearest image)" : "");
  printf("unbind: kernel %s%s on %s; positions %s; read %.2f s, grouping %.2f s\n",
         kernel.c_str(), rsqrt ? " rsqrt" : "", unbind::Exec().name(),
         float_positions ? "rounded to single precision" : "as stored", read_s, group_s);
  if (P.cap < 0) die("--cap must not be negative");
  if (P.refine < 0 || (P.refine > 0 && P.cap == 0)) die("--refine needs a positive K and --cap");
  if (P.refine > 0)
    printf("unbind: centre refinement of the %lld lowest energies in each group whose bound "
           "set ends above the cap\n", (long long)P.refine);
  if (P.cap > 0) {
    long long above = 0;
    for (int g = 0; g < ng; g++) above += G.off[g + 1] - G.off[g] > P.cap;
    printf("unbind: cap %lld sources per particle, chosen by particle id; %lld groups are "
           "above it\n", (long long)P.cap, above);
  }
  if (ng == 0) die("no groups");

  // ---- unbind --------------------------------------------------------------
  timer.reset();
  unbind::Result R;
  if (kernel == "f64")
    R = rsqrt ? unbind::unbindGroups<double, double, true>(G, P)
              : unbind::unbindGroups<double, double, false>(G, P);
  else if (kernel == "f32")
    R = rsqrt ? unbind::unbindGroups<float, double, true>(G, P)
              : unbind::unbindGroups<float, double, false>(G, P);
  else
    R = rsqrt ? unbind::unbindGroups<float, float, true>(G, P)
              : unbind::unbindGroups<float, float, false>(G, P);
  const double unbind_s = timer.seconds();
  const unbind::Stats& S = R.stats;

  long long alive = 0, bound = 0, passes = 0, stale = 0;
  int max_passes = 0;
  for (int g = 0; g < ng; g++) {
    alive += R.nbound[g] > 0;
    bound += R.nbound[g];
    stale += R.stale_frame[g];
    if (R.nbound[g] > 0) {
      passes += R.passes[g];
      max_passes = std::max(max_passes, R.passes[g]);
    }
  }
  printf("\ngroups with a self-bound sub-halo: %lld of %d (%lld have none)\n", alive, ng,
         ng - alive);
  printf("bound particles: %lld = %.1f%% of the particles in groups\n", bound,
         100.0 * bound / (double)G.n());
  printf("unbinding passes per sub-halo: mean %.2f, largest %d\n",
         alive ? (double)passes / alive : 0.0, max_passes);
  printf("groups where HBT-HERONS would move the kinetic term from a stale frame: %lld\n",
         stale);
  const double later =
      (double)(S.pairs_full_later + S.pairs_subtract + S.pairs_sample_update);
  printf("pair terms: first pass %.4g, later passes %.4g (in full %.4g, by subtraction "
         "%.4g) = %.1f%% of the first\n",
         (double)S.pairs_first, later, (double)S.pairs_full_later, (double)S.pairs_subtract,
         100.0 * later / (double)S.pairs_first);
  if (P.cap > 0)
    printf("capped sampling: %lld groups sampled; %.4g pair terms spent on changes of "
           "their samples in later passes\n", (long long)S.sampled_groups,
           (double)S.pairs_sample_update);
  if (P.refine > 0) {
    long long moved = 0, upper = 0, top = 0;
    for (int g = 0; g < ng; g++) {
      if (!R.refined_group[g]) continue;
      const long long k = std::min<long long>(P.refine, R.nbound[g]);
      top = std::max<long long>(top, R.centre_rank[g]);
      moved += R.centre_rank[g] > 0;
      upper += 2 * R.centre_rank[g] >= k;
    }
    printf("centre refinement: %lld groups, %.4g pair terms = %.1f%% of the first pass; the "
           "most bound particle is not the last pass's lowest energy in %lld of them; its "
           "rank in the last pass's order: largest %lld, in the upper half of the candidates "
           "in %lld groups\n", (long long)S.refined_groups, (double)S.pairs_refine,
           100.0 * (double)S.pairs_refine / (double)S.pairs_first, moved, top, upper);
  }
  const double kernel_s = S.kernel_s_first + S.kernel_s_later + S.kernel_s_refine;
  printf("time: unbinding %.3f s = kernel %.3f s (first pass %.3f, later %.3f) + host and "
         "copies %.3f s, in %d rounds\n",
         unbind_s, kernel_s, S.kernel_s_first, S.kernel_s_later + S.kernel_s_refine, S.host_s,
         S.passes);
  if (P.refine > 0)
    printf("time: of the later kernel time, centre refinement %.3f s\n", S.kernel_s_refine);
  printf("rate: %.4g pair terms per second in the first pass, %.4g overall\n",
         (double)S.pairs_first / S.kernel_s_first,
         ((double)S.pairs_first + later + (double)S.pairs_refine) / kernel_s);

  // By group size.
  const long long edges[5] = {0, 64, 1024, 16384, 1LL << 62};
  const char* names[4] = {"under 64", "64 to 1,023", "1,024 to 16,383", "16,384 and up"};
  std::unordered_map<long long, Row> ref;
  if (!ref_path.empty()) ref = readTable(ref_path);
  printf("\n%-18s %9s %12s %12s %8s %7s", "particles in group", "groups", "particles",
         "bound", "bound %", "passes");
  if (!ref_path.empty()) printf(" %12s %12s %10s", "bound (ref)", "difference", "same count");
  printf("\n");
  for (int b = 0; b < 4; b++) {
    long long groups = 0, parts = 0, bnd = 0, pss = 0, liv = 0, rbnd = 0, same = 0;
    for (int g = 0; g < ng; g++) {
      const long long size = G.off[g + 1] - G.off[g];
      if (size < edges[b] || size >= edges[b + 1]) continue;
      groups++;
      parts += size;
      bnd += R.nbound[g];
      if (R.nbound[g] > 0) {
        liv++;
        pss += R.passes[g];
      }
      if (!ref_path.empty()) {
        const auto it = ref.find(G.id[g]);
        const long long rb = it == ref.end() ? 0 : it->second.bound;
        rbnd += rb;
        same += rb == R.nbound[g];
      }
    }
    printf("%-18s %9lld %12lld %12lld %8.1f %7.2f", names[b], groups, parts, bnd,
           parts ? 100.0 * bnd / parts : 0.0, liv ? (double)pss / liv : 0.0);
    if (!ref_path.empty()) printf(" %12lld %+12lld %10lld", rbnd, bnd - rbnd, same);
    printf("\n");
  }

  // ---- compare -------------------------------------------------------------
  int rc = 0;
  if (!ref_path.empty()) {
    long long same = 0, both_alive = 0, only_here = 0, only_ref = 0, both_dead = 0;
    long long ref_bound = 0, size_mismatch = 0, pass_diff = 0, unmatched_ref = 0;
    long long hist[6] = {0, 0, 0, 0, 0, 0};   // |difference|: 0, 1, 2-5, 6-20, 21-100, more
    double worst_rel = 0;
    std::vector<int> order;
    std::unordered_map<long long, int> seen;
    for (int g = 0; g < ng; g++) {
      const long long size = G.off[g + 1] - G.off[g];
      const auto it = ref.find(G.id[g]);
      const long long rb = it == ref.end() ? 0 : it->second.bound;
      if (it != ref.end()) {
        seen[G.id[g]] = 1;
        size_mismatch += it->second.particles != size;
        if (rb > 0 && R.nbound[g] > 0) pass_diff += it->second.passes != R.passes[g];
      }
      ref_bound += rb;
      const long long d = std::llabs(R.nbound[g] - rb);
      same += d == 0;
      hist[d == 0 ? 0 : d == 1 ? 1 : d <= 5 ? 2 : d <= 20 ? 3 : d <= 100 ? 4 : 5]++;
      worst_rel = std::max(worst_rel, (double)d / (double)size);
      if (rb > 0 && R.nbound[g] > 0) both_alive++;
      else if (rb > 0) only_ref++;
      else if (R.nbound[g] > 0) only_here++;
      else both_dead++;
      if (d) order.push_back(g);
    }
    for (const auto& kv : ref) unmatched_ref += !seen.count(kv.first);
    printf("\ncompared with %s (%zu lines)\n", ref_path.c_str(), ref.size());
    printf("groups with the same bound count: %lld of %d (%.2f%%)\n", same, ng,
           100.0 * same / ng);
    printf("sub-halo in both: %lld, in neither: %lld, only here: %lld, only in the "
           "reference: %lld\n", both_alive, both_dead, only_here, only_ref);
    printf("bound particles: %lld here, %lld in the reference (%+lld, %+.4f%%)\n", bound,
           ref_bound, bound - ref_bound,
           ref_bound ? 100.0 * (bound - ref_bound) / (double)ref_bound : 0.0);
    printf("groups by |difference| in bound count: 0: %lld, 1: %lld, 2-5: %lld, 6-20: %lld, "
           "21-100: %lld, over 100: %lld; largest difference as a share of the group: "
           "%.3f%%\n", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5],
           100.0 * worst_rel);
    printf("sub-haloes in both with a different pass count: %lld\n", pass_diff);
    if (size_mismatch || unmatched_ref)
      printf("WARNING: %lld groups differ in size from the reference and %lld reference "
             "lines match no group: the table is for another snapshot or group set\n",
             size_mismatch, unmatched_ref);
    std::sort(order.begin(), order.end(), [&](int x, int y) {
      const auto bx = ref.count(G.id[x]) ? ref[G.id[x]].bound : 0;
      const auto by = ref.count(G.id[y]) ? ref[G.id[y]].bound : 0;
      return std::llabs(R.nbound[x] - bx) > std::llabs(R.nbound[y] - by);
    });
    if (!order.empty()) {
      printf("largest differences:\n%12s %12s %12s %12s %8s %8s\n", "group", "particles",
             "bound", "bound (ref)", "passes", "(ref)");
      for (size_t k = 0; k < order.size() && k < 10; k++) {
        const int g = order[k];
        const auto it = ref.find(G.id[g]);
        printf("%12lld %12lld %12lld %12lld %8d %8d\n", (long long)G.id[g],
               (long long)(G.off[g + 1] - G.off[g]), (long long)R.nbound[g],
               it == ref.end() ? 0LL : it->second.bound, R.passes[g],
               it == ref.end() ? 0 : it->second.passes);
      }
    }
    printf("UNBIND_COMPARE %s\n", same == ng ? "IDENTICAL" : "DIFFERENT");
    rc = same == ng ? 0 : 3;
  }

  // ---- write ---------------------------------------------------------------
  if (!out_path.empty()) {
    FILE* o = fopen(out_path.c_str(), "w");
    if (!o) die("cannot write " + out_path);
    fprintf(o, "# host_group_id particles_in_group bound_particles unbinding_passes "
               "unbinding_time_s\n");
    for (int g = 0; g < ng; g++)
      fprintf(o, "%lld %lld %lld %d 0\n", (long long)G.id[g],
              (long long)(G.off[g + 1] - G.off[g]), (long long)R.nbound[g], R.passes[g]);
    fclose(o);
    printf("\nwrote %s (%d lines)\n", out_path.c_str(), ng);
  }
  if (!dump_path.empty()) {
    const hsize_t m = (hsize_t)G.n();
    std::vector<long long> ids(m), grp(m);
    for (int g = 0; g < ng; g++)
      for (int64_t k = G.off[g]; k < G.off[g + 1]; k++) {
        ids[k] = pid[G.particle[k]];
        grp[k] = G.id[g];
      }
    hid_t f = H5Fcreate(dump_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (f < 0) die("cannot write " + dump_path);
    hid_t sp = H5Screate_simple(1, &m, NULL);
    auto put = [&](const char* name, hid_t type, const void* buf) {
      hid_t d = H5Dcreate2(f, name, type, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      if (d < 0 || H5Dwrite(d, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) < 0)
        die(std::string("cannot write ") + name);
      H5Dclose(d);
    };
    put("ParticleIDs", H5T_NATIVE_LLONG, ids.data());
    put("GroupIDs", H5T_NATIVE_LLONG, grp.data());
    put("Bound", H5T_NATIVE_UINT8, R.bound.data());
    put("Energy", H5T_NATIVE_DOUBLE, R.energy.data());
    if (P.refine > 0) {
      std::vector<uint8_t> most(m, 0);
      for (int g = 0; g < ng; g++)
        if (R.centre[g] >= 0) most[R.centre[g]] = 1;
      put("MostBound", H5T_NATIVE_UINT8, most.data());
      put("RefinedEnergy", H5T_NATIVE_DOUBLE, R.refined.data());
    }
    H5Sclose(sp);
    H5Fclose(f);
    printf("wrote %s (%llu particles)\n", dump_path.c_str(), (unsigned long long)m);
  }
  return rc;
}
