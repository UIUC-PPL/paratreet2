// cosmoio.hpp -- read particle POSITIONS out of the two on-disk formats this
// campaign compares against (Tipsy and NChilada), by GLOBAL particle range,
// with 64-bit offsets throughout.
//
// Shared by the ArborX driver (which needs xyz only) and the SWIFT converter
// (which needs xyz + mass). Deliberately standalone: it does NOT pull in
// paratreet/utility, so the comparison codes cannot accidentally inherit a
// paratreet behaviour and thereby stop being an independent check.
//
// WIRE FACTS, verified against paratreet2/utility/structures and against the
// exact byte sizes of the three snapshots on disk (see notes at each field):
//
// TIPSY (a single file). The snapshots here are XDR, i.e. BIG-ENDIAN.
//   preface  = 28-byte header + 4-byte XDR pad = 32 bytes
//     header = double time; int nbodies, ndim, nsph, ndark, nstar
//   then nsph gas records, nsph+ndark dark records, nstar star records:
//     gas  = 12 floats = 48 B: mass, pos[3], vel[3], rho, temp, hsmooth, metals, phi
//     dark =  9 floats = 36 B: mass, pos[3], vel[3], eps, phi
//     star = 11 floats = 44 B: mass, pos[3], vel[3], metals, tform, eps, phi
//   so pos sits at byte offset 4 inside every record, and mass at offset 0.
//   CHECK: cosmo25cmb.768g2_dm.001024 is 76780929056 B and
//     32 + 452984832*48 + 1528823808*36 == 76780929056 exactly, which is what
//     pins the preface at 32 (not 28) and the precision at single.
//
// NCHILADA (a DIRECTORY of per-attribute, per-family field files).
//   <dir>/{gas,dark,star}/{pos,mass,...}; a family is absent when empty.
//   Each field file is XDR (BIG-ENDIAN):
//     FieldHeader (28 B) = int magic(1062053); double time; uint64 numParticles;
//                          uint32 dimensions; int32 typecode
//     then min value, then max value, each `dimensions` words of that type,
//     then numParticles values.
//   So for pos (dimensions 3, float32) data starts at 28 + 12 + 12 = 52.
//   CHECK: cosmo25PLK dark/pos is 146767085620 B = 52 + 12230590464*12.
//   A field whose min == max MAY omit the per-particle data entirely; that is
//   detected here (by file size) and reported as an error rather than read
//   through, because a constant `pos` would mean a corrupt snapshot.
//
// GLOBAL ORDERING is gas, then dark, then star, in both formats -- the Tipsy
// convention, and the one paratreet's Reader and ChaNGa's loadNChilada both
// assume. Particle i means the same particle to every code in this campaign
// only because all of them use this ordering.

