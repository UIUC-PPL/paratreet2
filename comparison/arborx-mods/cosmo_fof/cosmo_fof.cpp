/****************************************************************************
 * cosmo_fof -- run ArborX's distributed DBSCAN as a friends-of-friends
 * cluster finder over a Tipsy or NChilada cosmology snapshot.
 *
 * WHY THIS EXISTS, rather than ArborX's own benchmarks/cluster driver:
 *
 *  1. Its binary reader declares the point count as `int` (data_timpl.hpp:
 *     `input.read(&num_points, sizeof(int))`), so it cannot express the 24B
 *     or 58B snapshots at all, and its per-rank seek
 *     `num_points_per_proc * comm_rank * value_size` multiplies two ints
 *     before promoting, which overflows past ~2^31 particles even if the
 *     header could hold the count.
 *  2. Its format is a flat interleaved xyz float32 array. Materialising the
 *     58B snapshot in it would cost 696 GB of Lustre and a full extra
 *     read+write pass; NChilada's `pos` field file already IS a flat xyz
 *     float32 array (behind a 52-byte header, big-endian), so reading it
 *     directly is both cheaper and one fewer place for the data to change.
 *  3. ArborX's distributed DBSCAN REQUIRES a spatially coherent initial
 *     distribution and does not create one. `computeRanksTo` builds a BVH
 *     over the per-rank bounding boxes and queries every local point,
 *     inflated by eps, against it. Under the file-order slab that the stock
 *     reader produces, every rank's box spans the whole domain, every point
 *     therefore matches all `comm_size` boxes, and the ghost exchange
 *     degenerates to n_local * comm_size exports -- 5.8e10 per rank at 4096
 *     ranks on the 58B set. That is not a fair measurement of ArborX; it is
 *     a measurement of a missing decomposition. The ArborX exascale paper's
 *     setting is data already domain-decomposed by the host simulation, so
 *     this driver supplies the equivalent: a Morton-order partition with
 *     equal particle counts per rank, TIMED AND REPORTED SEPARATELY from the
 *     DBSCAN itself, exactly as FoF3's "Total Decomposition time" is
 *     reported separately from its "Iteration 0 time".
 *
 * WHAT IS COMPARED. FoF at linking length b is precisely the connected
 * components of the graph joining particles closer than b, which is DBSCAN
 * with minPts = 2 (ArborX special-cases that to a connected-components
 * solve). The census printed at the end therefore reports components under
 * BOTH conventions -- counting isolated particles as their own component and
 * not counting them -- because the two codes need not agree on that a priori
 * and the gate has to be checked, not assumed.
 ****************************************************************************/

#include <ArborX_DBSCAN.hpp>
#include <ArborX_DistributedDBSCAN.hpp>
#include "../cluster/ArborX_DBSCANVerification.hpp"
#include <ArborX_Point.hpp>
#include <ArborX_Version.hpp>

#include <Kokkos_Core.hpp>

#include <mpi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <string>
#include <vector>

#include "cosmoio.hpp"

using Point = ArborX::Point<3, float>;

// The host-side staging view is an UNMANAGED view over the float vector the
// reader filled, via reinterpret_cast. That is only valid if a Point is
// exactly three tightly packed floats; any padding would reinterpret every
// third coordinate as the next point's x and produce a plausible-looking but
// wrong answer. Checked here rather than trusted.
static_assert(sizeof(Point) == 3 * sizeof(float),
              "ArborX::Point<3,float> is not 3 tightly packed floats; the "
              "reinterpret_cast staging in main() would be invalid");

