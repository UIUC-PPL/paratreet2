/****************************************************************************
 * cosmo_fof_hacc -- friends-of-friends over a Tipsy/NChilada cosmology
 * snapshot using ArborX's SINGLE-RANK DBSCAN kernel plus our own distributed
 * stitching, i.e. the way ArborX is actually used at scale.
 *
 * WHY NOT ArborX::Experimental::dbscan (the distributed wrapper)?
 *
 *  1. CORRECTNESS. On this data it does not return the right answer. With a
 *     file-order distribution it OVER-merges (28589 components / max_size
 *     70651 against a truth of 33933 / 26042 on the 100k check set) and with
 *     a Morton distribution it UNDER-merges (95172 / 27). Its own
 *     Details::verifyDBSCAN cannot see the over-merge case: the distributed
 *     overload explicitly skips verifyClustersAreUnique with the comment
 *     "no idea how to do it in distributed setting right now", so it checks
 *     only that connected points SHARE a label, never that distinct clusters
 *     do not. A separate, real defect in its merge-pair reconciliation was
 *     found and fixed as part of this work (see ARBORX_BUG_REPORT.md), which
 *     took the stock benchmark from 11/12 failing to 15/15 passing at
 *     16/32/64 ranks -- but "passing" there is only the under-merge
 *     direction, and the counts still disagree with truth.
 *
 *  2. SCALE. `communicateMergePairs` MPI_Allgathervs the ENTIRE global
 *     merge-pair set to EVERY rank and then mirrors it on the host, with
 *     `std::vector<int>` offsets scaled by sizeof(MergePair). So
 *     total_pairs * 16 must fit in a signed int -- a ceiling near 134e6
 *     pairs globally, whatever the machine. The 24B snapshot has 6.73e9
 *     components and the 58B one 2.92e10; the distributed wrapper cannot
 *     express their reconciliation at all.
 *
 *  3. REPRESENTATIVENESS. Codes that run ArborX at trillion-particle scale
 *     do not use that path. They keep their own domain decomposition with
 *     overload/ghost zones, call the LOCAL kernel per rank, and stitch
 *     across ranks themselves. Measuring the distributed wrapper would
 *     measure a path nobody uses at scale.
 *
 * WHAT THIS DOES, and it is deliberately HACC-shaped:
 *
 *   1. read a file-order slab of the snapshot (the same bytes FoF3 reads);
 *   2. redistribute onto a BALANCED RECTILINEAR 3-D brick grid of ranks: px
 *      slabs in x, py slabs in y inside each x-slab, pz bricks in z inside
 *      each column, with every cut placed at an equal particle COUNT rather
 *      than an equal width. Not a space-filling curve: keeping every domain
 *      an axis-aligned box keeps the halo test to a few binary searches per
 *      particle instead of a tree over rank bounding boxes, which is the
 *      property that makes the overload-zone style of decomposition cheap.
 *      A UNIFORM grid was tried first and does not survive real cosmology
 *      data: on the 24B snapshot at 128 ranks one brick held 2.24e9
 *      particles against a mean of 187e6 -- imbalance 46.7, and past
 *      ArborX's int32 local indexing, so that dataset could not be run at
 *      all -- while on the 58B snapshot the imbalance reached 5.0 and the
 *      most-loaded brick set the runtime for every rank;
 *   3. exchange an eps-thick overload/halo shell with every brick within
 *      reach (with balanced cuts the bricks do not line up face to face, so
 *      this is a range search over the cut arrays, not a fixed 26-neighbour
 *      stencil);
 *   4. run ArborX's single-rank dbscan (minPts = 2, which ArborX
 *      special-cases to connected components) over own + halo points;
 *   5. stitch: label every local component with the minimum global particle
 *      id it contains, send halo labels home, take minima, and iterate to a
 *      global fixed point. This is label propagation over the graph whose
 *      hyperedges are the per-rank components; it terminates in at most the
 *      rank-adjacency diameter, ~O(P^(1/3)) rounds.
 *
 * Timing is reported in four separate pieces -- read, decompose+halo, local
 * DBSCAN, stitch -- so the "FoF solve" (4+5) can be compared against FoF3's
 * "Iteration 0" and SWIFT's "Complete FOF search", with the decomposition
 * accounted separately as in both of those.
 ****************************************************************************/

#include <ArborX_DBSCAN.hpp>
#include <ArborX_Point.hpp>
#include <ArborX_Version.hpp>

#include <Kokkos_Core.hpp>

#include <mpi.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

#include "cosmoio.hpp"

using Point = ArborX::Point<3, float>;
static_assert(sizeof(Point) == 3 * sizeof(float),
              "ArborX::Point<3,float> must be 3 tightly packed floats for the "
              "unmanaged host staging view below to be valid");

