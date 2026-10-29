// nc2swift -- convert a Tipsy or NChilada cosmology snapshot into a single
// SWIFT-readable HDF5 initial-conditions file, in parallel.
//
// WHY A CONVERSION AT ALL. SWIFT reads ICs only through HDF5 (single_io.c /
// serial_io.c / parallel_io.c); there is no pluggable reader. Teaching
// read_ic_parallel a new on-disk format would mean reimplementing its
// slab-read + redistribute path, which is the part of SWIFT most entangled
// with the rest of the engine. Converting once per snapshot is both less
// invasive and leaves SWIFT itself entirely UNPATCHED, which matters for a
// published comparison.
//
// WHAT IS WRITTEN, AND THE ONE MODELLING DECISION IN IT.
// Every particle of every family (gas, dark, star) is written as a single
// species, PartType1 (dark matter). That is deliberate and it is what makes
// the comparison apples-to-apples: FoF3 and ArborX both treat the snapshot
// as one undifferentiated set of points, whereas SWIFT's FOF distinguishes
// "linkable" types (DM, which can form groups) from "attachable" ones (gas
// and stars, which are only attached to a group found by the linkables).
// Loading everything as PartType1 makes all N particles linkable, so all
// three codes solve the identical problem. Writing the true families instead
// would have SWIFT solve FOF over the 1/3 of particles that are DM and then
// attach the rest -- a different, cheaper problem, and not the one FoF3 is
// timed on.
//
// Fields written are exactly the four SWIFT's darkmatter_read_particles
// declares COMPULSORY:
//   Coordinates  (N,3) float32 -- SWIFT asks H5Dread for DOUBLE, and HDF5
//                converts on read, so storing float32 halves the file and
//                loses nothing: the source data IS float32.
//   Velocities   (N,3) float32, ZERO. FOF never reads velocities. They are
//                written rather than made OPTIONAL in SWIFT's reader
//                precisely so that SWIFT stays unpatched.
//   Masses       (N)   float32 from the snapshot.
//   ParticleIDs  (N)   uint64, the global particle index + 1.
//
// Coordinates are translated so the minimum corner sits at the origin
// (SWIFT requires 0 <= x < BoxSize) and BoxSize is the largest axis extent
// with a 1e-6 relative margin. Translation is exact in float32 here only
// because it is done in double and rounded once; FoF is translation
// invariant, so this cannot change the answer.

#include <hdf5.h>
#include <mpi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cosmoio.hpp"

#define CHK(expr, what)                                                       \
  do {                                                                        \
    if ((expr) < 0) {                                                          \
      fprintf(stderr, "[%d] HDF5 error at %s:%d: %s\n", rank, __FILE__,        \
              __LINE__, what);                                                 \
      MPI_Abort(MPI_COMM_WORLD, 1);                                            \
    }                                                                          \
  } while (0)

static int rank = 0, nranks = 1;

static void die(const char* what, const std::string& why) {
  fprintf(stderr, "[%d] FATAL %s: %s\n", rank, what, why.c_str());
  fflush(stderr);
  MPI_Abort(MPI_COMM_WORLD, 1);
}

