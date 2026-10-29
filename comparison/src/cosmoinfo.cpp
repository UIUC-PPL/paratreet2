// cosmoinfo -- validate cosmoio.hpp against a snapshot: print the header it
// decoded, the first few positions, and the bounding box over a strided
// sample (or, with --full, every particle). The bbox is the number to check
// against FoF3's "FoF linking length b = ... V = ... N = ..." line.
#include "cosmoio.hpp"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: cosmoinfo <snapshot> [--full] [--stride N]\n"); return 2; }
  std::string path = argv[1];
  bool full = false;
  int64_t stride = 1000;
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--full")) full = true;
    else if (!strcmp(argv[i], "--stride") && i + 1 < argc) stride = atoll(argv[++i]);
  }

  cosmoio::Snapshot s;
  std::string err;
  if (!cosmoio::open_snapshot(path, s, err)) { fprintf(stderr, "ERROR: %s\n", err.c_str()); return 1; }

  printf("path      : %s\n", s.path.c_str());
  printf("format    : %s\n", s.fmt == cosmoio::Format::Tipsy ? "tipsy" : "nchilada");
  printf("time      : %.17g\n", s.time);
  printf("n_gas     : %lld\n", (long long)s.n_gas);
  printf("n_dark    : %lld\n", (long long)s.n_dark);
  printf("n_star    : %lld\n", (long long)s.n_star);
  printf("n_total   : %lld\n", (long long)s.n_total);
  if (s.fmt == cosmoio::Format::Tipsy)
    printf("recs      : gas %d dark %d star %d (pos word %d)\n", s.gas_rec, s.dark_rec, s.star_rec, s.pos_word);
  else
    for (int i = 0; i < 3; ++i)
      if (s.pos[i].present)
        printf("nc fam %d  : n=%lld data_off=%lld code=%d word=%d mass=%s\n", i,
               (long long)s.pos[i].n, (long long)s.pos[i].data_off, s.pos[i].code,
               s.pos[i].word, s.mass[i].present ? "yes" : "no");

  // First 4 positions, and the first 4 of the dark family, so a family-offset
  // error shows up as an obviously wrong second block rather than silently.
  std::vector<float> p(12);
  if (cosmoio::read_pos(s, 0, 4, p.data(), err))
    for (int i = 0; i < 4; ++i)
      printf("pos[%d]    : %.9g %.9g %.9g\n", i, p[3*i], p[3*i+1], p[3*i+2]);
  else printf("pos[0..3] : ERROR %s\n", err.c_str());
  if (s.n_dark > 0 && cosmoio::read_pos(s, s.n_gas, 2, p.data(), err))
    for (int i = 0; i < 2; ++i)
      printf("dark[%d]   : %.9g %.9g %.9g\n", i, p[3*i], p[3*i+1], p[3*i+2]);

  // Bounding box + mass sum over a sample (or everything with --full).
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  double msum = 0; int64_t seen = 0; bool have_mass = true;
  const int64_t CH = 1 << 20;
  std::vector<float> buf(3 * CH), mbuf(CH);
  if (full) {
    for (int64_t off = 0; off < s.n_total; off += CH) {
      int64_t m = std::min(CH, s.n_total - off);
      if (!cosmoio::read_pos(s, off, m, buf.data(), err)) { fprintf(stderr, "ERROR: %s\n", err.c_str()); return 1; }
      for (int64_t i = 0; i < m; ++i)
        for (int d = 0; d < 3; ++d) {
          double v = buf[3*i+d];
          if (v < lo[d]) lo[d] = v;
          if (v > hi[d]) hi[d] = v;
        }
      if (have_mass) {
        if (cosmoio::read_mass(s, off, m, mbuf.data(), err))
          for (int64_t i = 0; i < m; ++i) msum += mbuf[i];
        else have_mass = false;
      }
      seen += m;
    }
  } else {
    for (int64_t off = 0; off < s.n_total; off += stride) {
      float q[3];
      if (!cosmoio::read_pos(s, off, 1, q, err)) { fprintf(stderr, "ERROR: %s\n", err.c_str()); return 1; }
      for (int d = 0; d < 3; ++d) {
        if (q[d] < lo[d]) lo[d] = q[d];
        if (q[d] > hi[d]) hi[d] = q[d];
      }
      ++seen;
    }
  }
  printf("sampled   : %lld particles (%s)\n", (long long)seen, full ? "full" : "strided");
  printf("bbox lo   : %.9g %.9g %.9g\n", lo[0], lo[1], lo[2]);
  printf("bbox hi   : %.9g %.9g %.9g\n", hi[0], hi[1], hi[2]);
  double V = (hi[0]-lo[0])*(hi[1]-lo[1])*(hi[2]-lo[2]);
  printf("bbox vol  : %.9g\n", V);
  printf("b(0.2)    : %.9g   [0.2 * cbrt(V/N), N = %lld]\n",
         0.2 * cbrt(V / (double)s.n_total), (long long)s.n_total);
  if (full && have_mass) printf("mass sum  : %.9g\n", msum);
  return 0;
}