namespace {

int g_rank = 0, g_nranks = 1;
double now() { return MPI_Wtime(); }

void die(const char *what, const std::string &why) {
  fprintf(stderr, "[%d] FATAL %s: %s\n", g_rank, what, why.c_str());
  fflush(stderr);
  MPI_Abort(MPI_COMM_WORLD, 1);
}

/// Factor P into the most cube-like (px, py, pz) with px*py*pz == P, so the
/// bricks are as compact as possible and the halo (which scales with brick
/// surface area) is as small as possible.
void factor3(int P, int &px, int &py, int &pz) {
  px = py = pz = 1;
  int best = -1;
  for (int a = 1; a <= P; ++a) {
    if (P % a) continue;
    int rem = P / a;
    for (int b = 1; b <= rem; ++b) {
      if (rem % b) continue;
      int c = rem / b;
      // Prefer the triple whose largest side is smallest, i.e. most cubic.
      int worst = std::max(a, std::max(b, c));
      if (best < 0 || worst < best) { best = worst; px = a; py = b; pz = c; }
    }
  }
}

/// Largest s in [0, k-1] with c[s] <= v, for a monotone edge array c[0..k].
/// Used for BOTH the ownership test and the halo range test, so a point can
/// never be assigned to one brick and tested against another's edges.
inline int slot(const double *c, int k, double v) {
  int s = (int)(std::upper_bound(c, c + k + 1, v) - c) - 1;
  if (s < 0) s = 0;
  if (s >= k) s = k - 1;
  return s;
}

/// A balanced RECTILINEAR ("multi-jagged") partition: px slabs in x, then py
/// slabs in y INSIDE each x-slab, then pz bricks in z inside each column.
/// Every brick is still an axis-aligned box, so the halo test stays a few
/// binary searches per particle, but the cuts are placed at equal particle
/// COUNTS instead of equal widths.
///
/// The uniform grid this replaces was unusable on clustered cosmology data:
/// on the 24B snapshot at 128 ranks one brick held 2.24e9 particles against
/// a mean of 187e6 (imbalance 46.7), past ArborX's int32 local indexing, so
/// the 24B row of the comparison could not be produced at all; on the 58B
/// snapshot the imbalance reached 5.0 and the most-loaded brick set the
/// runtime for every rank.
struct Grid {
  int px = 1, py = 1, pz = 1;
  std::vector<double> xc;  // px+1
  std::vector<double> yc;  // px*(py+1)
  std::vector<double> zc;  // px*py*(pz+1)
  const double *ycut(int i) const { return &yc[(size_t)i * (py + 1)]; }
  const double *zcut(int i, int j) const {
    return &zc[((size_t)i * py + j) * (pz + 1)];
  }
  int rank_of(int i, int j, int k) const { return (i * py + j) * pz + k; }
  void locate(const float *p, int &i, int &j, int &k) const {
    i = slot(xc.data(), px, p[0]);
    j = slot(ycut(i), py, p[1]);
    k = slot(zcut(i, j), pz, p[2]);
  }
  int brick_of(const float *p) const {
    int i, j, k;
    locate(p, i, j, k);
    return rank_of(i, j, k);
  }
};

/// Split each of `ngroup` groups into `k` parts of near-equal particle count
/// along one axis. `grp[s]` / `val[s]` describe a strided SAMPLE of the local
/// particles; the quantiles come from a two-level global histogram, so this
/// costs two small MPI_Allreduces and no sorting, gathering or sending of
/// particles. `cuts` comes back as ngroup*(k+1) monotone edges whose first
/// and last entries are `lo` and `hi_open`.
///
/// `min_width` stops a group being cut into slabs thinner than a few linking
/// lengths. Balance is the point of this function, but a slab much thinner
/// than eps is completely inside its neighbours' halos, and the halo rather
/// than the imbalance then dominates the cost.
void equal_count_cuts(MPI_Comm comm, int ngroup, int k, double lo,
                      double hi_open, double min_width,
                      const std::vector<int> &grp,
                      const std::vector<float> &val,
                      std::vector<double> &cuts) {
  const size_t ns = grp.size();
  cuts.assign((size_t)ngroup * (k + 1), 0.0);
  const double span = hi_open - lo;
  for (int g = 0; g < ngroup; ++g) {
    cuts[(size_t)g * (k + 1)] = lo;
    cuts[(size_t)g * (k + 1) + k] = hi_open;
  }
  if (k <= 1) return;
  if (!(span > 0)) {  // degenerate axis
    for (int g = 0; g < ngroup; ++g)
      for (int c = 1; c < k; ++c)
        cuts[(size_t)g * (k + 1) + c] = lo;
    return;
  }
  const int B1 = 2048, B2 = 512;   // 2048*512 ~ 1e6 resolution over the axis
  const double w1 = span / B1;

  // ---- coarse pass ----
  std::vector<long long> h1((size_t)ngroup * B1, 0);
  for (size_t s = 0; s < ns; ++s) {
    int b = (int)(((double)val[s] - lo) / w1);
    if (b < 0) b = 0;
    else if (b >= B1) b = B1 - 1;
    ++h1[(size_t)grp[s] * B1 + b];
  }
  MPI_Allreduce(MPI_IN_PLACE, h1.data(), (int)h1.size(), MPI_LONG_LONG, MPI_SUM,
                comm);

  const int nc = k - 1;
  std::vector<int> cbin((size_t)ngroup * nc, 0);
  std::vector<long long> cbefore((size_t)ngroup * nc, 0),
      ctarget((size_t)ngroup * nc, 0);
  for (int g = 0; g < ngroup; ++g) {
    long long T = 0;
    for (int b = 0; b < B1; ++b) T += h1[(size_t)g * B1 + b];
    int c = 1;
    long long acc = 0;
    for (int b = 0; b < B1 && c < k; ++b) {
      long long nxt = acc + h1[(size_t)g * B1 + b];
      while (c < k) {
        long long t = (long long)((__int128)T * c / k);
        if (t > nxt) break;
        size_t idx = (size_t)g * nc + (c - 1);
        cbin[idx] = b; cbefore[idx] = acc; ctarget[idx] = t;
        ++c;
      }
      acc = nxt;
    }
    for (; c < k; ++c) {  // T == 0, or all mass in the last bin
      size_t idx = (size_t)g * nc + (c - 1);
      cbin[idx] = B1 - 1;
      cbefore[idx] = acc - h1[(size_t)g * B1 + B1 - 1];
      ctarget[idx] = T;
    }
  }

  // ---- refine pass: one shared sub-histogram per distinct (group, bin) ----
  std::vector<int> slot_of((size_t)ngroup * B1, -1);
  int nslot = 0;
  for (int g = 0; g < ngroup; ++g)
    for (int c = 0; c < nc; ++c) {
      size_t key = (size_t)g * B1 + cbin[(size_t)g * nc + c];
      if (slot_of[key] < 0) slot_of[key] = nslot++;
    }
  std::vector<long long> h2((size_t)std::max(nslot, 1) * B2, 0);
  for (size_t s = 0; s < ns; ++s) {
    double t = ((double)val[s] - lo) / w1;
    int b = (int)t;
    if (b < 0) b = 0;
    else if (b >= B1) b = B1 - 1;
    int sl = slot_of[(size_t)grp[s] * B1 + b];
    if (sl < 0) continue;
    int sb = (int)((t - (double)b) * B2);
    if (sb < 0) sb = 0;
    else if (sb >= B2) sb = B2 - 1;
    ++h2[(size_t)sl * B2 + sb];
  }
  if (nslot)
    MPI_Allreduce(MPI_IN_PLACE, h2.data(), (int)h2.size(), MPI_LONG_LONG,
                  MPI_SUM, comm);

  for (int g = 0; g < ngroup; ++g) {
    for (int c = 1; c < k; ++c) {
      size_t idx = (size_t)g * nc + (c - 1);
      int b = cbin[idx];
      int sl = slot_of[(size_t)g * B1 + b];
      double cut = lo + (double)b * w1;
      if (sl >= 0) {
        long long acc = cbefore[idx], t = ctarget[idx];
        int sb = 0;
        for (; sb < B2; ++sb) {
          long long nxt = acc + h2[(size_t)sl * B2 + sb];
          if (t <= nxt) break;
          acc = nxt;
        }
        if (sb >= B2) sb = B2 - 1;
        cut = lo + ((double)b + (double)sb / B2) * w1;
      }
      cuts[(size_t)g * (k + 1) + c] = cut;
    }
    // Enforce monotonicity and the minimum slab width. min_width is chosen by
    // the caller as min(4*eps, span/k), so the forward sweep can always fit.
    double *cg = &cuts[(size_t)g * (k + 1)];
    for (int c = 1; c < k; ++c)
      if (cg[c] < cg[c - 1] + min_width) cg[c] = cg[c - 1] + min_width;
    for (int c = k - 1; c >= 1; --c)
      if (cg[c] > cg[c + 1] - min_width) cg[c] = cg[c + 1] - min_width;
    for (int c = 1; c < k; ++c)
      if (cg[c] < cg[c - 1]) cg[c] = cg[c - 1];
  }
}

struct Args {
  std::string filename;
  double eps = -1;
  double b_factor = 0.2;
  int minpts = 2;
  std::string impl = "fdbscan";
  int64_t max_particles = -1;
  bool verbose = false;
  int max_stitch_rounds = 512;
  // "uniform"  -- equal-WIDTH bricks. This is what HACC does, and it is the
  //               DEFAULT, because the point of this arm is to measure ArborX
  //               used the way HACC uses it, not to build the best possible
  //               distributed FoF around ArborX's kernel.
  // "balanced" -- equal-COUNT bricks (see Grid). Kept because it is the only
  //               way the 24B snapshot runs at these rank counts at all, and
  //               because the difference between the two IS a result: it
  //               separates "ArborX is slow here" from "the decomposition is
  //               imbalanced here".
  std::string partition = "uniform";
};

bool parse(int argc, char **argv, Args &a) {
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    auto need = [&](const char *w) -> const char * {
      if (i + 1 >= argc) { if (g_rank == 0) fprintf(stderr, "missing value after %s\n", w); return nullptr; }
      return argv[++i];
    };
    if (s == "-f" || s == "--filename") { auto v = need("-f"); if (!v) return false; a.filename = v; }
    else if (s == "-e" || s == "--eps") { auto v = need("-e"); if (!v) return false; a.eps = atof(v); }
    else if (s == "-b") { auto v = need("-b"); if (!v) return false; a.b_factor = atof(v); }
    else if (s == "-m" || s == "--core-min-size") { auto v = need("-m"); if (!v) return false; a.minpts = atoi(v); }
    else if (s == "-i" || s == "--impl") { auto v = need("-i"); if (!v) return false; a.impl = v; }
    else if (s == "-N") { auto v = need("-N"); if (!v) return false; a.max_particles = atoll(v); }
    else if (s == "-v" || s == "--verbose") a.verbose = true;
    else if (s == "-p" || s == "--partition") {
      auto v = need("-p"); if (!v) return false; a.partition = v;
      if (a.partition != "uniform" && a.partition != "balanced") {
        if (g_rank == 0) fprintf(stderr, "--partition must be uniform or balanced\n");
        return false;
      }
    }
    else { if (g_rank == 0) fprintf(stderr, "unknown option %s\n", argv[i]); return false; }
  }
  if (a.filename.empty()) { if (g_rank == 0) fprintf(stderr, "-f is required\n"); return false; }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);
  MPI_Comm comm = MPI_COMM_WORLD;
  MPI_Comm_rank(comm, &g_rank);
  MPI_Comm_size(comm, &g_nranks);
  const int rank = g_rank, nranks = g_nranks;
  // Line-buffered: under srun stdout is a pipe and therefore block-buffered,
  // so output printed shortly before an abort is lost with the buffer.
  setvbuf(stdout, nullptr, _IOLBF, 0);

  Args args;
  if (!parse(argc, argv, args)) { MPI_Finalize(); return 2; }

  Kokkos::initialize(argc, argv);
  {
    using ExecutionSpace = Kokkos::DefaultExecutionSpace;
    using MemorySpace = ExecutionSpace::memory_space;
    ExecutionSpace space{};

    if (rank == 0) {
      printf("=== ArborX local-kernel FoF with distributed stitching ===\n");
      printf("ArborX version    : %s (%s)\n", ArborX::version().c_str(),
             ArborX::gitCommitHash().c_str());
      printf("#MPI ranks        : %d\n", nranks);
      printf("exec space        : %s\n", ExecutionSpace::name());
      printf("impl              : %s\n", args.impl.c_str());
      printf("minpts            : %d\n", args.minpts);
    }

    // ---- 1. read a file-order slab ---------------------------------------
    cosmoio::Snapshot snap;
    std::string err;
    if (!cosmoio::open_snapshot(args.filename, snap, err)) die("open_snapshot", err);
    int64_t n_global = snap.n_total;
    if (args.max_particles > 0 && args.max_particles < n_global) n_global = args.max_particles;
    int64_t my_begin = (int64_t)((__int128)n_global * rank / nranks);
    int64_t my_end = (int64_t)((__int128)n_global * (rank + 1) / nranks);
    int64_t n_slab = my_end - my_begin;

    if (rank == 0) {
      printf("snapshot          : %s\n", args.filename.c_str());
      printf("n_total           : %lld (gas %lld dark %lld star %lld)\n",
             (long long)snap.n_total, (long long)snap.n_gas,
             (long long)snap.n_dark, (long long)snap.n_star);
      printf("n_used            : %lld\n", (long long)n_global);
    }

    MPI_Barrier(comm);
    double t0 = now();
    std::vector<float> xyz((size_t)3 * std::max<int64_t>(n_slab, 1));
    if (n_slab > 0 && !cosmoio::read_pos(snap, my_begin, n_slab, xyz.data(), err))
      die("read_pos", err);
    MPI_Barrier(comm);
    double t_read = now() - t0;

    // Global bounding box and the linking length.
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (int64_t i = 0; i < n_slab; ++i)
      for (int d = 0; d < 3; ++d) {
        double v = xyz[3 * i + d];
        if (v < lo[d]) lo[d] = v;
        if (v > hi[d]) hi[d] = v;
      }
    MPI_Allreduce(MPI_IN_PLACE, lo, 3, MPI_DOUBLE, MPI_MIN, comm);
    MPI_Allreduce(MPI_IN_PLACE, hi, 3, MPI_DOUBLE, MPI_MAX, comm);
    double V = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
    double eps = args.eps > 0 ? args.eps : args.b_factor * std::cbrt(V / (double)n_global);

    Grid grid;
    factor3(nranks, grid.px, grid.py, grid.pz);
    const int px = grid.px, py = grid.py, pz = grid.pz;
    // The halo test is widened by a relative 1e-6. ArborX evaluates the
    // squared distance in FLOAT, so a pair whose true separation is a few
    // times 1e-7*eps above eps can still be linked; if the halo used exactly
    // eps, such a pair would be linked when both points happen to land on one
    // rank and MISSED when the partition splits them. That is a
    // partition-dependent answer, which is exactly what a scaling study must
    // not have. Widening the halo (and only the halo -- the linking test is
    // still ArborX's) makes every pair ArborX could link present on both
    // sides at every rank count, at a cost of 1e-6 more halo particles.
    const double heps = eps * (1.0 + 1e-6);
    if (rank == 0) {
      printf("bbox              : [%.9g %.9g %.9g] .. [%.9g %.9g %.9g]\n",
             lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
      printf("eps (b)           : %.17g%s\n", eps, args.eps > 0 ? "  [given]" : "  [b*cbrt(V/N)]");
      printf("brick grid        : %d x %d x %d = %d  (%s)\n", px, py, pz,
             px * py * pz,
             args.partition == "uniform" ? "uniform, HACC-style"
                                         : "balanced rectilinear");
      printf("TIME read_s       : %.3f\n", t_read);
    }

    // ---- 2. balanced rectilinear partition, then redistribute -------------
    // Global ids travel with the coordinates: the stitch labels components by
    // the minimum GLOBAL particle id they contain, which is what makes the
    // label independent of how the data happens to be distributed.
    MPI_Barrier(comm);
    t0 = now();

    // The bbox is closed on the right, so the top edge is nudged open;
    // otherwise the maximum-coordinate particle would fall outside every
    // brick and slot() would have to clamp it back in.
    double hio_[3];
    for (int d = 0; d < 3; ++d) hio_[d] = std::nextafter(hi[d], hi[d] + 1.0);

    if (args.partition == "uniform") {
      // HACC's decomposition: equal-WIDTH bricks, no sampling, no histograms,
      // no communication. Every rank can compute every cut from the bounding
      // box alone. The cut ARRAYS are still filled in, so ownership and the
      // halo range search run the identical code in both modes -- the only
      // thing that changes is where the planes sit.
      grid.xc.assign(px + 1, 0.0);
      for (int c = 0; c <= px; ++c)
        grid.xc[c] = lo[0] + (hio_[0] - lo[0]) * (double)c / px;
      grid.yc.assign((size_t)px * (py + 1), 0.0);
      for (int i = 0; i < px; ++i)
        for (int c = 0; c <= py; ++c)
          grid.yc[(size_t)i * (py + 1) + c] =
              lo[1] + (hio_[1] - lo[1]) * (double)c / py;
      grid.zc.assign((size_t)px * py * (pz + 1), 0.0);
      for (int ij = 0; ij < px * py; ++ij)
        for (int c = 0; c <= pz; ++c)
          grid.zc[(size_t)ij * (pz + 1) + c] =
              lo[2] + (hio_[2] - lo[2]) * (double)c / pz;
    } else {

    // A strided sample of the local slab drives the quantile histograms. The
    // cuts only have to be good to a fraction of a percent, and a global
    // sample of ~64e6 points estimates every quantile to ~1e-4, so there is
    // no reason to histogram 5.8e10 particles three times over.
    int64_t stride = std::max<int64_t>(1, n_global / 64000000);
    int64_t ns = (n_slab + stride - 1) / stride;
    std::vector<float> sxyz((size_t)3 * std::max<int64_t>(ns, 1));
    for (int64_t k = 0, i = 0; i < n_slab; i += stride, ++k) {
      sxyz[3 * (size_t)k + 0] = xyz[3 * (size_t)i + 0];
      sxyz[3 * (size_t)k + 1] = xyz[3 * (size_t)i + 1];
      sxyz[3 * (size_t)k + 2] = xyz[3 * (size_t)i + 2];
    }
    std::vector<int> sg((size_t)std::max<int64_t>(ns, 1), 0);
    std::vector<float> sv((size_t)std::max<int64_t>(ns, 1), 0.f);
    // The bbox is closed on the right, so the top edge is nudged open;
    // otherwise the maximum-coordinate particle would fall outside every
    // brick and slot() would have to clamp it back in.
    const double *hio = hio_;

    for (int64_t k = 0; k < ns; ++k) sv[(size_t)k] = sxyz[3 * (size_t)k + 0];
    equal_count_cuts(comm, 1, px, lo[0], hio[0],
                     std::min(4.0 * eps, (hio[0] - lo[0]) / px), sg, sv, grid.xc);

    for (int64_t k = 0; k < ns; ++k) {
      sg[(size_t)k] = slot(grid.xc.data(), px, sxyz[3 * (size_t)k + 0]);
      sv[(size_t)k] = sxyz[3 * (size_t)k + 1];
    }
    equal_count_cuts(comm, px, py, lo[1], hio[1],
                     std::min(4.0 * eps, (hio[1] - lo[1]) / py), sg, sv, grid.yc);

    for (int64_t k = 0; k < ns; ++k) {
      int i = slot(grid.xc.data(), px, sxyz[3 * (size_t)k + 0]);
      int j = slot(grid.ycut(i), py, sxyz[3 * (size_t)k + 1]);
      sg[(size_t)k] = i * py + j;
      sv[(size_t)k] = sxyz[3 * (size_t)k + 2];
    }
    equal_count_cuts(comm, px * py, pz, lo[2], hio[2],
                     std::min(4.0 * eps, (hio[2] - lo[2]) / pz), sg, sv, grid.zc);
    sxyz.clear(); sxyz.shrink_to_fit();
    sg.clear(); sg.shrink_to_fit();
    sv.clear(); sv.shrink_to_fit();
    }  // end balanced

    std::vector<int> sendc(nranks, 0), recvc(nranks, 0), sendd(nranks, 0), recvd(nranks, 0);
    std::vector<int> dest((size_t)std::max<int64_t>(n_slab, 1));
    for (int64_t i = 0; i < n_slab; ++i) { int d = grid.brick_of(&xyz[3 * i]); dest[i] = d; ++sendc[d]; }
    for (int r = 1; r < nranks; ++r) sendd[r] = sendd[r - 1] + sendc[r - 1];
    std::vector<float> sbuf((size_t)3 * std::max<int64_t>(n_slab, 1));
    std::vector<long long> sgid((size_t)std::max<int64_t>(n_slab, 1));
    {
      std::vector<int> cur = sendd;
      for (int64_t i = 0; i < n_slab; ++i) {
        int slot = cur[dest[i]]++;
        sbuf[3 * (size_t)slot + 0] = xyz[3 * i + 0];
        sbuf[3 * (size_t)slot + 1] = xyz[3 * i + 1];
        sbuf[3 * (size_t)slot + 2] = xyz[3 * i + 2];
        sgid[(size_t)slot] = (long long)(my_begin + i);
      }
    }
    MPI_Alltoall(sendc.data(), 1, MPI_INT, recvc.data(), 1, MPI_INT, comm);
    int64_t n_own = 0;
    for (int r = 0; r < nranks; ++r) { recvd[r] = (int)n_own; n_own += recvc[r]; }
    if (n_own > (int64_t)2000000000)
      die("redistribute", "this brick holds " + std::to_string(n_own) +
                          " particles, past ArborX's int32 local indexing; use more ranks");

    MPI_Datatype F3;
    MPI_Type_contiguous(3, MPI_FLOAT, &F3);
    MPI_Type_commit(&F3);
    std::vector<float> own((size_t)3 * std::max<int64_t>(n_own, 1));
    std::vector<long long> gid((size_t)std::max<int64_t>(n_own, 1));
    MPI_Alltoallv(sbuf.data(), sendc.data(), sendd.data(), F3,
                  own.data(), recvc.data(), recvd.data(), F3, comm);
    MPI_Alltoallv(sgid.data(), sendc.data(), sendd.data(), MPI_LONG_LONG,
                  gid.data(), recvc.data(), recvd.data(), MPI_LONG_LONG, comm);
    MPI_Type_free(&F3);
    sbuf.clear(); sbuf.shrink_to_fit();
    sgid.clear(); sgid.shrink_to_fit();
    xyz.clear(); xyz.shrink_to_fit();
    dest.clear(); dest.shrink_to_fit();

    // ---- 3. halo (overload) exchange -------------------------------------
    // A particle is sent to every brick whose box comes within heps of it.
    // With a balanced rectilinear grid that set is no longer "the 26
    // neighbours": the y cuts differ between x-slabs and the z cuts between
    // columns, so bricks do not line up face to face, and where the data is
    // dense a brick can be thinner than eps and be reached across. The range
    // search below is exact for all of those cases -- it walks the same cut
    // arrays slot() uses for ownership, so a pair straddling any boundary is
    // guaranteed to be present on BOTH of the bricks that hold its ends.
    //
    // The overwhelmingly common case still costs nothing: a particle at least
    // heps inside its own brick in all three axes exits on the first test.
    int myi, myj, myk;
    {
      // Recover this rank's brick indices from its rank id.
      myk = rank % pz; int t = rank / pz; myj = t % py; myi = t / py;
    }
    const double *myzc = grid.zcut(myi, myj);
    const double *myyc = grid.ycut(myi);
    double blo[3] = {grid.xc[myi],   myyc[myj],   myzc[myk]};
    double bhi[3] = {grid.xc[myi+1], myyc[myj+1], myzc[myk+1]};

    std::fill(sendc.begin(), sendc.end(), 0);
    std::vector<int> hsend;  // (point, neighbour rank) pairs, flattened later
    hsend.reserve((size_t)n_own / 8 + 16);
    std::vector<int> hdest;
    hdest.reserve((size_t)n_own / 8 + 16);
    for (int64_t i = 0; i < n_own; ++i) {
      const float *p = &own[3 * i];
      if ((double)p[0] - heps >= blo[0] && (double)p[0] + heps <= bhi[0] &&
          (double)p[1] - heps >= blo[1] && (double)p[1] + heps <= bhi[1] &&
          (double)p[2] - heps >= blo[2] && (double)p[2] + heps <= bhi[2])
        continue;  // interior particle: reaches no other brick
      int i0 = slot(grid.xc.data(), px, (double)p[0] - heps);
      int i1 = slot(grid.xc.data(), px, (double)p[0] + heps);
      for (int a = i0; a <= i1; ++a) {
        const double *yca = grid.ycut(a);
        int j0 = slot(yca, py, (double)p[1] - heps);
        int j1 = slot(yca, py, (double)p[1] + heps);
        for (int b = j0; b <= j1; ++b) {
          const double *zcab = grid.zcut(a, b);
          int k0 = slot(zcab, pz, (double)p[2] - heps);
          int k1 = slot(zcab, pz, (double)p[2] + heps);
          for (int c = k0; c <= k1; ++c) {
            int nr = grid.rank_of(a, b, c);
            if (nr == rank) continue;
            hsend.push_back((int)i);
            hdest.push_back(nr);
            ++sendc[nr];
          }
        }
      }
    }
    if ((int64_t)hsend.size() > (int64_t)2000000000)
      die("halo", "this brick sends " + std::to_string(hsend.size()) +
                  " halo copies, past the int displacements MPI_Alltoallv "
                  "takes; use more ranks");
    for (int r = 1; r < nranks; ++r) sendd[r] = sendd[r - 1] + sendc[r - 1];
    int64_t n_hsend = (int64_t)hsend.size();
    std::vector<float> hxyz((size_t)3 * std::max<int64_t>(n_hsend, 1));
    std::vector<long long> hgid((size_t)std::max<int64_t>(n_hsend, 1));
    // back_idx[slot] is the OWN-point index that forward halo slot `slot`
    // came from. It has to be recorded HERE, while the mapping exists: the
    // coordinates are freed before the stitch, so the forward loop cannot be
    // replayed later. (The first draft of this file tried to and left
    // back_idx all -1, which silently disabled the entire label return --
    // the stitch would have run its rounds and propagated nothing.)
    std::vector<int> back_idx((size_t)std::max<int64_t>(n_hsend, 1), -1);
    {
      std::vector<int> cur = sendd;
      for (int64_t k = 0; k < n_hsend; ++k) {
        int slot = cur[hdest[k]]++;
        int i = hsend[k];
        hxyz[3 * (size_t)slot + 0] = own[3 * i + 0];
        hxyz[3 * (size_t)slot + 1] = own[3 * i + 1];
        hxyz[3 * (size_t)slot + 2] = own[3 * i + 2];
        hgid[(size_t)slot] = gid[i];
        back_idx[(size_t)slot] = i;
      }
    }
    hsend.clear(); hsend.shrink_to_fit();
    hdest.clear(); hdest.shrink_to_fit();
    MPI_Alltoall(sendc.data(), 1, MPI_INT, recvc.data(), 1, MPI_INT, comm);
    int64_t n_halo = 0;
    for (int r = 0; r < nranks; ++r) { recvd[r] = (int)n_halo; n_halo += recvc[r]; }

    MPI_Type_contiguous(3, MPI_FLOAT, &F3);
    MPI_Type_commit(&F3);
    std::vector<float> hrx((size_t)3 * std::max<int64_t>(n_halo, 1));
    std::vector<long long> hrg((size_t)std::max<int64_t>(n_halo, 1));
    MPI_Alltoallv(hxyz.data(), sendc.data(), sendd.data(), F3,
                  hrx.data(), recvc.data(), recvd.data(), F3, comm);
    MPI_Alltoallv(hgid.data(), sendc.data(), sendd.data(), MPI_LONG_LONG,
                  hrg.data(), recvc.data(), recvd.data(), MPI_LONG_LONG, comm);
    MPI_Type_free(&F3);
    // Remember who sent each halo point so its label can be sent home.
    std::vector<int> halo_src((size_t)std::max<int64_t>(n_halo, 1));
    for (int r = 0; r < nranks; ++r)
      for (int k = 0; k < recvc[r]; ++k) halo_src[(size_t)(recvd[r] + k)] = r;
    hxyz.clear(); hxyz.shrink_to_fit();
    hgid.clear(); hgid.shrink_to_fit();
    MPI_Barrier(comm);
    double t_decomp = now() - t0;

    {
      long long mn = n_own, mx = n_own, sum = n_own, hsum = n_halo, hmx = n_halo;
      MPI_Allreduce(MPI_IN_PLACE, &mn, 1, MPI_LONG_LONG, MPI_MIN, comm);
      MPI_Allreduce(MPI_IN_PLACE, &mx, 1, MPI_LONG_LONG, MPI_MAX, comm);
      MPI_Allreduce(MPI_IN_PLACE, &sum, 1, MPI_LONG_LONG, MPI_SUM, comm);
      MPI_Allreduce(MPI_IN_PLACE, &hsum, 1, MPI_LONG_LONG, MPI_SUM, comm);
      MPI_Allreduce(MPI_IN_PLACE, &hmx, 1, MPI_LONG_LONG, MPI_MAX, comm);
      double wmin = 1e300;
      for (int d = 0; d < 3; ++d) {
        double w = bhi[d] - blo[d];
        if (w < wmin) wmin = w;
      }
      MPI_Allreduce(MPI_IN_PLACE, &wmin, 1, MPI_DOUBLE, MPI_MIN, comm);
      if (rank == 0) {
        printf("TIME decomp_s     : %.3f\n", t_decomp);
        printf("per-rank own      : min %lld max %lld mean %.0f imbalance %.3f\n",
               mn, mx, (double)sum / nranks, (double)mx / ((double)sum / nranks));
        printf("halo              : total %lld (%.2f%% of N) max/rank %lld\n",
               hsum, 100.0 * (double)hsum / (double)n_global, hmx);
        printf("thinnest brick    : %.6g  (eps/side = %.4f)\n", wmin, eps / wmin);
        if (sum != n_global)
          printf("WARNING census    : brick redistribution holds %lld of %lld\n",
                 sum, (long long)n_global);
      }
    }

    // ---- 4. ArborX single-rank DBSCAN over own + halo ---------------------
    int64_t n_loc = n_own + n_halo;
    std::vector<float> all((size_t)3 * std::max<int64_t>(n_loc, 1));
    std::memcpy(all.data(), own.data(), (size_t)3 * n_own * sizeof(float));
    if (n_halo > 0)
      std::memcpy(all.data() + 3 * n_own, hrx.data(), (size_t)3 * n_halo * sizeof(float));
    hrx.clear(); hrx.shrink_to_fit();
    own.clear(); own.shrink_to_fit();

    Kokkos::View<Point *, MemorySpace> points(
        Kokkos::view_alloc(space, Kokkos::WithoutInitializing, "hacc::points"), n_loc);
    {
      Kokkos::View<Point *, Kokkos::HostSpace, Kokkos::MemoryTraits<Kokkos::Unmanaged>>
          hv(reinterpret_cast<Point *>(all.data()), n_loc);
      Kokkos::deep_copy(space, points, hv);
      space.fence();
    }
    all.clear(); all.shrink_to_fit();

    ArborX::DBSCAN::Parameters p;
    // MUST be set: Parameters defaults _implementation to FDBSCAN_DenseBox,
    // which builds a (box/eps)^3 cell grid -- 2.5e11 cells on the 2B set.
    p.setVerbosity(args.verbose)
        .setImplementation(args.impl == "fdbscan-densebox"
                               ? ArborX::DBSCAN::Implementation::FDBSCAN_DenseBox
                               : ArborX::DBSCAN::Implementation::FDBSCAN);

    MPI_Barrier(comm);
    t0 = now();
    Kokkos::View<int *, MemorySpace> loc_labels("hacc::loc_labels", 0);
    ArborX::dbscan(space, points, eps, args.minpts, loc_labels, p);
    space.fence();
    MPI_Barrier(comm);
    double t_dbscan = now() - t0;
    if (rank == 0) printf("TIME dbscan_s     : %.3f\n", t_dbscan);

    auto h_loc = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, loc_labels);

    // ---- 5. stitch --------------------------------------------------------
    // label[i] starts at the point's own global id (halo points included).
    // Repeatedly: take the minimum label within each local component and
    // assign it to every member; send halo labels home; owners take minima.
    // Stop when no label changes anywhere.
    MPI_Barrier(comm);
    t0 = now();
    // Every point -- own or halo, clustered or noise -- is given a component
    // id, so the ENTIRE state of the stitch is one long long per component.
    // ArborX's serial labels are the local index of a representative, or -1
    // for a point with no neighbour within eps; a -1 point becomes its own
    // singleton component rather than being skipped, because such a point can
    // still be joined to a cluster that lives on another rank.
    //
    // Keeping the state per COMPONENT rather than per POINT is what makes the
    // rounds cheap: lowering a component's label lowers it for every member
    // at once, so a round costs O(n_halo + n_hsend) -- a few million -- and
    // not O(n_loc). The per-point version of this loop performed 2.9e10 label
    // writes on the 58B snapshot and spent 44 s in a phase whose actual
    // communication is a few megabytes.
    std::vector<int> comp_of((size_t)std::max<int64_t>(n_loc, 1));
    std::vector<long long> cmin;
    {
      std::vector<int> remap((size_t)std::max<int64_t>(n_loc, 1), -1);
      for (int64_t i = 0; i < n_loc; ++i) {
        long long base = (i < n_own) ? gid[(size_t)i] : hrg[(size_t)(i - n_own)];
        int L = h_loc((size_t)i);
        if (L < 0) {
          comp_of[(size_t)i] = (int)cmin.size();
          cmin.push_back(base);
        } else if (remap[(size_t)L] < 0) {
          remap[(size_t)L] = (int)cmin.size();
          comp_of[(size_t)i] = (int)cmin.size();
          cmin.push_back(base);
        } else {
          int r = remap[(size_t)L];
          comp_of[(size_t)i] = r;
          if (base < cmin[(size_t)r]) cmin[(size_t)r] = base;
        }
      }
    }
    const int64_t n_comp = (int64_t)cmin.size();

    std::vector<long long> hsend_lab((size_t)std::max<int64_t>(n_halo, 1));
    std::vector<long long> hback((size_t)std::max<int64_t>(n_hsend, 1));
    // Reverse of the halo exchange: counts swap roles.
    std::vector<int> bsendc(nranks, 0), bsendd(nranks, 0), brecvc(nranks, 0), brecvd(nranks, 0);
    for (int r = 0; r < nranks; ++r) bsendc[r] = recvc[r];   // halo points I hold, per owner
    for (int r = 1; r < nranks; ++r) bsendd[r] = bsendd[r-1] + bsendc[r-1];
    MPI_Alltoall(bsendc.data(), 1, MPI_INT, brecvc.data(), 1, MPI_INT, comm);
    { int64_t a = 0; for (int r = 0; r < nranks; ++r) { brecvd[r] = (int)a; a += brecvc[r]; } }

    // The return Alltoallv lands label k in the SAME slot ordering the
    // forward halo send used: its receive displacements are the prefix sum of
    // `sendc`, which is exactly `sendd`, and within a rank the order is
    // preserved. So back_idx[k], recorded above, is the owner-side index.

    int rounds = 0;
    long long total_changed = 0;
    for (; rounds < args.max_stitch_rounds; ++rounds) {
      // Each halo point reports the current label of the local component it
      // joined here; its owner folds that into the component IT put the point
      // in. That is one full round of label propagation across rank
      // boundaries, and it is a fixed point exactly when no component's label
      // falls anywhere.
      for (int64_t i = 0; i < n_halo; ++i)
        hsend_lab[(size_t)i] = cmin[(size_t)comp_of[(size_t)(n_own + i)]];
      MPI_Alltoallv(hsend_lab.data(), bsendc.data(), bsendd.data(), MPI_LONG_LONG,
                    hback.data(), brecvc.data(), brecvd.data(), MPI_LONG_LONG, comm);
      // hback is in the SAME order as the forward halo send, so element k
      // corresponds to the k-th (own point, neighbour) pair, which is exactly
      // the ordering `back_idx` records.
      long long changed = 0;
      for (int64_t k = 0; k < n_hsend; ++k) {
        int i = back_idx[(size_t)k];
        if (i < 0) continue;
        int c = comp_of[(size_t)i];
        if (hback[(size_t)k] < cmin[(size_t)c]) {
          cmin[(size_t)c] = hback[(size_t)k];
          ++changed;
        }
      }
      long long g = changed;
      MPI_Allreduce(MPI_IN_PLACE, &g, 1, MPI_LONG_LONG, MPI_SUM, comm);
      total_changed += g;
      if (g == 0) break;
    }
    MPI_Barrier(comm);
    double t_stitch = now() - t0;
    if (rounds >= args.max_stitch_rounds && rank == 0)
      printf("WARNING stitch    : hit the %d-round cap WITHOUT converging; the "
             "component count below is an UNDER-merge and must not be quoted.\n",
             args.max_stitch_rounds);
    if (rank == 0)
      printf("TIME stitch_s     : %.3f  (rounds %d, component-min updates %lld)\n",
             t_stitch, rounds, total_changed);

    // ---- component count, with NO communication at all -------------------
    // A component's label is the minimum global id it contains, and the
    // particle carrying that gid is owned by exactly ONE rank. At the stitch's
    // fixed point every member of a component carries that same minimum, so
    // exactly one own point per component satisfies label == gid. Counting
    // those and summing is therefore exact, and needs no exchange -- which
    // matters because the exchange is the part that breaks at scale.
    long long my_reps = 0;
    for (int64_t i = 0; i < n_own; ++i)
      if (cmin[(size_t)comp_of[(size_t)i]] == gid[(size_t)i]) ++my_reps;
    long long n_comp_global = 0, n_labelled = 0, my_n_own = n_own;
    MPI_Reduce(&my_reps, &n_comp_global, 1, MPI_LONG_LONG, MPI_SUM, 0, comm);
    MPI_Reduce(&my_n_own, &n_labelled, 1, MPI_LONG_LONG, MPI_SUM, 0, comm);
    if (rank == 0) {
      printf("CENSUS_CHECK %s  labelled %lld of %lld\n",
             n_labelled == (long long)n_global ? "PASS" : "FAIL",
             n_labelled, (long long)n_global);
      printf("HACCCOUNT components: %lld\n", n_comp_global);
      fflush(stdout);
    }

    // Release everything the census does not read. On the 24B snapshot this
    // is the difference between finishing and not: at 128 ranks the census
    // needs two 1.5 GB buffers on top of a 25 GB resident set, and the NIC
    // has to register the send buffer.
    hrg.clear(); hrg.shrink_to_fit();
    gid.clear(); gid.shrink_to_fit();
    back_idx.clear(); back_idx.shrink_to_fit();
    hsend_lab.clear(); hsend_lab.shrink_to_fit();
    hback.clear(); hback.shrink_to_fit();
    halo_src.clear(); halo_src.shrink_to_fit();
    loc_labels = Kokkos::View<int *, MemorySpace>("hacc::loc_labels", 0);
    points = Kokkos::View<Point *, MemorySpace>(
        Kokkos::view_alloc(space, Kokkos::WithoutInitializing, "hacc::points"), 0);
    h_loc = decltype(h_loc)();

    // ---- max_size, with only O(candidates) communication -----------------
    // The component COUNT above is already exact and needs no exchange.
    // max_size is the one remaining quantity that is not local, and the old
    // route -- hash every own label to a peer and exchange all n_own of them
    // -- is what broke at scale. MPI registers each peer's slice with the
    // NIC, and after ArborX's DBSCAN has been through it that fails with
    //   cxil_map: write error
    // Cutting the messages by size did not help (each peer's slice was only
    // 12 MB at 128 nodes) and neither did throttling to 32 peers per wave:
    // jobs 5405685, 5405686 and 5406964 all died there AFTER printing a
    // valid HACCCOUNT. The binding limit is the aggregate registration, so
    // the fix has to be volume, not scheduling.
    //
    // Bound the answer, then exchange only what can possibly attain it.
    // Write l_c(r) for the number of rank-r own particles carrying label c,
    // and s_c = sum_r l_c(r) for the component's true size. Let
    //   Lmax = max over all (c, r) of l_c(r)          [one scalar allreduce]
    //   T    = max(1, Lmax / nranks)
    // and call (c, r) a CANDIDATE slice when l_c(r) >= T.
    //   * A component with no candidate slice has s_c <= nranks*(T-1)
    //     < nranks*T <= Lmax <= max_size, so it cannot be the maximum.
    //     Non-candidates are therefore provably irrelevant.
    //   * Slices are disjoint, so there are at most n_global/T candidates
    //     in total -- tens of thousands here, not billions.
    // Summing only candidate slices gives a partial p_c <= s_c, and since
    // every omitted slice is < T and there are at most nranks of them,
    // s_c < p_c + nranks*T. So with B = max_c p_c (a valid lower bound on
    // max_size), the argmax must satisfy p_c > B - nranks*T. That SHORTLIST
    // is broadcast and summed exactly -- over all slices, not just the
    // candidate ones -- which makes the reported max_size exact, not a
    // bound.
    {
      // (a) own-particle count per label. Per-LABEL and not per local
      // component index: two local components can end up carrying the same
      // label (two blobs joined through a neighbour's territory), and the
      // bound above is only valid for per-rank, per-label slices.
      std::vector<int> cnt((size_t)std::max<int64_t>(n_comp, 1), 0);
      for (int64_t i = 0; i < n_own; ++i) ++cnt[(size_t)comp_of[(size_t)i]];
      std::vector<std::pair<long long, long long>> slice;
      slice.reserve((size_t)n_comp);
      for (int64_t c = 0; c < n_comp; ++c)
        if (cnt[(size_t)c] > 0)
          slice.emplace_back(cmin[(size_t)c], (long long)cnt[(size_t)c]);
      cnt.clear(); cnt.shrink_to_fit();
      comp_of.clear(); comp_of.shrink_to_fit();
      cmin.clear(); cmin.shrink_to_fit();
      std::sort(slice.begin(), slice.end());
      {
        size_t w = 0;
        for (size_t i = 0; i < slice.size();) {
          size_t j = i;
          long long s = 0;
          while (j < slice.size() && slice[j].first == slice[i].first)
            s += slice[j++].second;
          slice[w++] = {slice[i].first, s};
          i = j;
        }
        slice.resize(w);
      }

      // Every own particle must be accounted into exactly one label.
      long long my_acc = 0;
      for (const auto &p : slice) my_acc += p.second;
      long long tot_acc = 0;
      MPI_Reduce(&my_acc, &tot_acc, 1, MPI_LONG_LONG, MPI_SUM, 0, comm);

      // (b) the threshold
      long long my_lmax = 0;
      for (const auto &p : slice) my_lmax = std::max(my_lmax, p.second);
      long long Lmax = 0;
      MPI_Allreduce(&my_lmax, &Lmax, 1, MPI_LONG_LONG, MPI_MAX, comm);
      const long long T = std::max<long long>(1, Lmax / nranks);

      // (c) hash-partition the candidate slices by label and exchange
      std::vector<int> sc(nranks, 0), rc(nranks, 0), sd(nranks, 0), rd(nranks, 0);
      auto owner = [&](long long L) {
        uint64_t z = (uint64_t)L + 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        z ^= z >> 31;
        return (int)(z % (uint64_t)nranks);
      };
      long long my_cand = 0;
      for (const auto &p : slice)
        if (p.second >= T) { sc[owner(p.first)] += 2; ++my_cand; }
      long long n_cand = 0, max_cand = 0;
      MPI_Allreduce(&my_cand, &n_cand, 1, MPI_LONG_LONG, MPI_SUM, comm);
      MPI_Allreduce(&my_cand, &max_cand, 1, MPI_LONG_LONG, MPI_MAX, comm);
      // sc/sd are int arrays because MPI_Alltoallv takes int counts, and each
      // candidate contributes TWO long longs, so the per-rank candidate count
      // has to stay under INT_MAX/2. On clustered cosmology data T is ~1e5 and
      // this is off by four orders of magnitude, but a pathological input (all
      // components tiny -> Lmax small -> T == 1 -> every component a
      // candidate) would overflow it silently, which is exactly the class of
      // bug this census already produced once.
      if (max_cand > (long long)(INT_MAX / 2)) {
        if (rank == 0)
          printf("FATAL census    : %lld candidate slices on one rank exceeds "
                 "INT_MAX/2; Lmax %lld gives T %lld, too small to prune\n",
                 max_cand, Lmax, T);
        MPI_Abort(comm, 1);
      }
      for (int r = 1; r < nranks; ++r) sd[r] = sd[r-1] + sc[r-1];
      int64_t ns = (int64_t)sd[nranks-1] + sc[nranks-1];
      std::vector<long long> sb((size_t)std::max<int64_t>(ns, 1));
      { std::vector<int> cur = sd;
        for (const auto &p : slice)
          if (p.second >= T) {
            int o = owner(p.first);
            sb[(size_t)(cur[o]++)] = p.first;
            sb[(size_t)(cur[o]++)] = p.second;
          } }
      MPI_Alltoall(sc.data(), 1, MPI_INT, rc.data(), 1, MPI_INT, comm);
      int64_t nr = 0;
      for (int r = 0; r < nranks; ++r) { rd[r] = (int)nr; nr += rc[r]; }
      std::vector<long long> rb((size_t)std::max<int64_t>(nr, 1));
      MPI_Alltoallv(sb.data(), sc.data(), sd.data(), MPI_LONG_LONG,
                    rb.data(), rc.data(), rd.data(), MPI_LONG_LONG, comm);
      sb.clear(); sb.shrink_to_fit();

      // (d) partial sums per label, and the lower bound B
      std::vector<std::pair<long long, long long>> part;
      part.reserve((size_t)(nr / 2));
      for (int64_t i = 0; i + 1 < nr; i += 2)
        part.emplace_back(rb[(size_t)i], rb[(size_t)i + 1]);
      rb.clear(); rb.shrink_to_fit();
      std::sort(part.begin(), part.end());
      { size_t w = 0;
        for (size_t i = 0; i < part.size();) {
          size_t j = i;
          long long s = 0;
          while (j < part.size() && part[j].first == part[i].first)
            s += part[j++].second;
          part[w++] = {part[i].first, s};
          i = j;
        } part.resize(w); }
      long long my_best = 0;
      for (const auto &p : part) my_best = std::max(my_best, p.second);
      long long B = 0;
      MPI_Allreduce(&my_best, &B, 1, MPI_LONG_LONG, MPI_MAX, comm);

      // (e) the shortlist: every label that could still beat B
      const long long floor_p = B - (long long)nranks * T;
      std::vector<long long> mine;
      for (const auto &p : part)
        if (p.second > floor_p) mine.push_back(p.first);
      int my_n = (int)mine.size();
      std::vector<int> allc(nranks), alld(nranks);
      MPI_Allgather(&my_n, 1, MPI_INT, allc.data(), 1, MPI_INT, comm);
      int64_t tot_s = 0;
      for (int r = 0; r < nranks; ++r) { alld[r] = (int)tot_s; tot_s += allc[r]; }
      // The shortlist is replicated on every rank, so it has to stay small.
      // It is tiny whenever B >> nranks*T, which is the clustered case: on
      // cosmo25 B is 2.2e9 and nranks*T is ~1.7e7, so exactly one label
      // survives. It is NOT small when every component is tiny -- then B and
      // nranks*T are comparable, floor_p goes negative and everything
      // survives. Refuse to allgather that rather than exhaust memory; B is
      // still a valid lower bound and is reported as one.
      const int64_t SHORTLIST_CAP = 4000000;   // 32 MB replicated
      int shortlist_ok = (tot_s <= SHORTLIST_CAP) ? 1 : 0;
      if (!shortlist_ok) {
        if (rank == 0)
          printf("WARNING census    : shortlist of %lld labels exceeds the "
                 "%lld cap (Lmax %lld T %lld B %lld) -- max_size below is a "
                 "LOWER BOUND, not the exact maximum\n",
                 (long long)tot_s, (long long)SHORTLIST_CAP, Lmax, T, B);
        tot_s = 0; my_n = 0;
        for (int r = 0; r < nranks; ++r) { allc[r] = 0; alld[r] = 0; }
        mine.clear();
      }
      std::vector<long long> shortl((size_t)std::max<int64_t>(tot_s, 1));
      MPI_Allgatherv(mine.data(), my_n, MPI_LONG_LONG, shortl.data(),
                     allc.data(), alld.data(), MPI_LONG_LONG, comm);
      shortl.resize((size_t)std::max<int64_t>(tot_s, 0));
      std::sort(shortl.begin(), shortl.end());
      shortl.erase(std::unique(shortl.begin(), shortl.end()), shortl.end());

      // (f) exact sizes for the shortlist, over ALL slices
      const size_t K = shortl.size();
      std::vector<long long> loc(std::max<size_t>(K, 1), 0), glob(std::max<size_t>(K, 1), 0);
      for (size_t k = 0; k < K; ++k) {
        auto it = std::lower_bound(slice.begin(), slice.end(),
                                   std::make_pair(shortl[k], (long long)0));
        if (it != slice.end() && it->first == shortl[k]) loc[k] = it->second;
      }
      if (K > 0)
        MPI_Allreduce(loc.data(), glob.data(), (int)K, MPI_LONG_LONG, MPI_SUM, comm);
      long long mx = 0;
      for (size_t k = 0; k < K; ++k) mx = std::max(mx, glob[k]);
      if (!shortlist_ok) mx = B;   // lower bound; warned about above

      if (rank == 0) {
        printf("census            : Lmax %lld T %lld candidates %lld "
               "shortlist %zu max_size=%s\n", Lmax, T, n_cand, K,
               shortlist_ok ? "exact" : "LOWER BOUND");
        printf("CENSUS_CHECK2 %s  accounted %lld of %lld\n",
               tot_acc == (long long)n_global ? "PASS" : "FAIL", tot_acc,
               (long long)n_global);
        // mx is exact, so it must not be below the partial-sum lower bound.
        if (shortlist_ok && mx < B)
          printf("WARNING census    : exact max %lld is below the partial-sum "
                 "lower bound %lld -- the shortlist logic is wrong\n", mx, B);
        printf("HACCSTAT components: %lld max_size %lld\n", n_comp_global, mx);
        printf("HACCRESULT ranks=%d n=%lld eps=%.17g read_s=%.3f decomp_s=%.3f "
               "dbscan_s=%.3f stitch_s=%.3f solve_s=%.3f\n",
               nranks, (long long)n_global, eps, t_read, t_decomp, t_dbscan,
               t_stitch, t_dbscan + t_stitch);
      }
    }
  }
  Kokkos::finalize();
  MPI_Finalize();
  return 0;
}
