// hbtstat -- summarize one output of HBT-HERONS against the FoF groups it
// was given (design/subhalo-checks.md, Check 2).
//
//   hbtstat <snapshot.hdf5> <catalogue dir> [table.txt]
//
// <snapshot.hdf5> is the file tipsy2hbt wrote (its PartType1/FOFGroupIDs
// give every group's size). <catalogue dir> holds SubSnap_NNN.<rank>.hdf5.
// Needs HBT-HERONS built with HBT_MEASURE_UNBINDING_TIME for the iteration
// counts and times; without it those columns read as zero.
//
// Prints totals and the largest groups, and with a third argument writes
// one line per sub-halo: host group id, particles in the group, bound
// particles, unbinding passes, unbinding time in seconds. That table is the
// reference a later unbinding kernel is compared against.
//
// Build: c++ -O2 -std=c++17 hbtstat.cpp -o hbtstat \
//            -I/opt/homebrew/include -L/opt/homebrew/lib -lhdf5

#include <hdf5.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <unordered_map>
#include <vector>

struct Sub {
  long long host, nbound, rank;
  int iters, mpi_rank;
  float t0, t1;
};

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: hbtstat <snapshot.hdf5> <catalogue dir> [table.txt]\n");
    return 1;
  }
  // Group sizes from the snapshot.
  hid_t f = H5Fopen(argv[1], H5F_ACC_RDONLY, H5P_DEFAULT);
  if (f < 0) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
  hid_t d = H5Dopen2(f, "/PartType1/FOFGroupIDs", H5P_DEFAULT);
  hid_t sp = H5Dget_space(d);
  hsize_t n;
  H5Sget_simple_extent_dims(sp, &n, NULL);
  std::vector<long long> fof(n);
  H5Dread(d, H5T_NATIVE_LLONG, H5S_ALL, H5S_ALL, H5P_DEFAULT, fof.data());
  H5Sclose(sp); H5Dclose(d); H5Fclose(f);
  const long long null_id = 2147483647LL;
  std::unordered_map<long long, long long> gsize;
  long long grouped = 0;
  for (long long g : fof)
    if (g != null_id) { gsize[g]++; grouped++; }

  // Sub-haloes from every catalogue file in the directory.
  hid_t mt = H5Tcreate(H5T_COMPOUND, sizeof(Sub));
  H5Tinsert(mt, "HostHaloId", HOFFSET(Sub, host), H5T_NATIVE_LLONG);
  H5Tinsert(mt, "Nbound", HOFFSET(Sub, nbound), H5T_NATIVE_LLONG);
  H5Tinsert(mt, "Rank", HOFFSET(Sub, rank), H5T_NATIVE_LLONG);
  H5Tinsert(mt, "NumberUnbindingIterations", HOFFSET(Sub, iters), H5T_NATIVE_INT);
  H5Tinsert(mt, "MPIRank", HOFFSET(Sub, mpi_rank), H5T_NATIVE_INT);
  H5Tinsert(mt, "StartUnbinding", HOFFSET(Sub, t0), H5T_NATIVE_FLOAT);
  H5Tinsert(mt, "EndUnbinding", HOFFSET(Sub, t1), H5T_NATIVE_FLOAT);
  std::vector<Sub> subs;
  DIR* dir = opendir(argv[2]);
  if (!dir) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
  int files = 0;
  H5Eset_auto2(H5E_DEFAULT, NULL, NULL);   // absent timing fields are not errors
  while (dirent* e = readdir(dir)) {
    std::string name = e->d_name;
    if (name.rfind("SubSnap_", 0) != 0) continue;
    hid_t cf = H5Fopen((std::string(argv[2]) + "/" + name).c_str(),
                       H5F_ACC_RDONLY, H5P_DEFAULT);
    if (cf < 0) continue;
    hid_t cd = H5Dopen2(cf, "/Subhalos", H5P_DEFAULT);
    hid_t cs = H5Dget_space(cd);
    hsize_t m = 0;
    H5Sget_simple_extent_dims(cs, &m, NULL);
    std::vector<Sub> part(m, Sub{0, 0, 0, 0, 0, 0, 0});
    if (m && H5Dread(cd, mt, H5S_ALL, H5S_ALL, H5P_DEFAULT, part.data()) < 0) {
      fprintf(stderr, "cannot read /Subhalos in %s\n", name.c_str());
      return 1;
    }
    subs.insert(subs.end(), part.begin(), part.end());
    H5Sclose(cs); H5Dclose(cd); H5Fclose(cf);
    files++;
  }
  closedir(dir);

  long long bound = 0, alive = 0, iters = 0;
  int max_iters = 0;
  double t_sum = 0;
  for (const Sub& s : subs) {
    if (s.nbound <= 0) continue;
    alive++; bound += s.nbound; iters += s.iters;
    max_iters = std::max(max_iters, s.iters);
    t_sum += (s.t1 - s.t0) * 1e-9;   // the catalogue stores nanoseconds
  }
  printf("groups given: %zu holding %lld particles\n", gsize.size(), grouped);
  printf("sub-haloes with bound particles: %lld (from %d files); "
         "groups with none: %lld\n", alive, files,
         (long long)gsize.size() - alive);
  printf("bound particles: %lld = %.1f%% of the particles in groups\n", bound,
         100.0 * bound / (double)grouped);
  printf("unbinding passes per sub-halo: mean %.2f, max %d\n",
         alive ? (double)iters / alive : 0.0, max_iters);
  printf("unbinding time summed over sub-haloes: %.3f s\n", t_sum);

  std::sort(subs.begin(), subs.end(), [&](const Sub& a, const Sub& b) {
    return gsize[a.host] > gsize[b.host];
  });
  printf("\nlargest groups:\n%12s %12s %8s %7s %10s\n", "in group", "bound",
         "bound %", "passes", "time (s)");
  for (size_t i = 0; i < subs.size() && i < 8; i++) {
    const Sub& s = subs[i];
    const long long g = gsize[s.host];
    printf("%12lld %12lld %8.1f %7d %10.4f\n", g, s.nbound,
           100.0 * s.nbound / (double)g, s.iters, (s.t1 - s.t0) * 1e-9);
  }
  if (argc > 3) {
    FILE* o = fopen(argv[3], "w");
    fprintf(o, "# host_group_id particles_in_group bound_particles "
               "unbinding_passes unbinding_time_s\n");
    for (const Sub& s : subs)
      fprintf(o, "%lld %lld %lld %d %.6f\n", s.host, gsize[s.host], s.nbound,
              s.iters, (s.t1 - s.t0) * 1e-9);
    fclose(o);
    printf("\nwrote %s (%zu lines)\n", argv[3], subs.size());
  }
  return 0;
}