namespace {

double now() { return MPI_Wtime(); }

void die(int rank, const char* what, const std::string& why) {
  fprintf(stderr, "[%d] FATAL %s: %s\n", rank, what, why.c_str());
  fflush(stderr);
  MPI_Abort(MPI_COMM_WORLD, 1);
}

// ---- Morton keys ----------------------------------------------------------
// 21 bits per dimension -> a 63-bit key, so the cell grid is 2^21 on a side.
// That is far finer than any per-rank region at the scales here, so equal
// keys never dominate a rank's share.
inline uint64_t spread21(uint64_t v) {
  v &= 0x1fffffULL;
  v = (v | (v << 32)) & 0x1f00000000ffffULL;
  v = (v | (v << 16)) & 0x1f0000ff0000ffULL;
  v = (v | (v << 8)) & 0x100f00f00f00f00fULL;
  v = (v | (v << 4)) & 0x10c30c30c30c30c3ULL;
  v = (v | (v << 2)) & 0x1249249249249249ULL;
  return v;
}
inline uint64_t morton3(uint32_t x, uint32_t y, uint32_t z) {
  return spread21(x) | (spread21(y) << 1) | (spread21(z) << 2);
}

struct Args {
  std::string filename;
  double eps = -1;         // absolute linking length; -1 => derive from b
  double b_factor = 0.2;   // b = b_factor * cbrt(V/N)
  int minpts = 2;
  std::string impl = "fdbscan";
  int64_t max_particles = -1;
  int sample_per_rank = 1024;
  bool census = true;
  bool verbose = false;
  bool no_redist = false;  // diagnostic only: see the note in main()
  bool verify = false;
  bool serial = false;
};

bool parse(int argc, char** argv, Args& a, int rank) {
  for (int i = 1; i < argc; ++i) {
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        if (rank == 0) fprintf(stderr, "missing value after %s\n", argv[i]);
        return nullptr;
      }
      return argv[++i];
    };
    std::string s = argv[i];
    if (s == "-f" || s == "--filename") { const char* v = next(); if (!v) return false; a.filename = v; }
    else if (s == "-e" || s == "--eps") { const char* v = next(); if (!v) return false; a.eps = atof(v); }
    else if (s == "-b" || s == "--b-factor") { const char* v = next(); if (!v) return false; a.b_factor = atof(v); }
    else if (s == "-m" || s == "--core-min-size") { const char* v = next(); if (!v) return false; a.minpts = atoi(v); }
    else if (s == "-i" || s == "--impl") { const char* v = next(); if (!v) return false; a.impl = v; }
    else if (s == "-N" || s == "--max-particles") { const char* v = next(); if (!v) return false; a.max_particles = atoll(v); }
    else if (s == "--sample-per-rank") { const char* v = next(); if (!v) return false; a.sample_per_rank = atoi(v); }
    else if (s == "--no-census") a.census = false;
    else if (s == "--verify") a.verify = true;
    else if (s == "--serial") a.serial = true;
    else if (s == "-v" || s == "--verbose") a.verbose = true;
    else if (s == "--no-redistribute") a.no_redist = true;
    else if (s == "-h" || s == "--help") {
      if (rank == 0)
        printf("usage: cosmo_fof -f <snapshot> [-b 0.2 | -e <eps>] [-m 2]\n"
               "                 [-i fdbscan|fdbscan-densebox] [-N maxparticles]\n"
               "                 [--sample-per-rank N] [--no-census] [-v]\n");
      return false;
    } else {
      if (rank == 0) fprintf(stderr, "unknown option %s\n", argv[i]);
      return false;
    }
  }
  if (a.filename.empty()) {
    if (rank == 0) fprintf(stderr, "-f <snapshot> is required\n");
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  MPI_Init(&argc, &argv);
  MPI_Comm comm = MPI_COMM_WORLD;
  int rank, nranks;
  MPI_Comm_rank(comm, &rank);
  MPI_Comm_size(comm, &nranks);

  // Line-buffer stdout. Under srun, stdout is a pipe and therefore
  // BLOCK-buffered, so anything printed shortly before an abort is lost with
  // the buffer -- which is how a serial run that had already printed its
  // answer looked like a crash before printing anything, and how the stock
  // ArborX benchmark (which prints nothing on success) looked like a hang.
  // Diagnosing either cost a job apiece.
  setvbuf(stdout, nullptr, _IOLBF, 0);

  Args args;
  if (!parse(argc, argv, args, rank)) { MPI_Finalize(); return 2; }

  Kokkos::initialize(argc, argv);
  {
    using ExecutionSpace = Kokkos::DefaultExecutionSpace;
    using MemorySpace = ExecutionSpace::memory_space;
    ExecutionSpace space{};
    bool serial_done = false;

    if (rank == 0) {
      printf("=== ArborX cosmo FoF ===\n");
      printf("ArborX version    : %s (%s)\n", ArborX::version().c_str(),
             ArborX::gitCommitHash().c_str());
      printf("#MPI ranks        : %d\n", nranks);
      printf("exec space        : %s\n", ExecutionSpace::name());
      printf("snapshot          : %s\n", args.filename.c_str());
      printf("impl              : %s\n", args.impl.c_str());
      printf("minpts            : %d\n", args.minpts);
      fflush(stdout);
    }

    // ---- open + slab read -------------------------------------------------
    cosmoio::Snapshot snap;
    std::string err;
    if (!cosmoio::open_snapshot(args.filename, snap, err))
      die(rank, "open_snapshot", err);

    int64_t n_global = snap.n_total;
    if (args.max_particles > 0 && args.max_particles < n_global)
      n_global = args.max_particles;

    // File-order slab: the same decomposition-free starting point FoF3's
    // Readers get, so neither code is handed a better-than-realistic layout.
    int64_t my_begin = (int64_t)((__int128)n_global * rank / nranks);
    int64_t my_end = (int64_t)((__int128)n_global * (rank + 1) / nranks);
    int64_t n_local = my_end - my_begin;

    if (rank == 0) {
      printf("n_total           : %lld  (gas %lld dark %lld star %lld)\n",
             (long long)snap.n_total, (long long)snap.n_gas,
             (long long)snap.n_dark, (long long)snap.n_star);
      printf("n_used            : %lld  (~%lld per rank)\n",
             (long long)n_global, (long long)(n_global / nranks));
      fflush(stdout);
    }

    MPI_Barrier(comm);
    double t0 = now();
    std::vector<float> xyz((size_t)3 * n_local);
    if (n_local > 0 && !cosmoio::read_pos(snap, my_begin, n_local, xyz.data(), err))
      die(rank, "read_pos", err);
    MPI_Barrier(comm);
    double t_read = now() - t0;

    // ---- global bounding box + linking length ------------------------------
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (int64_t i = 0; i < n_local; ++i)
      for (int d = 0; d < 3; ++d) {
        double v = xyz[3 * i + d];
        if (v < lo[d]) lo[d] = v;
        if (v > hi[d]) hi[d] = v;
      }
    MPI_Allreduce(MPI_IN_PLACE, lo, 3, MPI_DOUBLE, MPI_MIN, comm);
    MPI_Allreduce(MPI_IN_PLACE, hi, 3, MPI_DOUBLE, MPI_MAX, comm);

    double V = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
    double eps = args.eps > 0 ? args.eps
                              : args.b_factor * std::cbrt(V / (double)n_global);
    if (rank == 0) {
      printf("bbox              : [%.9g %.9g %.9g] .. [%.9g %.9g %.9g]\n",
             lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
      printf("bbox volume       : %.17g\n", V);
      printf("eps (b)           : %.17g%s\n", eps,
             args.eps > 0 ? "  [given]" : "  [b_factor * cbrt(V/N)]");
      printf("TIME read_s       : %.3f\n", t_read);
      fflush(stdout);
    }

    // ---- Morton-order redistribution --------------------------------------
    // Equal particle counts per rank, spatially compact regions. Timed
    // separately: this is the analogue of FoF3's decomposition, NOT part of
    // the FoF solve.
    // Checksum of the point multiset BEFORE redistribution. Compared with the
    // same checksum after, this proves the partition neither loses, duplicates
    // nor corrupts a point -- which is exactly the class of bug that would
    // make a component count wrong while leaving every timing plausible.
    auto checksum = [&](const std::vector<float>& v, int64_t n) {
      double s1 = 0, s2 = 0;
      for (int64_t i = 0; i < 3 * n; ++i) {
        double x = v[(size_t)i];
        s1 += x;
        s2 += x * x;
      }
      double out[3] = {s1, s2, (double)n};
      MPI_Allreduce(MPI_IN_PLACE, out, 3, MPI_DOUBLE, MPI_SUM, comm);
      return std::array<double, 3>{out[0], out[1], out[2]};
    };
    auto ck_before = checksum(xyz, n_local);

    MPI_Barrier(comm);
    t0 = now();
    if (!args.no_redist && nranks > 1) {
      // Normalise into [0, 2^21) per axis using the GLOBAL box, so every
      // rank computes the same key for the same point.
      double scale[3];
      const double GRID = 2097152.0;  // 2^21
      for (int d = 0; d < 3; ++d) {
        double w = hi[d] - lo[d];
        scale[d] = (w > 0) ? (GRID - 1.0) / w : 0.0;
      }
      std::vector<uint64_t> key((size_t)n_local);
      for (int64_t i = 0; i < n_local; ++i) {
        uint32_t c[3];
        for (int d = 0; d < 3; ++d) {
          double t = (xyz[3 * i + d] - lo[d]) * scale[d];
          if (t < 0) t = 0;
          if (t > GRID - 1) t = GRID - 1;
          c[d] = (uint32_t)t;
        }
        key[i] = morton3(c[0], c[1], c[2]);
      }

      // Sampled splitters. Sample budget is per-rank so the gather stays
      // bounded: 1024 x 4096 ranks = 4.2M keys = 34 MB on the root.
      int S = std::min<int64_t>(args.sample_per_rank, std::max<int64_t>(n_local, 0));
      std::vector<uint64_t> mysample((size_t)S);
      for (int i = 0; i < S; ++i)
        mysample[i] = key[(int64_t)((double)i * n_local / std::max(S, 1))];

      std::vector<int> scnt(nranks), sdis(nranks);
      MPI_Allgather(&S, 1, MPI_INT, scnt.data(), 1, MPI_INT, comm);
      int64_t stot = 0;
      for (int r = 0; r < nranks; ++r) { sdis[r] = (int)stot; stot += scnt[r]; }
      std::vector<uint64_t> allsample((size_t)std::max<int64_t>(stot, 1));
      MPI_Allgatherv(mysample.data(), S, MPI_UINT64_T, allsample.data(),
                     scnt.data(), sdis.data(), MPI_UINT64_T, comm);
      std::sort(allsample.begin(), allsample.begin() + stot);

      std::vector<uint64_t> split((size_t)std::max(nranks - 1, 1));
      for (int r = 1; r < nranks; ++r)
        split[r - 1] = allsample[(size_t)((__int128)stot * r / nranks)];

      // Assign, then Alltoallv the points. Counting sort by destination.
      std::vector<int> sendc(nranks, 0), recvc(nranks, 0);
      std::vector<int> dest((size_t)n_local);
      for (int64_t i = 0; i < n_local; ++i) {
        int d = (int)(std::upper_bound(split.begin(), split.begin() + (nranks - 1),
                                       key[i]) - split.begin());
        dest[i] = d;
        ++sendc[d];
      }
      std::vector<int> sendd(nranks, 0), recvd(nranks, 0);
      for (int r = 1; r < nranks; ++r) sendd[r] = sendd[r - 1] + sendc[r - 1];
      std::vector<float> sbuf((size_t)3 * n_local);
      {
        std::vector<int> cur = sendd;
        for (int64_t i = 0; i < n_local; ++i) {
          int slot = cur[dest[i]]++;
          sbuf[3 * (size_t)slot + 0] = xyz[3 * i + 0];
          sbuf[3 * (size_t)slot + 1] = xyz[3 * i + 1];
          sbuf[3 * (size_t)slot + 2] = xyz[3 * i + 2];
        }
      }
      key.clear(); key.shrink_to_fit();
      dest.clear(); dest.shrink_to_fit();

      MPI_Alltoall(sendc.data(), 1, MPI_INT, recvc.data(), 1, MPI_INT, comm);
      int64_t n_new = 0;
      for (int r = 0; r < nranks; ++r) { recvd[r] = (int)n_new; n_new += recvc[r]; }
      if (n_new > (int64_t)2147483000)
        die(rank, "redistribute",
            "this rank would own " + std::to_string(n_new) +
                " particles, past the int32 limit ArborX uses for local "
                "indices -- run on more ranks");

      MPI_Datatype F3;
      MPI_Type_contiguous(3, MPI_FLOAT, &F3);
      MPI_Type_commit(&F3);
      std::vector<float> rbuf((size_t)3 * std::max<int64_t>(n_new, 1));
      MPI_Alltoallv(sbuf.data(), sendc.data(), sendd.data(), F3, rbuf.data(),
                    recvc.data(), recvd.data(), F3, comm);
      MPI_Type_free(&F3);
      xyz.swap(rbuf);
      n_local = n_new;
    }
    MPI_Barrier(comm);
    double t_redist = now() - t0;

    {
      auto ck_after = checksum(xyz, n_local);
      // Sums over ~1e10 float32 coordinates in double: reordering the addition
      // changes the low bits, so this is a relative tolerance, not equality.
      bool ok = (ck_before[2] == ck_after[2]) &&
                std::abs(ck_before[0] - ck_after[0]) <=
                    1e-9 * (std::abs(ck_before[0]) + 1.0) &&
                std::abs(ck_before[1] - ck_after[1]) <=
                    1e-9 * (std::abs(ck_before[1]) + 1.0);
      if (rank == 0) {
        printf("REDIST_CHECK %s  n %.0f -> %.0f  sum %.17g -> %.17g  "
               "sumsq %.17g -> %.17g\n",
               ok ? "PASS" : "FAIL", ck_before[2], ck_after[2], ck_before[0],
               ck_after[0], ck_before[1], ck_after[1]);
        fflush(stdout);
      }
      if (!ok) die(rank, "redistribute", "point multiset changed");
    }

    // Report the imbalance the partition actually achieved: ArborX's per-rank
    // memory and its local DBSCAN cost both scale with it, so a claim about
    // ArborX's scaling is not interpretable without it.
    {
      long long mn = n_local, mx = n_local, sum = n_local;
      MPI_Allreduce(MPI_IN_PLACE, &mn, 1, MPI_LONG_LONG, MPI_MIN, comm);
      MPI_Allreduce(MPI_IN_PLACE, &mx, 1, MPI_LONG_LONG, MPI_MAX, comm);
      MPI_Allreduce(MPI_IN_PLACE, &sum, 1, MPI_LONG_LONG, MPI_SUM, comm);
      if (rank == 0) {
        printf("TIME redist_s     : %.3f\n", t_redist);
        printf("per-rank n        : min %lld max %lld mean %.0f imbalance %.3f\n",
               mn, mx, (double)sum / nranks,
               (double)mx / ((double)sum / nranks));
        if (sum != n_global)
          printf("WARNING census    : redistribution moved %lld particles but "
                 "%lld were read\n", sum, (long long)n_global);
        fflush(stdout);
      }
    }

    // Is the partition actually SPATIALLY COMPACT? This is the property
    // ArborX's distributed DBSCAN depends on and the whole reason the
    // redistribution exists, so it is measured rather than assumed. The
    // number to watch is the mean per-rank box diagonal as a fraction of the
    // global diagonal: with a working Morton partition on P ranks it should
    // be around P^(-1/3); at 1.0 the partition has scattered the points and
    // every rank spans the whole domain.
    {
      double blo[3] = {1e300, 1e300, 1e300}, bhi[3] = {-1e300, -1e300, -1e300};
      for (int64_t i = 0; i < n_local; ++i)
        for (int d = 0; d < 3; ++d) {
          double v = xyz[3 * i + d];
          if (v < blo[d]) blo[d] = v;
          if (v > bhi[d]) bhi[d] = v;
        }
      double diag = 0, gdiag = 0;
      for (int d = 0; d < 3; ++d) {
        double e = (n_local > 0) ? (bhi[d] - blo[d]) : 0.0;
        diag += e * e;
        gdiag += (hi[d] - lo[d]) * (hi[d] - lo[d]);
      }
      diag = std::sqrt(diag);
      gdiag = std::sqrt(gdiag);
      double sum = diag, mx = diag;
      MPI_Allreduce(MPI_IN_PLACE, &sum, 1, MPI_DOUBLE, MPI_SUM, comm);
      MPI_Allreduce(MPI_IN_PLACE, &mx, 1, MPI_DOUBLE, MPI_MAX, comm);
      if (rank == 0)
        printf("LOCALITY mean_box_diag/global %.4f  max %.4f  ideal ~%.4f "
               "(= P^-1/3)\n",
               sum / nranks / gdiag, mx / gdiag,
               std::pow((double)nranks, -1.0 / 3.0));
      fflush(stdout);
    }

    // ---- to device ---------------------------------------------------------
    Kokkos::View<Point*, MemorySpace> points(
        Kokkos::view_alloc(space, Kokkos::WithoutInitializing, "cosmo::points"),
        n_local);
    {
      Kokkos::View<Point*, Kokkos::HostSpace,
                   Kokkos::MemoryTraits<Kokkos::Unmanaged>>
          host(reinterpret_cast<Point*>(xyz.data()), n_local);
      Kokkos::deep_copy(space, points, host);
      space.fence();
    }
    // DEVICE-SIDE checksum of `points`, reduced on the device and compared
    // with the host checksum of `xyz`. Every probe so far has validated the
    // HOST buffer; none has validated what ArborX actually searches. If the
    // device view disagrees, the reinterpret_cast staging or the async copy
    // is at fault and nothing downstream can be trusted.
    {
      double dsum = 0, dsumsq = 0;
      Kokkos::parallel_reduce(
          "cosmo::device_checksum", Kokkos::RangePolicy(space, 0, n_local),
          KOKKOS_LAMBDA(int i, double &s1, double &s2) {
            for (int d = 0; d < 3; ++d) {
              double x = points(i)[d];
              s1 += x;
              s2 += x * x;
            }
          },
          dsum, dsumsq);
      space.fence();
      double hsum = 0, hsumsq = 0;
      for (int64_t i = 0; i < 3 * n_local; ++i) {
        double x = xyz[(size_t)i];
        hsum += x;
        hsumsq += x * x;
      }
      double g[4] = {dsum, dsumsq, hsum, hsumsq};
      MPI_Allreduce(MPI_IN_PLACE, g, 4, MPI_DOUBLE, MPI_SUM, comm);
      if (rank == 0) {
        bool ok = std::abs(g[0] - g[2]) <= 1e-6 * (std::abs(g[2]) + 1.0) &&
                  std::abs(g[1] - g[3]) <= 1e-6 * (std::abs(g[3]) + 1.0);
        printf("DEVICE_CHECK %s  dev_sum %.17g host_sum %.17g  "
               "dev_sumsq %.17g host_sumsq %.17g\n",
               ok ? "PASS" : "FAIL", g[0], g[2], g[1], g[3]);
      }
    }

    xyz.clear(); xyz.shrink_to_fit();

    // ---- SERIAL BISECTION -------------------------------------------------
    // ArborX's NON-distributed dbscan on the whole point set, in one rank.
    // This shares my reader, my eps and my census but NONE of the
    // distributed layer, so it decides where the wrong component counts come
    // from: if this returns FoF3's answer then reader, linking length and
    // census are all correct and only ArborX::Experimental::dbscan (the
    // distributed wrapper) is at fault.
    if (args.serial) {
      if (nranks != 1)
        die(rank, "--serial", "must be run with exactly one MPI rank");
      ArborX::DBSCAN::Parameters sp;
      // MUST set the implementation explicitly. DBSCAN::Parameters defaults
      // `_implementation` to FDBSCAN_DenseBox, NOT FDBSCAN, so leaving it
      // alone would have the serial bisection run a DIFFERENT algorithm from
      // the distributed path it is supposed to be bisecting against.
      sp.setVerbosity(args.verbose)
          .setImplementation(args.impl == "fdbscan-densebox"
                                 ? ArborX::DBSCAN::Implementation::FDBSCAN_DenseBox
                                 : ArborX::DBSCAN::Implementation::FDBSCAN);
      printf("SERIAL impl %s  points %lld\n", args.impl.c_str(),
             (long long)points.size());
      fflush(stdout);
      MPI_Barrier(comm);
      double ts = now();
      // Non-deprecated overload (labels passed in). The deprecated
      // return-by-value form was used first and is what produced an
      // unexplained std::length_error; passing the view in removes the
      // overload ambiguity as a variable entirely.
      Kokkos::View<int *, MemorySpace> slabels("cosmo::serial_labels", 0);
      ArborX::dbscan(space, points, (double)eps, args.minpts, slabels, sp);
      space.fence();
      double t_serial = now() - ts;
      printf("SERIAL labels_extent %lld\n", (long long)slabels.extent(0));
      fflush(stdout);
      auto h = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, slabels);
      printf("SERIAL mirror_extent %lld\n", (long long)h.extent(0));
      fflush(stdout);
      long long noise = 0;
      std::vector<long long> v;
      // Markers at every step: a std::length_error was being thrown somewhere
      // in this block even though the only allocation is a reserve of 100000
      // long longs, which cannot exceed max_size(). Narrow it by printing
      // between each statement rather than reasoning about it further.
      printf("SERIAL m1 about to reserve %zu\n", (size_t)h.extent(0));
      fflush(stdout);
      try {
        v.reserve((size_t)h.extent(0));
      } catch (std::exception const &e) {
        printf("SERIAL EXC in reserve: %s\n", e.what());
        fflush(stdout);
        throw;
      }
      printf("SERIAL m2 reserved, capacity %zu\n", v.capacity());
      fflush(stdout);
      try {
        for (size_t i = 0; i < h.extent(0); ++i) {
          if (h(i) < 0) ++noise;
          else v.push_back((long long)h(i));
        }
      } catch (std::exception const &e) {
        printf("SERIAL EXC in loop at size %zu: %s\n", v.size(), e.what());
        fflush(stdout);
        throw;
      }
      printf("SERIAL m3 collected %zu labels, %lld noise\n", v.size(), noise);
      fflush(stdout);
      std::sort(v.begin(), v.end());
      printf("SERIAL m4 sorted\n");
      fflush(stdout);
      long long distinct = 0, mx = 0;
      for (size_t i = 0; i < v.size();) {
        size_t j = i;
        while (j < v.size() && v[j] == v[i]) ++j;
        ++distinct;
        if ((long long)(j - i) > mx) mx = (long long)(j - i);
        i = j;  // MUST advance. Without it this is an infinite loop whose only
                // effects are on locals, i.e. UB, and the compiler elided the
                // whole loop -- leaving distinct = mx = 0 and reporting
                // "groups_multi: 0, max_size 1" for a perfectly good set of
                // labels. It also manifested earlier as an unexplained
                // std::length_error, which is what UB is entitled to do.
      }
      printf("SERIAL dbscan_s %.3f  n_local %lld  labels %lld\n", t_serial,
             (long long)n_local, (long long)h.extent(0));
      printf("SERIALSTAT groups_multi: %lld  isolated: %lld\n", distinct, noise);
      printf("SERIALSTAT components: %lld max_size %lld\n", distinct + noise,
             std::max(mx, noise > 0 ? 1LL : 0LL));
      fflush(stdout);
      // Fall through to the end of this scope rather than calling
      // Kokkos::finalize() here: `points`, `slabels` and `h` are still alive
      // at this point, and destroying a Kokkos View after finalize() is an
      // error in its own right (it was masking whatever the real failure was).
      serial_done = true;
    }
    if (!serial_done) {

    // ---- DBSCAN -----------------------------------------------------------
    ArborX::DBSCAN::Parameters p;
    p.setVerbosity(args.verbose)
        .setImplementation(args.impl == "fdbscan-densebox"
                               ? ArborX::DBSCAN::Implementation::FDBSCAN_DenseBox
                               : ArborX::DBSCAN::Implementation::FDBSCAN);

    Kokkos::View<long long*, MemorySpace> labels("cosmo::labels", 0);
    MPI_Barrier(comm);
    t0 = now();
    ArborX::Experimental::dbscan(comm, space, points, (float)eps, args.minpts,
                                 labels, p);
    space.fence();
    MPI_Barrier(comm);
    double t_dbscan = now() - t0;
    if (rank == 0) {
      printf("TIME dbscan_s     : %.3f\n", t_dbscan);
      fflush(stdout);
    }

    // ArborX's OWN verifier, on the labels ArborX just produced. This is the
    // check that separates "my census misreads correct labels" from "the
    // labels are wrong": it recomputes the neighbourhood relation directly
    // and asserts every label is consistent with it.
    if (args.verify) {
      MPI_Barrier(comm);
      t0 = now();
      bool ok = ArborX::Details::verifyDBSCAN(comm, space, points, (float)eps,
                                              args.minpts, labels,
                                              args.verbose);
      int all_ok = ok ? 1 : 0;
      MPI_Allreduce(MPI_IN_PLACE, &all_ok, 1, MPI_INT, MPI_MIN, comm);
      MPI_Barrier(comm);
      if (rank == 0) {
        printf("ARBORX_VERIFY %s  (%.3f s)\n", all_ok ? "PASS" : "FAIL",
               now() - t0);
        fflush(stdout);
      }
    }

    // ---- component census -------------------------------------------------
    // Labels are global ids of a representative particle, or -1 for a point
    // with no neighbour within eps ("noise" in DBSCAN's vocabulary, an
    // isolated particle in FoF's). Count distinct labels and their sizes by
    // hashing each label to an owner rank, so nothing is gathered anywhere.
    if (args.census) {
      MPI_Barrier(comm);
      t0 = now();
      auto h_labels = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{},
                                                          labels);
      long long n_noise = 0;
      std::vector<int> sendc(nranks, 0), recvc(nranks, 0), sendd(nranks, 0),
          recvd(nranks, 0);
      auto owner = [&](long long L) {
        // splitmix64, so adjacent representative ids do not pile onto one rank
        uint64_t z = (uint64_t)L + 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        z ^= z >> 31;
        return (int)(z % (uint64_t)nranks);
      };
      int64_t nl = (int64_t)h_labels.extent(0);
      for (int64_t i = 0; i < nl; ++i) {
        if (h_labels(i) < 0) { ++n_noise; continue; }
        ++sendc[owner(h_labels(i))];
      }
      for (int r = 1; r < nranks; ++r) sendd[r] = sendd[r - 1] + sendc[r - 1];
      int64_t n_send = sendd[nranks - 1] + sendc[nranks - 1];
      std::vector<long long> sb((size_t)std::max<int64_t>(n_send, 1));
      {
        std::vector<int> cur = sendd;
        for (int64_t i = 0; i < nl; ++i)
          if (h_labels(i) >= 0) sb[cur[owner(h_labels(i))]++] = h_labels(i);
      }
      MPI_Alltoall(sendc.data(), 1, MPI_INT, recvc.data(), 1, MPI_INT, comm);
      int64_t n_recv = 0;
      for (int r = 0; r < nranks; ++r) { recvd[r] = (int)n_recv; n_recv += recvc[r]; }
      std::vector<long long> rb((size_t)std::max<int64_t>(n_recv, 1));
      MPI_Alltoallv(sb.data(), sendc.data(), sendd.data(), MPI_LONG_LONG,
                    rb.data(), recvc.data(), recvd.data(), MPI_LONG_LONG, comm);
      std::sort(rb.begin(), rb.begin() + n_recv);
      long long n_distinct = 0, my_max = 0;
      for (int64_t i = 0; i < n_recv;) {
        int64_t j = i;
        while (j < n_recv && rb[j] == rb[i]) ++j;
        ++n_distinct;
        if (j - i > my_max) my_max = j - i;
        i = j;
      }
      long long tot_distinct = 0, max_size = 0, tot_noise = 0, tot_labeled = 0;
      long long my_labeled = n_recv;
      MPI_Reduce(&n_distinct, &tot_distinct, 1, MPI_LONG_LONG, MPI_SUM, 0, comm);
      MPI_Reduce(&my_max, &max_size, 1, MPI_LONG_LONG, MPI_MAX, 0, comm);
      MPI_Reduce(&n_noise, &tot_noise, 1, MPI_LONG_LONG, MPI_SUM, 0, comm);
      MPI_Reduce(&my_labeled, &tot_labeled, 1, MPI_LONG_LONG, MPI_SUM, 0, comm);
      MPI_Barrier(comm);
      double t_census = now() - t0;
      if (rank == 0) {
        long long with = tot_distinct + tot_noise;
        printf("TIME census_s     : %.3f\n", t_census);
        // SELF-CHECK: every particle must be accounted for exactly once --
        // either as noise or inside one labelled group. If this fails the
        // census is misreading the labels and its component count means
        // nothing, independently of whether ArborX's labels are right.
        printf("CENSUS_CHECK %s  labeled %lld + noise %lld = %lld, n %lld\n",
               (tot_labeled + tot_noise == (long long)n_global) ? "PASS" : "FAIL",
               tot_labeled, tot_noise, tot_labeled + tot_noise,
               (long long)n_global);
        printf("ARBORXSTAT groups_multi: %lld  isolated: %lld\n", tot_distinct,
               tot_noise);
        printf("ARBORXSTAT components: %lld max_size %lld"
               "   [components counts isolated particles; groups_multi does not]\n",
               with, std::max(max_size, tot_noise > 0 ? 1LL : 0LL));
        fflush(stdout);
      }
    }

    if (rank == 0) {
      printf("ARBORXRESULT ranks=%d n=%lld eps=%.17g read_s=%.3f "
             "redist_s=%.3f dbscan_s=%.3f\n",
             nranks, (long long)n_global, eps, t_read, t_redist, t_dbscan);
      fflush(stdout);
    }
    }  // !serial_done
  }
  Kokkos::finalize();
  MPI_Finalize();
  return 0;
}
