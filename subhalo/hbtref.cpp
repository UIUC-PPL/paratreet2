// hbtref -- turn one HBT-HERONS output into the flat reference files an
// unbinding kernel is compared against (design/subhalo-checks.md, "Exact
// reference for the unbinding kernel").
//
//   hbtref <catalogue dir> <out prefix> [snapshot.hdf5]
//
// <catalogue dir> holds SubSnap_NNN.<rank>.hdf5 from a run with
// SaveBoundParticleBindingEnergies 1. Writes
//
//   <out prefix>.particles.bin   one 24-byte record per bound particle:
//                                int64 particle id, int64 group id,
//                                float64 energy per unit mass in (km/s)^2.
//                                Native (little-endian) byte order, no header.
//   <out prefix>.groups.txt      one line per group with a sub-halo: group id,
//                                bound particles, unbinding passes, the bound
//                                particles' mean position (Mpc) and velocity
//                                (km/s).
//
// The group id is the snapshot's FOFGroupIDs value; the particle id is its
// ParticleIDs value. unbindcmp compares two .particles.bin files.
//
// With the snapshot tipsy2hbt wrote as a third argument it also writes
//
//   <out prefix>.fof3.particles.bin   the same records in FoF3's numbering:
//                                the particle id is the particle's order in
//                                the Tipsy file (ParticleIDs - 1) and the
//                                group id is the group's FoF3 label, the
//                                smallest order among the group's particles.
//
// That is the file to compare a kernel inside FoF3 against: it can write
// (order, label, energy) without knowing the snapshot's numbering.
//
// Build: make hbtref (subhalo/Makefile; HDF5).

#include <hdf5.h>

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <unordered_map>
#include <vector>

static std::vector<long long> read_ll(hid_t f, const char* name) {
  hid_t d = H5Dopen2(f, name, H5P_DEFAULT);
  if (d < 0) { fprintf(stderr, "hbtref: no dataset %s\n", name); exit(1); }
  hid_t sp = H5Dget_space(d);
  hsize_t n = 0;
  H5Sget_simple_extent_dims(sp, &n, NULL);
  std::vector<long long> v(n);
  if (H5Dread(d, H5T_NATIVE_LLONG, H5S_ALL, H5S_ALL, H5P_DEFAULT, v.data()) < 0) {
    fprintf(stderr, "hbtref: cannot read %s\n", name);
    exit(1);
  }
  H5Sclose(sp);
  H5Dclose(d);
  return v;
}

