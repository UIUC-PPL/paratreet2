// Do reads that STRADDLE a family boundary agree with reads taken wholly
// inside each family? The 2B Tipsy set has 452,984,832 gas particles at 48 B
// per record followed by 1,528,823,808 dark at 36 B, so a slab crossing that
// boundary changes both the record stride and the base offset mid-read. The
// 24B/58B NChilada sets change FILE at the boundary. Either is a place an
// off-by-one silently corrupts a fraction of the particles -- and a
// conversion is not re-runnable cheaply at 2 TB, so it is checked first.
#include <cmath>
#include <cstdio>
#include "cosmoio.hpp"

static int check(const char* path) {
  cosmoio::Snapshot s; std::string e;
  if (!cosmoio::open_snapshot(path, s, e)) { printf("  OPEN FAIL %s\n", e.c_str()); return 1; }
  printf("  %s: gas %lld dark %lld star %lld\n", s.fmt==cosmoio::Format::Tipsy?"tipsy":"nchilada",
         (long long)s.n_gas, (long long)s.n_dark, (long long)s.n_star);
  int bad = 0;
  int64_t B = s.n_gas;                    // the gas -> dark boundary
  if (B == 0 || s.n_dark == 0) { printf("  (single family; boundary test n/a)\n"); }
  else {
    for (int64_t w : {(int64_t)1, (int64_t)3, (int64_t)17, (int64_t)1000, (int64_t)(1<<20)+7}) {
      int64_t st = B - w, n = 2 * w;
      if (st < 0) continue;
      std::vector<float> span(3*n), a(3*w), b(3*w);
      std::vector<float> ms(n), ma(w), mb(w);
      if (!cosmoio::read_pos(s, st, n, span.data(), e)) { printf("  span pos FAIL %s\n", e.c_str()); return 1; }
      if (!cosmoio::read_pos(s, st, w, a.data(), e))    { printf("  a pos FAIL\n"); return 1; }
      if (!cosmoio::read_pos(s, B,  w, b.data(), e))    { printf("  b pos FAIL\n"); return 1; }
      int64_t d = 0;
      for (int64_t i = 0; i < 3*w; ++i) { if (span[i] != a[i]) ++d; if (span[3*w+i] != b[i]) ++d; }
      bool mok = true;
      if (cosmoio::read_mass(s, st, n, ms.data(), e) &&
          cosmoio::read_mass(s, st, w, ma.data(), e) &&
          cosmoio::read_mass(s, B,  w, mb.data(), e)) {
        for (int64_t i = 0; i < w; ++i) { if (ms[i]!=ma[i]) mok=false; if (ms[w+i]!=mb[i]) mok=false; }
      }
      printf("  boundary +/-%-8lld pos_mismatch=%-6lld mass_consistent=%s  gasmass=%.9g darkmass=%.9g\n",
             (long long)w, (long long)d, mok?"yes":"NO", ma[0], mb[0]);
      if (d || !mok) ++bad;
    }
  }
  // A read of the very last particles must not run off the end.
  std::vector<float> tail(3*5);
  if (!cosmoio::read_pos(s, s.n_total-5, 5, tail.data(), e)) { printf("  TAIL FAIL %s\n", e.c_str()); ++bad; }
  else printf("  last particle: %.9g %.9g %.9g\n", tail[12], tail[13], tail[14]);
  // And one past the end must be REFUSED, not silently truncated.
  std::vector<float> over(3*2);
  if (cosmoio::read_pos(s, s.n_total-1, 2, over.data(), e)) { printf("  OVERRUN NOT REFUSED\n"); ++bad; }
  else printf("  overrun correctly refused\n");
  return bad;
}

int main() {
  int bad = 0;
  const char* sets[] = {
    "/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024",
    "/lustre/orion/csc710/proj-shared/cosmo25PLK.2304g_dm.004096",
    "/lustre/orion/csc710/proj-shared/romulus25.3072g.ad.000352"};
  for (auto p : sets) { printf("%s\n", p); bad += check(p); }
  printf("\n%s\n", bad ? "*** BOUNDARY CHECK FAILED ***" : "BOUNDARY CHECK PASSED (all three snapshots)");
  return bad ? 1 : 0;
}