#ifndef COSMOIO_HPP
#define COSMOIO_HPP

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cosmoio {

enum class Format { Tipsy, NChilada };

// ---- big-endian decode ----------------------------------------------------
// Every value in both formats is a 4- or 8-byte big-endian word.
inline uint32_t be32(const void* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline uint64_t be64(const void* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}
inline float bef32(const void* p) {
  uint32_t v = be32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline double bef64(const void* p) {
  uint64_t v = be64(p);
  double d;
  std::memcpy(&d, &v, 8);
  return d;
}

// NChilada type codes (utility/structures/TypeHandling.h)
enum NCType { NC_INT8 = 1, NC_UINT8, NC_INT16, NC_UINT16, NC_INT32,
              NC_UINT32, NC_INT64, NC_UINT64, NC_FLOAT32, NC_FLOAT64 };

inline int nc_word_size(int code) {
  switch (code) {
    // XDR pads every integer narrower than 32 bits out to a 4-byte word.
    case NC_INT8: case NC_UINT8: case NC_INT16: case NC_UINT16:
    case NC_INT32: case NC_UINT32: case NC_FLOAT32: return 4;
    case NC_INT64: case NC_UINT64: case NC_FLOAT64: return 8;
    default: return 0;
  }
}

/// One (family, attribute) field file of an NChilada snapshot.
struct NCField {
  std::string path;
  int64_t n = 0;         // numParticles from the header
  int64_t data_off = 0;  // byte offset of element 0
  int dims = 0;
  int code = 0;
  int word = 0;          // bytes per word
  bool present = false;
};

/// A snapshot opened for reading. Cheap to copy; holds no file descriptors.
struct Snapshot {
  Format fmt = Format::Tipsy;
  std::string path;
  double time = 0.0;
  int64_t n_gas = 0, n_dark = 0, n_star = 0, n_total = 0;

  // Tipsy
  int64_t preface = 32;
  int gas_rec = 48, dark_rec = 36, star_rec = 44;
  int pos_in_rec = 4, mass_in_rec = 0;
  int pos_word = 4;  // 4 = single-precision positions, 8 = double

  // NChilada: index 0 = gas, 1 = dark, 2 = star
  NCField pos[3], mass[3];
};

// ---- low-level helpers ----------------------------------------------------

inline bool full_pread(int fd, void* buf, size_t n, int64_t off,
                       std::string& err) {
  char* p = static_cast<char*>(buf);
  while (n > 0) {
    ssize_t got = ::pread(fd, p, n, off);
    if (got < 0) {
      if (errno == EINTR) continue;
      err = std::string("pread failed: ") + std::strerror(errno);
      return false;
    }
    if (got == 0) {
      err = "pread hit end of file short of the requested range";
      return false;
    }
    p += got; n -= (size_t)got; off += got;
  }
  return true;
}

inline bool is_dir(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline int64_t file_size(const std::string& p) {
  struct stat st;
  if (::stat(p.c_str(), &st) != 0) return -1;
  return (int64_t)st.st_size;
}

// ---- NChilada -------------------------------------------------------------

/// Read and validate one field-file header. Absent file -> present=false,
/// which is a legitimate answer (an empty family, or an attribute the
/// snapshot does not carry).
inline bool nc_open_field(const std::string& path, int want_dims, NCField& f,
                          std::string& err) {
  f = NCField{};
  f.path = path;
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return true;  // absent: present stays false
  unsigned char h[28];
  bool ok = full_pread(fd, h, 28, 0, err);
  ::close(fd);
  if (!ok) { err = path + ": " + err; return false; }

  int32_t magic = (int32_t)be32(h + 0);
  if (magic != 1062053) {
    err = path + ": bad NChilada magic number " + std::to_string(magic);
    return false;
  }
  f.n    = (int64_t)be64(h + 12);
  f.dims = (int)be32(h + 20);
  f.code = (int)be32(h + 24);
  f.word = nc_word_size(f.code);
  if (f.word == 0) {
    err = path + ": unrecognised NChilada type code " + std::to_string(f.code);
    return false;
  }
  if (f.dims != want_dims) {
    err = path + ": expected dimensions " + std::to_string(want_dims) +
          ", file says " + std::to_string(f.dims);
    return false;
  }
  f.data_off = 28 + 2 * (int64_t)f.dims * f.word;  // header + min + max

  // A min==max field may omit the data. Detect by size rather than by
  // comparing min/max, so a truncated file is caught too.
  int64_t need = f.data_off + f.n * (int64_t)f.dims * f.word;
  int64_t have = file_size(path);
  if (have != need) {
    err = path + ": size " + std::to_string(have) + " != expected " +
          std::to_string(need) +
          " (a min==max field may store no per-particle data; this reader "
          "will not synthesise it)";
    return false;
  }
  f.present = true;
  return true;
}

inline bool nc_open(const std::string& dir, Snapshot& s, std::string& err) {
  static const char* fam[3] = {"gas", "dark", "star"};
  s.fmt = Format::NChilada;
  s.path = dir;
  int64_t n[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    if (!nc_open_field(dir + "/" + fam[i] + "/pos", 3, s.pos[i], err))
      return false;
    if (!s.pos[i].present) continue;
    n[i] = s.pos[i].n;
    // mass is optional here: only the SWIFT converter wants it, and it is
    // checked for consistency when it exists.
    if (!nc_open_field(dir + "/" + fam[i] + "/mass", 1, s.mass[i], err))
      return false;
    if (s.mass[i].present && s.mass[i].n != n[i]) {
      err = dir + "/" + fam[i] + ": mass has " +
            std::to_string(s.mass[i].n) + " elements but pos has " +
            std::to_string(n[i]);
      return false;
    }
  }
  s.n_gas = n[0]; s.n_dark = n[1]; s.n_star = n[2];
  s.n_total = n[0] + n[1] + n[2];
  if (s.n_total == 0) { err = dir + ": no particles in any family"; return false; }
  return true;
}

// ---- Tipsy ----------------------------------------------------------------

inline bool tipsy_open(const std::string& path, Snapshot& s, std::string& err) {
  s.fmt = Format::Tipsy;
  s.path = path;
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) { err = path + ": " + std::strerror(errno); return false; }
  unsigned char h[32];
  bool ok = full_pread(fd, h, 32, 0, err);
  ::close(fd);
  if (!ok) { err = path + ": " + err; return false; }

  s.time        = bef64(h + 0);
  int64_t nbody = (int32_t)be32(h + 8);
  int32_t ndim  = (int32_t)be32(h + 12);
  s.n_gas       = (int32_t)be32(h + 16);
  s.n_dark      = (int32_t)be32(h + 20);
  s.n_star      = (int32_t)be32(h + 24);

  if (ndim != 3) {
    err = path + ": ndim reads " + std::to_string(ndim) +
          " under big-endian decoding -- this reader only handles XDR "
          "(big-endian) Tipsy, which is what every snapshot in this campaign "
          "is; a native little-endian file would need a byte-order probe";
    return false;
  }
  s.n_total = s.n_gas + s.n_dark + s.n_star;
  if (s.n_total != nbody) {
    err = path + ": header nbodies " + std::to_string(nbody) +
          " != nsph+ndark+nstar " + std::to_string(s.n_total);
    return false;
  }

  // Single vs double precision positions is decided by file size, exactly as
  // TipsyReader::loadHeader does it.
  int64_t have = file_size(path);
  auto total_for = [&](int g, int d, int st) {
    return s.preface + s.n_gas * g + s.n_dark * d + s.n_star * st;
  };
  if (have == total_for(48, 36, 44)) {
    s.gas_rec = 48; s.dark_rec = 36; s.star_rec = 44; s.pos_word = 4;
  } else if (have == total_for(72, 60, 68)) {
    // double pos, single vel: pos becomes 3 x 8 B
    s.gas_rec = 72; s.dark_rec = 60; s.star_rec = 68; s.pos_word = 8;
  } else {
    err = path + ": size " + std::to_string(have) +
          " matches neither the single-precision layout (" +
          std::to_string(total_for(48, 36, 44)) + ") nor the double-position "
          "layout (" + std::to_string(total_for(72, 60, 68)) + ")";
    return false;
  }
  return true;
}

// ---- public API -----------------------------------------------------------

/// Open `path`: a directory is read as NChilada, a file as Tipsy.
inline bool open_snapshot(const std::string& path, Snapshot& s,
                          std::string& err) {
  return is_dir(path) ? nc_open(path, s, err) : tipsy_open(path, s, err);
}

/// Global particle range [start, start+count) -> `out` as 3*count floats.
/// `out` must hold 3*count floats. Reads in bounded chunks so the caller
/// never needs a buffer proportional to `count` beyond `out` itself.
inline bool read_pos(const Snapshot& s, int64_t start, int64_t count,
                     float* out, std::string& err) {
  if (start < 0 || count < 0 || start + count > s.n_total) {
    err = "read_pos: range [" + std::to_string(start) + "," +
          std::to_string(start + count) + ") outside snapshot of " +
          std::to_string(s.n_total) + " particles";
    return false;
  }
  if (count == 0) return true;

  const int64_t bnd[4] = {0, s.n_gas, s.n_gas + s.n_dark, s.n_total};

  for (int fam = 0; fam < 3; ++fam) {
    int64_t lo = std::max(start, bnd[fam]);
    int64_t hi = std::min(start + count, bnd[fam + 1]);
    if (lo >= hi) continue;
    int64_t local_lo = lo - bnd[fam];      // index within the family
    int64_t n = hi - lo;
    float* dst = out + 3 * (lo - start);

    if (s.fmt == Format::NChilada) {
      const NCField& f = s.pos[fam];
      if (!f.present) { err = "read_pos: family has no pos field"; return false; }
      if (f.code != NC_FLOAT32 && f.code != NC_FLOAT64) {
        err = f.path + ": pos is not a floating-point field";
        return false;
      }
      int fd = ::open(f.path.c_str(), O_RDONLY);
      if (fd < 0) { err = f.path + ": " + std::strerror(errno); return false; }
      const int64_t rec = 3LL * f.word;
      const int64_t CH = 1 << 20;  // particles per chunk
      std::vector<unsigned char> buf((size_t)(CH * rec));
      bool ok = true;
      for (int64_t done = 0; done < n && ok; done += CH) {
        int64_t m = std::min(CH, n - done);
        ok = full_pread(fd, buf.data(), (size_t)(m * rec),
                        f.data_off + (local_lo + done) * rec, err);
        if (!ok) break;
        float* o = dst + 3 * done;
        if (f.word == 4)
          for (int64_t i = 0; i < 3 * m; ++i) o[i] = bef32(buf.data() + 4 * i);
        else
          for (int64_t i = 0; i < 3 * m; ++i)
            o[i] = (float)bef64(buf.data() + 8 * i);
      }
      ::close(fd);
      if (!ok) { err = f.path + ": " + err; return false; }
    } else {
      // Tipsy: strided. Read whole records in bulk and pick out pos.
      const int rec = (fam == 0 ? s.gas_rec : fam == 1 ? s.dark_rec : s.star_rec);
      int64_t base = s.preface;
      if (fam >= 1) base += s.n_gas * s.gas_rec;
      if (fam >= 2) base += s.n_dark * s.dark_rec;
      int fd = ::open(s.path.c_str(), O_RDONLY);
      if (fd < 0) { err = s.path + ": " + std::strerror(errno); return false; }
      const int64_t CH = 1 << 19;
      std::vector<unsigned char> buf((size_t)CH * rec);
      bool ok = true;
      for (int64_t done = 0; done < n && ok; done += CH) {
        int64_t m = std::min(CH, n - done);
        ok = full_pread(fd, buf.data(), (size_t)(m * rec),
                        base + (local_lo + done) * (int64_t)rec, err);
        if (!ok) break;
        float* o = dst + 3 * done;
        for (int64_t i = 0; i < m; ++i) {
          const unsigned char* r = buf.data() + (size_t)i * rec + s.pos_in_rec;
          if (s.pos_word == 4) {
            o[3 * i + 0] = bef32(r + 0);
            o[3 * i + 1] = bef32(r + 4);
            o[3 * i + 2] = bef32(r + 8);
          } else {
            o[3 * i + 0] = (float)bef64(r + 0);
            o[3 * i + 1] = (float)bef64(r + 8);
            o[3 * i + 2] = (float)bef64(r + 16);
          }
        }
      }
      ::close(fd);
      if (!ok) { err = s.path + ": " + err; return false; }
    }
  }
  return true;
}

/// As read_pos, for the scalar `mass` attribute. Returns false when the
/// snapshot does not carry mass for a family the range touches.
inline bool read_mass(const Snapshot& s, int64_t start, int64_t count,
                      float* out, std::string& err) {
  if (start < 0 || count < 0 || start + count > s.n_total) {
    err = "read_mass: range outside snapshot";
    return false;
  }
  if (count == 0) return true;
  const int64_t bnd[4] = {0, s.n_gas, s.n_gas + s.n_dark, s.n_total};

  for (int fam = 0; fam < 3; ++fam) {
    int64_t lo = std::max(start, bnd[fam]);
    int64_t hi = std::min(start + count, bnd[fam + 1]);
    if (lo >= hi) continue;
    int64_t local_lo = lo - bnd[fam];
    int64_t n = hi - lo;
    float* dst = out + (lo - start);

    if (s.fmt == Format::NChilada) {
      const NCField& f = s.mass[fam];
      if (!f.present) {
        err = s.path + ": no mass field for family " + std::to_string(fam);
        return false;
      }
      int fd = ::open(f.path.c_str(), O_RDONLY);
      if (fd < 0) { err = f.path + ": " + std::strerror(errno); return false; }
      const int64_t CH = 1 << 21;
      std::vector<unsigned char> buf((size_t)CH * f.word);
      bool ok = true;
      for (int64_t done = 0; done < n && ok; done += CH) {
        int64_t m = std::min(CH, n - done);
        ok = full_pread(fd, buf.data(), (size_t)(m * f.word),
                        f.data_off + (local_lo + done) * f.word, err);
        if (!ok) break;
        for (int64_t i = 0; i < m; ++i)
          dst[done + i] = (f.word == 4) ? bef32(buf.data() + 4 * i)
                                        : (float)bef64(buf.data() + 8 * i);
      }
      ::close(fd);
      if (!ok) { err = f.path + ": " + err; return false; }
    } else {
      const int rec = (fam == 0 ? s.gas_rec : fam == 1 ? s.dark_rec : s.star_rec);
      int64_t base = s.preface;
      if (fam >= 1) base += s.n_gas * s.gas_rec;
      if (fam >= 2) base += s.n_dark * s.dark_rec;
      int fd = ::open(s.path.c_str(), O_RDONLY);
      if (fd < 0) { err = s.path + ": " + std::strerror(errno); return false; }
      const int64_t CH = 1 << 19;
      std::vector<unsigned char> buf((size_t)CH * rec);
      bool ok = true;
      for (int64_t done = 0; done < n && ok; done += CH) {
        int64_t m = std::min(CH, n - done);
        ok = full_pread(fd, buf.data(), (size_t)(m * rec),
                        base + (local_lo + done) * (int64_t)rec, err);
        if (!ok) break;
        for (int64_t i = 0; i < m; ++i)
          dst[done + i] = bef32(buf.data() + (size_t)i * rec + s.mass_in_rec);
      }
      ::close(fd);
      if (!ok) { err = s.path + ": " + err; return false; }
    }
  }
  return true;
}

}  // namespace cosmoio
#endif