struct Sub {
  long long host, nbound;
  int iters;
  double x[3], v[3];
};
struct Rec {
  long long id, group;
  double e;
};

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: hbtref <catalogue dir> <out prefix> [snapshot.hdf5]\n");
    return 1;
  }
  const std::string dirname = argv[1], prefix = argv[2];
  hsize_t three = 3;
  hid_t vec3 = H5Tarray_create2(H5T_NATIVE_DOUBLE, 1, &three);
  hid_t mt = H5Tcreate(H5T_COMPOUND, sizeof(Sub));
  H5Tinsert(mt, "HostHaloId", HOFFSET(Sub, host), H5T_NATIVE_LLONG);
  H5Tinsert(mt, "Nbound", HOFFSET(Sub, nbound), H5T_NATIVE_LLONG);
  H5Tinsert(mt, "NumberUnbindingIterations", HOFFSET(Sub, iters), H5T_NATIVE_INT);
  H5Tinsert(mt, "ComovingAveragePosition", HOFFSET(Sub, x), vec3);
  H5Tinsert(mt, "PhysicalAverageVelocity", HOFFSET(Sub, v), vec3);
  hid_t vl_id = H5Tvlen_create(H5T_NATIVE_LLONG);
  hid_t vl_e = H5Tvlen_create(H5T_NATIVE_DOUBLE);

  FILE* fp = fopen((prefix + ".particles.bin").c_str(), "wb");
  FILE* fg = fopen((prefix + ".groups.txt").c_str(), "w");
  if (!fp || !fg) { fprintf(stderr, "cannot write %s.*\n", prefix.c_str()); return 1; }
  fprintf(fg, "# group_id bound_particles passes x y z [Mpc] vx vy vz [km/s]\n");

  DIR* dir = opendir(dirname.c_str());
  if (!dir) { fprintf(stderr, "cannot open %s\n", dirname.c_str()); return 1; }
  long long groups = 0, bound = 0;
  int files = 0;
  std::vector<Rec> all;                       // kept only for the FoF3 file
  while (dirent* e = readdir(dir)) {
    const std::string name = e->d_name;
    if (name.rfind("SubSnap_", 0) != 0) continue;
    hid_t f = H5Fopen((dirname + "/" + name).c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (f < 0) continue;
    hid_t ds = H5Dopen2(f, "/Subhalos", H5P_DEFAULT);
    hid_t dp = H5Dopen2(f, "/SubhaloParticles", H5P_DEFAULT);
    hid_t de = H5Dopen2(f, "/BindingEnergies", H5P_DEFAULT);
    if (ds < 0 || dp < 0 || de < 0) {
      fprintf(stderr, "%s lacks Subhalos, SubhaloParticles or BindingEnergies "
                      "(run with SaveBoundParticleBindingEnergies 1)\n", name.c_str());
      return 1;
    }
    hid_t sp = H5Dget_space(ds);
    hsize_t m = 0;
    H5Sget_simple_extent_dims(sp, &m, NULL);
    std::vector<Sub> subs(m);
    std::vector<hvl_t> ids(m), en(m);
    if (m) {
      if (H5Dread(ds, mt, H5S_ALL, H5S_ALL, H5P_DEFAULT, subs.data()) < 0 ||
          H5Dread(dp, vl_id, H5S_ALL, H5S_ALL, H5P_DEFAULT, ids.data()) < 0 ||
          H5Dread(de, vl_e, H5S_ALL, H5S_ALL, H5P_DEFAULT, en.data()) < 0) {
        fprintf(stderr, "cannot read %s\n", name.c_str());
        return 1;
      }
    }
    std::vector<Rec> out;
    for (hsize_t i = 0; i < m; i++) {
      const Sub& s = subs[i];
      if (s.nbound <= 0) continue;
      if ((long long)en[i].len != s.nbound || (long long)ids[i].len < s.nbound) {
        fprintf(stderr, "%s: sub-halo %llu has %lld bound particles but %zu "
                        "ids and %zu energies\n", name.c_str(),
                (unsigned long long)i, s.nbound, ids[i].len, en[i].len);
        return 1;
      }
      const long long* pid = (const long long*)ids[i].p;
      const double* pe = (const double*)en[i].p;
      for (long long k = 0; k < s.nbound; k++) out.push_back({pid[k], s.host, pe[k]});
      fprintf(fg, "%lld %lld %d %.9g %.9g %.9g %.9g %.9g %.9g\n", s.host,
              s.nbound, s.iters, s.x[0], s.x[1], s.x[2], s.v[0], s.v[1], s.v[2]);
      groups++;
      bound += s.nbound;
    }
    if (!out.empty() && fwrite(out.data(), sizeof(Rec), out.size(), fp) != out.size()) {
      fprintf(stderr, "short write\n");
      return 1;
    }
    if (argc > 3) all.insert(all.end(), out.begin(), out.end());
    if (m) {
      H5Treclaim(vl_id, sp, H5P_DEFAULT, ids.data());
      H5Treclaim(vl_e, sp, H5P_DEFAULT, en.data());
    }
    H5Sclose(sp); H5Dclose(ds); H5Dclose(dp); H5Dclose(de); H5Fclose(f);
    files++;
  }
  closedir(dir);
  fclose(fp);
  fclose(fg);
  printf("hbtref: %d files, %lld groups with a sub-halo, %lld bound particles\n",
         files, groups, bound);
  printf("hbtref: wrote %s.particles.bin (%lld bytes) and %s.groups.txt\n",
         prefix.c_str(), bound * (long long)sizeof(Rec), prefix.c_str());

  if (argc > 3) {
    // FoF3's numbering: order = ParticleIDs - 1, label = smallest order in
    // the group.
    hid_t f = H5Fopen(argv[3], H5F_ACC_RDONLY, H5P_DEFAULT);
    if (f < 0) { fprintf(stderr, "hbtref: cannot open %s\n", argv[3]); return 1; }
    const std::vector<long long> id = read_ll(f, "/PartType1/ParticleIDs");
    const std::vector<long long> fof = read_ll(f, "/PartType1/FOFGroupIDs");
    H5Fclose(f);
    std::unordered_map<long long, long long> label;   // group id -> label
    for (size_t i = 0; i < id.size(); i++) {
      auto it = label.find(fof[i]);
      if (it == label.end()) label[fof[i]] = id[i] - 1;
      else if (id[i] - 1 < it->second) it->second = id[i] - 1;
    }
    for (Rec& r : all) {
      auto it = label.find(r.group);
      if (it == label.end()) {
        fprintf(stderr, "hbtref: group %lld is not in %s\n", r.group, argv[3]);
        return 1;
      }
      r.id -= 1;
      r.group = it->second;
    }
    const std::string path = prefix + ".fof3.particles.bin";
    FILE* ff = fopen(path.c_str(), "wb");
    if (!ff || (!all.empty() && fwrite(all.data(), sizeof(Rec), all.size(), ff) != all.size())) {
      fprintf(stderr, "hbtref: cannot write %s\n", path.c_str());
      return 1;
    }
    fclose(ff);
    printf("hbtref: wrote %s (particle id = Tipsy order, group id = FoF3 label)\n",
           path.c_str());
  }
  return 0;
}
