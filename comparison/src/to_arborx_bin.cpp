// Write a snapshot's positions in ArborX's OWN benchmark binary format, so
// the stock ArborX_Benchmark_DistributedDBSCAN can read the real cosmology
// point set with none of my driver involved.
//
// Format (benchmarks/cluster/data_timpl.hpp): native-endian
//     int num_points; int dim; float xyz[num_points * dim]
// `num_points` is an int, which is exactly why this cannot be used for the
// 24B/58B snapshots -- but 100k fits, and 100k is where the answer is known
// (FoF3 and SWIFT both: 33933 components, max_size 26042).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "cosmoio.hpp"

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: to_arborx_bin <snapshot> <out.bin> [max_n]\n"); return 2; }
  cosmoio::Snapshot s; std::string e;
  if (!cosmoio::open_snapshot(argv[1], s, e)) { fprintf(stderr, "%s\n", e.c_str()); return 1; }
  long long n = s.n_total;
  if (argc > 3) { long long m = atoll(argv[3]); if (m > 0 && m < n) n = m; }
  if (n > 2147483000LL) {
    fprintf(stderr, "REFUSING: %lld points cannot be expressed in ArborX's "
                    "int-typed header\n", n);
    return 1;
  }
  std::vector<float> xyz((size_t)3 * n);
  if (!cosmoio::read_pos(s, 0, n, xyz.data(), e)) { fprintf(stderr, "%s\n", e.c_str()); return 1; }
  FILE* f = fopen(argv[2], "wb");
  if (!f) { perror("fopen"); return 1; }
  int np = (int)n, dim = 3;
  fwrite(&np, sizeof(int), 1, f);
  fwrite(&dim, sizeof(int), 1, f);
  fwrite(xyz.data(), sizeof(float), (size_t)3 * n, f);
  fclose(f);
  printf("wrote %s: %lld points, dim 3, %lld bytes\n", argv[2], n,
         8LL + 12LL * n);
  return 0;
}
