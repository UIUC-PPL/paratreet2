// tipsy2hbt -- write a Tipsy snapshot plus FoF3's label files as one HDF5
// file in the SWIFT snapshot layout that HBT-HERONS reads
// (design/subhalo-checks.md, Check 2).
//
// Inputs
//   -i <file>     standard (big-endian, single precision) Tipsy snapshot
//   -l <prefix>   FoF3 label files <prefix>.<pe>.lbl (FoF3 -L), records of
//                 two native 8-byte integers: particle order, label. The
//                 label is the group's minimum particle order, or -1 for
//                 "no group" (FoF3 -K -m M writes -1 below M).
//   -o <file>     output, e.g. <dir>/snap_0000.hdf5
//   --box-mpc L   physical side of the periodic box in Mpc (no h)
//   --h h         dimensionless Hubble parameter (default 0.7)
//   --omega-m, --omega-l   (defaults 0.3, 0.7)
//   --period P    box side in code units (default 1; positions are taken to
//                 lie in [-P/2, P/2), the N-Body Shop convention)
//   --scale-factor a   write this expansion factor instead of the Tipsy
//                 header's time. For test inputs only: the particles are
//                 then not at a real epoch (design/subhalo-checks.md, "A
//                 scale factor other than 1")
//
// What is written. Every particle of every family goes into PartType1 (dark
// matter), as comparison/src/nc2swift.cpp does and for the same reason: FoF3
// links all particles as one species. Datasets: Coordinates (comoving, Mpc),
// Velocities (peculiar, km/s), Masses (1e10 Msun), ParticleIDs (order + 1),
// FOFGroupIDs. Group ids run 1..G by decreasing size (ties by label), the
// SWIFT convention; particles in no group get 2147483647, which is also
// written as Parameters:FOF:group_id_default. The groups Header, Cosmology,
// Units, PhysicalConstants/CGS and Parameters carry exactly the attributes
// HBT-HERONS's reader asks for (src/io/swiftsim_io/swiftsim_io.cpp).
//
// Units. N-Body Shop code units have G = 1, box side P and critical density
// 1 (dHubble0 = sqrt(8 pi / 3) = 2.894405). So with the box side L in Mpc:
//   length unit   = L / P                               [Mpc]
//   mass unit     = rho_crit * (L / P)^3                [Msun]
//   velocity unit = (L / P) * 100 h / 2.894405          [km/s]
// Tipsy velocities are multiplied by the expansion factor to give peculiar
// velocities (a = 1 for a present-day output, where it changes nothing).
// The scaling is exact for any L: bound or unbound does not depend on it.
//
// Serial, all in memory: for inputs of up to a few hundred million
// particles. A Frontier-size input needs the parallel layout of nc2swift.
//
// Build (Homebrew HDF5):
//   c++ -O2 -std=c++17 tipsy2hbt.cpp -o tipsy2hbt \
//       -I/opt/homebrew/include -L/opt/homebrew/lib -lhdf5

#include <hdf5.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

