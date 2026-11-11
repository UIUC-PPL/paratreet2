// unbindrec.h -- the per-particle record of an unbinding result and its
// reader, shared by unbindcmp, unbindspec and unbindcheck. Plain C++, no
// libraries.
//
// A .particles.bin file holds one 24-byte record per BOUND particle, native
// byte order, no header: int64 particle id, int64 group id, float64 energy
// per unit mass in (km/s)^2.

#ifndef SUBHALO_UNBINDREC_H
#define SUBHALO_UNBINDREC_H

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

struct Rec {
  long long id, group;
  double e;
};

// Sort by particle id and refuse a particle that appears twice.
inline void sortRecords(std::vector<Rec>& v, const char* path, const char* tool) {
  std::sort(v.begin(), v.end(), [](const Rec& a, const Rec& b) { return a.id < b.id; });
  for (size_t i = 1; i < v.size(); i++)
    if (v[i].id == v[i - 1].id) {
      fprintf(stderr, "%s: particle %lld appears twice in %s\n", tool, v[i].id, path);
      exit(2);
    }
}

// Read a .particles.bin file, sorted by particle id.
inline std::vector<Rec> loadRecords(const char* path, const char* tool) {
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "%s: cannot open %s\n", tool, path); exit(2); }
  fseek(f, 0, SEEK_END);
  const long bytes = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (bytes % (long)sizeof(Rec)) {
    fprintf(stderr, "%s: %s is not a whole number of 24-byte records\n", tool, path);
    exit(2);
  }
  std::vector<Rec> v(bytes / sizeof(Rec));
  if (!v.empty() && fread(v.data(), sizeof(Rec), v.size(), f) != v.size()) {
    fprintf(stderr, "%s: short read of %s\n", tool, path);
    exit(2);
  }
  fclose(f);
  sortRecords(v, path, tool);
  return v;
}

// Median of |E| per group, and the number of bound particles per group.
inline std::unordered_map<long long, double> groupScales(
    const std::vector<Rec>& v, std::unordered_map<long long, long long>& count) {
  std::unordered_map<long long, std::vector<double>> by;
  for (const Rec& r : v) by[r.group].push_back(std::fabs(r.e));
  std::unordered_map<long long, double> s;
  for (auto& kv : by) {
    auto& a = kv.second;
    count[kv.first] = (long long)a.size();
    std::nth_element(a.begin(), a.begin() + a.size() / 2, a.end());
    s[kv.first] = a[a.size() / 2];
  }
  return s;
}

// The p-th quantile (0..1) of d; reorders d.
inline double percentile(std::vector<double>& d, double p) {
  if (d.empty()) return 0;
  size_t k = std::min(d.size() - 1, (size_t)(p * d.size()));
  std::nth_element(d.begin(), d.begin() + k, d.end());
  return d[k];
}

#endif