// Attributes are file metadata, so under H5Pset_fapl_mpio every rank must
// create and write them with identical values.
static void attr_d(hid_t g, const char* name, const double* v, int n) {
  hsize_t d = (hsize_t)n;
  hid_t sp = (n == 1) ? H5Screate(H5S_SCALAR) : H5Screate_simple(1, &d, NULL);
  hid_t a = H5Acreate(g, name, H5T_NATIVE_DOUBLE, sp, H5P_DEFAULT, H5P_DEFAULT);
  CHK(a, name);
  CHK(H5Awrite(a, H5T_NATIVE_DOUBLE, v), name);
  H5Aclose(a); H5Sclose(sp);
}
static void attr_i(hid_t g, const char* name, const int* v, int n) {
  hsize_t d = (hsize_t)n;
  hid_t sp = (n == 1) ? H5Screate(H5S_SCALAR) : H5Screate_simple(1, &d, NULL);
  hid_t a = H5Acreate(g, name, H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT);
  CHK(a, name);
  CHK(H5Awrite(a, H5T_NATIVE_INT, v), name);
  H5Aclose(a); H5Sclose(sp);
}
static void attr_ll(hid_t g, const char* name, const long long* v, int n) {
  hsize_t d = (hsize_t)n;
  hid_t sp = H5Screate_simple(1, &d, NULL);
  hid_t a = H5Acreate(g, name, H5T_NATIVE_LLONG, sp, H5P_DEFAULT, H5P_DEFAULT);
  CHK(a, name);
  CHK(H5Awrite(a, H5T_NATIVE_LLONG, v), name);
  H5Aclose(a); H5Sclose(sp);
}

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  MPI_Comm comm = MPI_COMM_WORLD;
  MPI_Comm_rank(comm, &rank);
  MPI_Comm_size(comm, &nranks);

  std::string in, out;
  int64_t max_particles = -1;
  int64_t chunk = 1 << 24;  // particles per collective write call
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    if (s == "-i" && i + 1 < argc) in = argv[++i];
    else if (s == "-o" && i + 1 < argc) out = argv[++i];
    else if (s == "-N" && i + 1 < argc) max_particles = atoll(argv[++i]);
    else if (s == "--chunk" && i + 1 < argc) chunk = atoll(argv[++i]);
    else {
      if (rank == 0) fprintf(stderr, "usage: nc2swift -i <snapshot> -o <out.hdf5> [-N max] [--chunk n]\n");
      MPI_Finalize(); return 2;
    }
  }
  if (in.empty() || out.empty()) {
    if (rank == 0) fprintf(stderr, "-i and -o are required\n");
    MPI_Finalize(); return 2;
  }

  cosmoio::Snapshot snap;
  std::string err;
  if (!cosmoio::open_snapshot(in, snap, err)) die("open_snapshot", err);

  int64_t N = snap.n_total;
  if (max_particles > 0 && max_particles < N) N = max_particles;

  int64_t beg = (int64_t)((__int128)N * rank / nranks);
  int64_t end = (int64_t)((__int128)N * (rank + 1) / nranks);
  int64_t n_local = end - beg;

  if (rank == 0) {
    printf("nc2swift: %s -> %s\n", in.c_str(), out.c_str());
    printf("  ranks %d, N %lld (gas %lld dark %lld star %lld), ~%lld/rank\n",
           nranks, (long long)N, (long long)snap.n_gas,
           (long long)snap.n_dark, (long long)snap.n_star,
           (long long)(N / nranks));
    fflush(stdout);
  }

  double t0 = MPI_Wtime();
  std::vector<float> xyz((size_t)3 * std::max<int64_t>(n_local, 1));
  std::vector<float> mass((size_t)std::max<int64_t>(n_local, 1));
  if (n_local > 0) {
    if (!cosmoio::read_pos(snap, beg, n_local, xyz.data(), err))
      die("read_pos", err);
    if (!cosmoio::read_mass(snap, beg, n_local, mass.data(), err))
      die("read_mass", err);
  }
  MPI_Barrier(comm);
  double t_read = MPI_Wtime() - t0;

  // Global bounding box -> translation and BoxSize.
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (int64_t i = 0; i < n_local; ++i)
    for (int d = 0; d < 3; ++d) {
      double v = xyz[3 * i + d];
      if (v < lo[d]) lo[d] = v;
      if (v > hi[d]) hi[d] = v;
    }
  MPI_Allreduce(MPI_IN_PLACE, lo, 3, MPI_DOUBLE, MPI_MIN, comm);
  MPI_Allreduce(MPI_IN_PLACE, hi, 3, MPI_DOUBLE, MPI_MAX, comm);
  double L = 0;
  for (int d = 0; d < 3; ++d) L = std::max(L, hi[d] - lo[d]);
  L *= (1.0 + 1e-6);
  if (rank == 0) {
    printf("  bbox [%.9g %.9g %.9g] .. [%.9g %.9g %.9g]\n", lo[0], lo[1], lo[2],
           hi[0], hi[1], hi[2]);
    printf("  BoxSize %.17g   (translation -lo, so 0 <= x < BoxSize)\n", L);
    printf("  read+mass %.1f s\n", t_read);
    fflush(stdout);
  }
  for (int64_t i = 0; i < n_local; ++i)
    for (int d = 0; d < 3; ++d)
      xyz[3 * i + d] = (float)((double)xyz[3 * i + d] - lo[d]);

  // ---- create the file --------------------------------------------------
  t0 = MPI_Wtime();
  MPI_Info info;
  MPI_Info_create(&info);
  MPI_Info_set(info, "striping_factor", "64");
  MPI_Info_set(info, "romio_cb_write", "enable");
  hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
  CHK(H5Pset_fapl_mpio(fapl, comm, info), "set_fapl_mpio");
  hid_t f = H5Fcreate(out.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
  CHK(f, "H5Fcreate");
  H5Pclose(fapl);
  MPI_Info_free(&info);

  {
    hid_t g = H5Gcreate(f, "/Header", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    CHK(g, "create /Header");
    // Arrays MUST be swift_type_count (7) long: read_ic_parallel indexes
    // NumPart_Total[ptype] for every ptype, so a 6-long attribute would
    // leave the neutrino entry uninitialised.
    const int NT = 7;
    long long npart[NT] = {0}, nhigh[NT] = {0}, nthis[NT] = {0};
    npart[1] = (long long)(N & 0xFFFFFFFFLL);   // Gadget convention: low word
    nhigh[1] = (long long)(N >> 32);            // ... and high word
    nthis[1] = npart[1];
    attr_ll(g, "NumPart_Total", npart, NT);
    attr_ll(g, "NumPart_Total_HighWord", nhigh, NT);
    attr_ll(g, "NumPart_ThisFile", nthis, NT);
    int flag[NT] = {0};
    attr_i(g, "Flag_Entropy_ICs", flag, NT);
    double box[3] = {L, L, L};
    attr_d(g, "BoxSize", box, 3);
    int dim = 3, nfiles = 1;
    attr_i(g, "Dimension", &dim, 1);
    attr_i(g, "NumFilesPerSnapshot", &nfiles, 1);
    double t = snap.time, z = 0.0;
    attr_d(g, "Time", &t, 1);
    attr_d(g, "Redshift", &z, 1);
    H5Gclose(g);
  }
  {
    // Unit system. These snapshots are in code units whose physical scale is
    // irrelevant to FOF: the linking length is passed to SWIFT as
    // FOF:absolute_linking_length in these same units, so declaring the
    // cgs factors as 1 keeps SWIFT's internal units identical to the file's
    // and no conversion is applied to any coordinate.
    hid_t g = H5Gcreate(f, "/Units", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    CHK(g, "create /Units");
    double one = 1.0;
    attr_d(g, "Unit length in cgs (U_L)", &one, 1);
    attr_d(g, "Unit mass in cgs (U_M)", &one, 1);
    attr_d(g, "Unit time in cgs (U_t)", &one, 1);
    attr_d(g, "Unit current in cgs (U_I)", &one, 1);
    attr_d(g, "Unit temperature in cgs (U_T)", &one, 1);
    H5Gclose(g);
  }

  hid_t pg = H5Gcreate(f, "/PartType1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  CHK(pg, "create /PartType1");

  auto make = [&](const char* name, hid_t type, int ncomp) {
    hsize_t dims[2] = {(hsize_t)N, (hsize_t)ncomp};
    hid_t sp = H5Screate_simple(ncomp == 1 ? 1 : 2, dims, NULL);
    hid_t d = H5Dcreate(pg, name, type, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    CHK(d, name);
    H5Sclose(sp);
    return d;
  };
  hid_t d_pos = make("Coordinates", H5T_IEEE_F32LE, 3);
  hid_t d_vel = make("Velocities", H5T_IEEE_F32LE, 3);
  hid_t d_mas = make("Masses", H5T_IEEE_F32LE, 1);
  hid_t d_ids = make("ParticleIDs", H5T_STD_U64LE, 1);

  hid_t dxpl = H5Pcreate(H5P_DATASET_XFER);
  CHK(H5Pset_dxpl_mpio(dxpl, H5FD_MPIO_COLLECTIVE), "set_dxpl_mpio");

  // Every collective call must be entered by every rank the same number of
  // times, so the loop count is the GLOBAL maximum, and ranks that have run
  // out of particles issue zero-length selections.
  int64_t my_chunks = (n_local + chunk - 1) / chunk;
  int64_t max_chunks = my_chunks;
  MPI_Allreduce(MPI_IN_PLACE, &max_chunks, 1, MPI_LONG_LONG, MPI_MAX, comm);
  if (max_chunks == 0) max_chunks = 1;

  std::vector<float> zeros;
  std::vector<unsigned long long> ids;

  for (int64_t c = 0; c < max_chunks; ++c) {
    int64_t off = c * chunk;
    int64_t m = (off < n_local) ? std::min(chunk, n_local - off) : 0;

    auto wr = [&](hid_t dset, hid_t mtype, int ncomp, const void* buf) {
      hsize_t start[2] = {(hsize_t)(beg + off), 0};
      hsize_t count[2] = {(hsize_t)m, (hsize_t)ncomp};
      hid_t fsp = H5Dget_space(dset);
      if (m > 0)
        H5Sselect_hyperslab(fsp, H5S_SELECT_SET, start, NULL, count, NULL);
      else
        H5Sselect_none(fsp);
      hsize_t mdims[2] = {(hsize_t)m, (hsize_t)ncomp};
      hid_t msp = H5Screate_simple(ncomp == 1 ? 1 : 2, mdims, NULL);
      if (m == 0) H5Sselect_none(msp);
      CHK(H5Dwrite(dset, mtype, msp, fsp, dxpl, buf), "H5Dwrite");
      H5Sclose(msp); H5Sclose(fsp);
    };

    if (m > (int64_t)zeros.size() / 3) zeros.assign((size_t)3 * m, 0.0f);
    if (m > (int64_t)ids.size()) ids.resize((size_t)m);
    for (int64_t i = 0; i < m; ++i)
      ids[i] = (unsigned long long)(beg + off + i) + 1ULL;

    wr(d_pos, H5T_NATIVE_FLOAT, 3, m ? xyz.data() + 3 * off : nullptr);
    wr(d_vel, H5T_NATIVE_FLOAT, 3, m ? zeros.data() : nullptr);
    wr(d_mas, H5T_NATIVE_FLOAT, 1, m ? mass.data() + off : nullptr);
    wr(d_ids, H5T_NATIVE_ULLONG, 1, m ? ids.data() : nullptr);
  }

  H5Pclose(dxpl);
  H5Dclose(d_pos); H5Dclose(d_vel); H5Dclose(d_mas); H5Dclose(d_ids);
  H5Gclose(pg);
  H5Fclose(f);
  MPI_Barrier(comm);
  double t_write = MPI_Wtime() - t0;

  if (rank == 0) {
    double gb = (double)N * (12 + 12 + 4 + 8) / 1e9;
    printf("  wrote %.1f GB in %.1f s (%.2f GB/s)\n", gb, t_write, gb / t_write);
    printf("NC2SWIFT done N=%lld boxsize=%.17g read_s=%.1f write_s=%.1f\n",
           (long long)N, L, t_read, t_write);
    fflush(stdout);
  }
  MPI_Finalize();
  return 0;
}