static void die(const std::string& m) {
  fprintf(stderr, "tipsy2hbt: %s\n", m.c_str());
  exit(1);
}
static float bef32(const unsigned char* p) {
  uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
static int32_t be32(const unsigned char* p) {
  return int32_t((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                 (uint32_t(p[2]) << 8) | uint32_t(p[3]));
}
static double bef64(const unsigned char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  double d;
  memcpy(&d, &v, 8);
  return d;
}

static void attr(hid_t g, const char* name, hid_t type, int n, const void* v) {
  hsize_t dim = n;
  hid_t sp = n == 1 ? H5Screate(H5S_SCALAR) : H5Screate_simple(1, &dim, NULL);
  hid_t a = H5Acreate2(g, name, type, sp, H5P_DEFAULT, H5P_DEFAULT);
  if (a < 0 || H5Awrite(a, type, v) < 0) die(std::string("attribute ") + name);
  H5Aclose(a);
  H5Sclose(sp);
}
static void attr_d(hid_t g, const char* name, double v) {
  attr(g, name, H5T_NATIVE_DOUBLE, 1, &v);
}
static void attr_s(hid_t g, const char* name, const std::string& v) {
  hid_t t = H5Tcopy(H5T_C_S1);
  H5Tset_size(t, v.size() + 1);
  H5Tset_strpad(t, H5T_STR_NULLTERM);
  attr(g, name, t, 1, v.c_str());
  H5Tclose(t);
}
static void dset(hid_t g, const char* name, hid_t type, int rank,
                 const hsize_t* dims, const void* buf, double a_exp,
                 bool has_a_exp) {
  hid_t sp = H5Screate_simple(rank, dims, NULL);
  hid_t d = H5Dcreate2(g, name, type, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (d < 0 || H5Dwrite(d, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) < 0)
    die(std::string("dataset ") + name);
  if (has_a_exp) {
    float e = (float)a_exp;
    attr(d, "a-scale exponent", H5T_NATIVE_FLOAT, 1, &e);
  }
  H5Dclose(d);
  H5Sclose(sp);
}

int main(int argc, char** argv) {
  std::string in, lbl, out;
  double box_mpc = 0, h = 0.7, om = 0.3, ol = 0.7, period = 1.0, a_given = 0;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) die("missing value for " + a);
      return argv[++i];
    };
    if (a == "-i") in = next();
    else if (a == "-l") lbl = next();
    else if (a == "-o") out = next();
    else if (a == "--box-mpc") box_mpc = atof(next());
    else if (a == "--h") h = atof(next());
    else if (a == "--omega-m") om = atof(next());
    else if (a == "--omega-l") ol = atof(next());
    else if (a == "--period") period = atof(next());
    else if (a == "--scale-factor") a_given = atof(next());
    else die("unknown option " + a);
  }
  if (in.empty() || lbl.empty() || out.empty() || box_mpc <= 0)
    die("usage: tipsy2hbt -i <tipsy> -l <label prefix> -o <out.hdf5> "
        "--box-mpc L [--h h] [--omega-m] [--omega-l] [--period P] [--scale-factor a]");
  if (a_given < 0) die("--scale-factor must be positive");

  // ---- Tipsy ---------------------------------------------------------------
  FILE* f = fopen(in.c_str(), "rb");
  if (!f) die("cannot open " + in);
  struct stat st;
  stat(in.c_str(), &st);
  unsigned char hd[32];
  if (fread(hd, 1, 28, f) != 28) die("short header");
  const double file_time = bef64(hd);
  const double time = a_given > 0 ? a_given : file_time;
  const long n = be32(hd + 8), ndim = be32(hd + 12);
  const long ngas = be32(hd + 16), ndark = be32(hd + 20), nstar = be32(hd + 24);
  if (ndim != 3 || n != ngas + ndark + nstar) die("not a standard Tipsy header");
  const long body = ngas * 48 + ndark * 36 + nstar * 44;
  long hdr = 0;
  if (st.st_size == 28 + body) hdr = 28;
  else if (st.st_size == 32 + body) hdr = 32;
  else die("file size does not match the header (truncated, or double "
           "precision positions)");
  fseek(f, hdr, SEEK_SET);

  std::vector<double> x(3 * (size_t)n);
  std::vector<float> v(3 * (size_t)n), m((size_t)n);
  double eps_code = 0, mass_code = 0;
  const double len = box_mpc / period;                   // Mpc per code length
  const double rho_crit = 2.77536627e11 * h * h;         // Msun / Mpc^3
  const double mass_unit = rho_crit * len * len * len / 1e10;   // 1e10 Msun
  const double vel_unit = len * 100.0 * h / 2.894405 * time;    // km/s
  const long fam_n[3] = {ngas, ndark, nstar};
  const int fam_rec[3] = {48, 36, 44};
  const int fam_eps[3] = {-1, 28, 36};                   // byte offset of eps
  std::vector<unsigned char> buf;
  long at = 0;
  for (int fam = 0; fam < 3; fam++) {
    const int rec = fam_rec[fam];
    const long chunk = 1 << 20;
    for (long done = 0; done < fam_n[fam]; done += chunk) {
      const long c = std::min(chunk, fam_n[fam] - done);
      buf.resize((size_t)c * rec);
      if (fread(buf.data(), rec, c, f) != (size_t)c) die("short read");
      for (long i = 0; i < c; i++, at++) {
        const unsigned char* p = buf.data() + (size_t)i * rec;
        m[at] = (float)(bef32(p) * mass_unit);
        mass_code += bef32(p);
        for (int k = 0; k < 3; k++) {
          double xc = bef32(p + 4 + 4 * k) + 0.5 * period;
          xc -= period * std::floor(xc / period);        // into [0, P)
          double xp = xc * len;
          if (xp >= box_mpc) xp = 0;                     // rounding at the edge
          x[3 * at + k] = xp;
          v[3 * at + k] = (float)(bef32(p + 16 + 4 * k) * vel_unit);
        }
        if (eps_code == 0 && fam_eps[fam] >= 0) eps_code = bef32(p + fam_eps[fam]);
      }
    }
  }
  fclose(f);
  if (eps_code <= 0) eps_code = 0.02 * period / std::cbrt((double)n);
  const double soft = eps_code * len;

  // ---- labels --------------------------------------------------------------
  std::vector<long> label((size_t)n, LONG_MIN);
  long files = 0, recs = 0;
  for (;; files++) {
    const std::string p = lbl + "." + std::to_string(files) + ".lbl";
    FILE* g = fopen(p.c_str(), "rb");
    if (!g) break;
    long r[2 * 4096];
    size_t got;
    while ((got = fread(r, 16, 4096, g)) > 0)
      for (size_t i = 0; i < got; i++, recs++) {
        const long o = r[2 * i];
        if (o < 0 || o >= n) die("label file has an order outside the input");
        if (label[o] != LONG_MIN) die("an order appears twice in the label files");
        label[o] = r[2 * i + 1];
      }
    fclose(g);
  }
  if (files == 0) die("no label files at " + lbl + ".0.lbl");
  if (recs != n) die("label files hold " + std::to_string(recs) +
                     " records, the input has " + std::to_string(n));

  // Group ids 1..G by decreasing size, ties by label.
  std::unordered_map<long, long> size;
  long none = 0;
  for (long i = 0; i < n; i++) {
    if (label[i] < 0) none++;
    else size[label[i]]++;
  }
  std::vector<std::pair<long, long>> groups(size.begin(), size.end());
  std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  const long null_id = 2147483647L;
  if ((long)groups.size() >= null_id) die("too many groups for the null id");
  std::unordered_map<long, long> gid;
  gid.reserve(groups.size());
  for (size_t i = 0; i < groups.size(); i++) gid[groups[i].first] = (long)i + 1;
  std::vector<long long> fof((size_t)n), ids((size_t)n);
  for (long i = 0; i < n; i++) {
    fof[i] = label[i] < 0 ? null_id : gid[label[i]];
    ids[i] = i + 1;
  }

  // ---- HDF5 ----------------------------------------------------------------
  hid_t file = H5Fcreate(out.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file < 0) die("cannot create " + out);
  auto group = [&](const char* name) {
    hid_t g = H5Gcreate2(file, name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (g < 0) die(std::string("group ") + name);
    return g;
  };
  {
    hid_t g = group("Header");
    int ntypes = 2, nfiles = 1;
    double box[3] = {box_mpc, box_mpc, box_mpc};
    long long this_file[2] = {0, (long long)n};
    unsigned total[2] = {0, (unsigned)(n & 0xffffffffL)};
    unsigned high[2] = {0, (unsigned)((unsigned long)n >> 32)};
    attr(g, "NumPartTypes", H5T_NATIVE_INT, 1, &ntypes);
    attr(g, "NumFilesPerSnapshot", H5T_NATIVE_INT, 1, &nfiles);
    attr(g, "BoxSize", H5T_NATIVE_DOUBLE, 3, box);
    attr(g, "NumPart_ThisFile", H5T_NATIVE_LLONG, 2, this_file);
    attr(g, "NumPart_Total", H5T_NATIVE_UINT, 2, total);
    attr(g, "NumPart_Total_HighWord", H5T_NATIVE_UINT, 2, high);
    H5Gclose(g);
  }
  {
    hid_t g = group("Cosmology");
    const double a = time;
    const double ez = std::sqrt(om / (a * a * a) + ol + (1 - om - ol) / (a * a));
    attr_d(g, "Scale-factor", a);
    attr_d(g, "Omega_m", om);
    attr_d(g, "Omega_lambda", ol);
    attr_d(g, "h", h);
    attr_d(g, "H [internal units]", 100.0 * h * ez);     // km/s/Mpc = 1/U_t
    H5Gclose(g);
  }
  const double pc = 3.08567758149e18, msun = 1.98841e33;
  {
    hid_t g = group("Units");
    attr_d(g, "Unit length in cgs (U_L)", 1e6 * pc);            // Mpc
    attr_d(g, "Unit mass in cgs (U_M)", 1e10 * msun);           // 1e10 Msun
    attr_d(g, "Unit time in cgs (U_t)", 1e6 * pc / 1e5);        // Mpc/(km/s)
    H5Gclose(g);
    hid_t pcg = group("PhysicalConstants");
    hid_t cgs = H5Gcreate2(pcg, "CGS", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    attr_d(cgs, "parsec", pc);
    attr_d(cgs, "solar_mass", msun);
    H5Gclose(cgs);
    H5Gclose(pcg);
  }
  {
    hid_t g = group("Parameters");
    char s[64];
    attr_s(g, "FOF:group_id_default", std::to_string(null_id));
    snprintf(s, sizeof s, "%.9g", soft);
    attr_s(g, "Gravity:comoving_DM_softening", s);
    attr_s(g, "Gravity:max_physical_DM_softening", s);
    H5Gclose(g);
  }
  {
    hid_t g = group("PartType1");
    hsize_t d2[2] = {(hsize_t)n, 3}, d1[1] = {(hsize_t)n};
    dset(g, "Coordinates", H5T_NATIVE_DOUBLE, 2, d2, x.data(), 1, true);
    dset(g, "Velocities", H5T_NATIVE_FLOAT, 2, d2, v.data(), 0, true);
    dset(g, "Masses", H5T_NATIVE_FLOAT, 1, d1, m.data(), 0, true);
    dset(g, "ParticleIDs", H5T_NATIVE_LLONG, 1, d1, ids.data(), 0, false);
    dset(g, "FOFGroupIDs", H5T_NATIVE_LLONG, 1, d1, fof.data(), 0, false);
    H5Gclose(g);
  }
  H5Fclose(file);

  printf("tipsy2hbt: %ld particles (gas %ld dark %ld star %ld), a = %g\n", n,
         ngas, ndark, nstar, time);
  if (a_given > 0)
    printf("tipsy2hbt: scale factor %g given on the command line; the file's time "
           "is %g\n", a_given, file_time);
  printf("tipsy2hbt: %ld label files; %zu groups holding %ld particles, "
         "largest %ld; %ld particles in no group\n", files, groups.size(),
         n - none, groups.empty() ? 0L : groups[0].second, none);
  printf("tipsy2hbt: box %g Mpc, h %g; units: %g Mpc, %.6g x 1e10 Msun, "
         "%.6g km/s per code unit\n", box_mpc, h, len, mass_unit, vel_unit);
  printf("tipsy2hbt: total mass %.6g code = %.6g x 1e10 Msun "
         "(Omega_m * rho_crit * V = %.6g); softening %.4g Mpc\n", mass_code,
         mass_code * mass_unit, om * rho_crit * box_mpc * box_mpc * box_mpc / 1e10,
         soft);
  printf("tipsy2hbt: wrote %s\n", out.c_str());
  return 0;
}
