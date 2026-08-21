#include "NChiladaReader.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <endian.h>
#include <sys/stat.h>
#include <vector>

namespace NChilada {

namespace {

// ---- fixed-width big-endian decoding -------------------------------------
// XDR pads every integer type narrower than 32 bits out to a 4-byte word
// (xdr_char and xdr_short both go through xdr_int) and stores floats as raw
// big-endian IEEE-754, so every value in a field file is either a 4-byte or
// an 8-byte big-endian word -- which is exactly what tree_xdr.h's mySizeof()
// reports. Decoding is therefore a byte swap and a cast, and a whole range
// can be pulled in with one fread instead of a call per element.

inline uint32_t beWord32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return be32toh(v);
}

inline uint64_t beWord64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return be64toh(v);
}

/// Decode `n` consecutive words of type `code` out of `raw` into `out`.
template <typename Dest>
void decode(TypeHandling::DataTypeCode code, const char* raw, int64_t n,
            Dest* out) {
  switch (code) {
    case TypeHandling::int8:
    case TypeHandling::int16:
    case TypeHandling::int32:
      for (int64_t i = 0; i < n; ++i)
        out[i] = static_cast<Dest>(static_cast<int32_t>(beWord32(raw + 4 * i)));
      return;
    case TypeHandling::uint8:
    case TypeHandling::uint16:
    case TypeHandling::uint32:
      for (int64_t i = 0; i < n; ++i)
        out[i] = static_cast<Dest>(beWord32(raw + 4 * i));
      return;
    case TypeHandling::int64:
      for (int64_t i = 0; i < n; ++i)
        out[i] = static_cast<Dest>(static_cast<int64_t>(beWord64(raw + 8 * i)));
      return;
    case TypeHandling::uint64:
      for (int64_t i = 0; i < n; ++i)
        out[i] = static_cast<Dest>(beWord64(raw + 8 * i));
      return;
    case TypeHandling::float32:
      for (int64_t i = 0; i < n; ++i) {
        const uint32_t w = beWord32(raw + 4 * i);
        float f;
        std::memcpy(&f, &w, 4);
        out[i] = static_cast<Dest>(f);
      }
      return;
    case TypeHandling::float64:
      for (int64_t i = 0; i < n; ++i) {
        const uint64_t w = beWord64(raw + 8 * i);
        double d;
        std::memcpy(&d, &w, 8);
        out[i] = static_cast<Dest>(d);
      }
      return;
    default:
      throw XDRException("Unrecognized data type code in NChilada field file");
  }
}

// ---- destination flattening ----------------------------------------------
// A field value is `dimensions` scalars laid out consecutively. Vector3D is
// exactly three of them, which lets one code path fill both a scalar and a
// vector field.

template <typename T>
struct Flatten {
  typedef T Scalar;
  static const unsigned int dimensions = 1;
  static Scalar* first(T* p) { return p; }
};

template <typename T>
struct Flatten<Vector3D<T> > {
  typedef T Scalar;
  static const unsigned int dimensions = 3;
  static Scalar* first(Vector3D<T>* p) { return &p->x; }
};

/// Open a field file and read and validate its header. Returns null --
/// without throwing -- only when the file does not exist and `optional`.
FILE* openField(const std::string& path, unsigned int expectedDim,
                FieldHeader& fh, bool optional) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    if (optional && errno == ENOENT) return nullptr;
    throw XDRException("Couldn't open NChilada field file: " + path);
  }
  // xdr_destroy on an xdrstdio stream releases the XDR handle only; the
  // FILE* stays open and is repositioned explicitly below.
  XDR xdrs;
  xdrstdio_create(&xdrs, f, XDR_DECODE);
  const bool got = xdr_template(&xdrs, &fh);
  xdr_destroy(&xdrs);
  if (!got) {
    std::fclose(f);
    throw XDRException("Couldn't read the header of NChilada field file: " + path);
  }
  if (fh.magic != FieldHeader::MagicNumber) {
    std::fclose(f);
    throw XDRException(path + " is not an NChilada field file (bad magic number)");
  }
  if (fh.dimensions != expectedDim) {
    std::fclose(f);
    throw XDRException("Wrong dimensionality in NChilada field file: " + path);
  }
  return f;
}

/// Body of readField/readOptionalField: `dest` is count*dim consecutive
/// scalars. False (only when `optional`) means the file is not there.
template <typename Scalar>
bool readFlat(const std::string& path, unsigned int dim, int64_t start,
              int64_t count, Scalar* dest, bool optional) {
  FieldHeader fh;
  FILE* f = openField(path, dim, fh, optional);
  if (f == nullptr) return false;

  try {
    if (start < 0 || count < 0
        || start + count > static_cast<int64_t>(fh.numParticles))
      throw XDRException("Requested range lies outside NChilada field file: " + path);

    const unsigned int word = mySizeof(fh.code);
    if (word == 0)
      throw XDRException("Unrecognized data type code in NChilada field file: " + path);
    const size_t valueBytes = static_cast<size_t>(dim) * word;

    // The min and max values sit immediately after the header, and a field
    // whose min equals its max may omit the per-particle data entirely (the
    // value is recovered from min). So this has to be settled before any
    // data offset is computed, or a constant field reads off the end.
    // Comparison is on the decoded values rather than on the raw bytes, to
    // match what every other reader of this format does (tree_xdr.h's
    // readField, NChilReader::loadHeader). Decoding through double is exact
    // for the float codes; a 64-bit integer field past 2^53 could compare
    // equal when it is not, but no attribute read here is such a field.
    std::vector<char> ends(2 * valueBytes);
    if (fseeko(f, FieldHeader::sizeBytes, SEEK_SET) != 0
        || std::fread(&ends[0], 1, ends.size(), f) != ends.size())
      throw XDRException("Couldn't read min/max of NChilada field file: " + path);

    double lo[3], hi[3];
    decode(fh.code, &ends[0], dim, lo);
    decode(fh.code, &ends[0] + valueBytes, dim, hi);
    bool constant = true;
    for (unsigned int d = 0; d < dim; ++d)
      if (lo[d] != hi[d]) constant = false;

    if (constant) {
      for (int64_t i = 0; i < count; ++i)
        for (unsigned int d = 0; d < dim; ++d)
          dest[i * dim + d] = static_cast<Scalar>(lo[d]);
    } else {
      // fseeko takes a 64-bit off_t, so the multi-GB offsets of a large
      // snapshot need none of the repeated-SEEK_CUR workaround that
      // tree_xdr.h carries for xdr_setpos' 32-bit position.
      const off_t offset = static_cast<off_t>(FieldHeader::sizeBytes)
          + (static_cast<off_t>(start) + 2) * static_cast<off_t>(valueBytes);
      if (fseeko(f, offset, SEEK_SET) != 0)
        throw XDRException("Couldn't seek in NChilada field file: " + path);

      // Staged in bounded chunks: `count` is a whole Reader's share of a
      // family and the raw block is a pure intermediate.
      const int64_t kChunk = 1 << 16;
      std::vector<char> raw(static_cast<size_t>(kChunk) * valueBytes);
      for (int64_t done = 0; done < count; done += kChunk) {
        const int64_t n = std::min<int64_t>(kChunk, count - done);
        const size_t bytes = static_cast<size_t>(n) * valueBytes;
        if (std::fread(&raw[0], 1, bytes, f) != bytes)
          throw XDRException("Short read from NChilada field file: " + path);
        decode(fh.code, &raw[0], n * dim, dest + done * dim);
      }
    }
  } catch (...) {
    std::fclose(f);
    throw;
  }
  std::fclose(f);
  return true;
}

} // anonymous namespace

bool isDirectory(const std::string& path) {
  struct stat s;
  if (stat(path.c_str(), &s) != 0) return false;
  return S_ISDIR(s.st_mode);
}

int64_t fieldCount(const std::string& path, double* time) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return 0;   // absent family or absent attribute

  XDR xdrs;
  FieldHeader fh;
  xdrstdio_create(&xdrs, f, XDR_DECODE);
  const bool got = xdr_template(&xdrs, &fh);
  xdr_destroy(&xdrs);
  std::fclose(f);

  if (!got)
    throw XDRException("Couldn't read the header of NChilada field file: " + path);
  if (fh.magic != FieldHeader::MagicNumber)
    throw XDRException(path + " is not an NChilada field file (bad magic number)");
  if (fh.dimensions != 1 && fh.dimensions != 3)
    throw XDRException("Wrong dimensionality in NChilada field file: " + path);

  if (time != nullptr) *time = fh.time;
  return static_cast<int64_t>(fh.numParticles);
}

bool loadHeader(const std::string& dirname, Header& h) {
  // The family counts come from `pos`, which every family has; a family
  // directory that is missing entirely counts as zero particles. The time
  // stamp is the same in every field file of a snapshot, so whichever
  // family is present supplies it.
  double time = 0.0;
  h.nsph = fieldCount(dirname + "/gas/pos", &time);
  h.ndark = fieldCount(dirname + "/dark/pos", &time);
  h.nstar = fieldCount(dirname + "/star/pos", &time);
  h.nbodies = h.nsph + h.ndark + h.nstar;
  h.time = time;
  return h.nbodies > 0;
}

template <typename T>
void readField(const std::string& path, int64_t start, int64_t count, T* dest) {
  typedef Flatten<T> F;
  static_assert(sizeof(T) == F::dimensions * sizeof(typename F::Scalar),
                "field destination type must be densely packed scalars");
  readFlat(path, F::dimensions, start, count, F::first(dest), false);
}

template <typename T>
bool readOptionalField(const std::string& path, int64_t start, int64_t count,
                       T* dest) {
  typedef Flatten<T> F;
  static_assert(sizeof(T) == F::dimensions * sizeof(typename F::Scalar),
                "field destination type must be densely packed scalars");
  return readFlat(path, F::dimensions, start, count, F::first(dest), true);
}

// Real is float or double depending on the build (src/common.h); instantiate
// both so this file does not have to care which.
template void readField<float>(const std::string&, int64_t, int64_t, float*);
template void readField<double>(const std::string&, int64_t, int64_t, double*);
template void readField<Vector3D<float> >(const std::string&, int64_t, int64_t, Vector3D<float>*);
template void readField<Vector3D<double> >(const std::string&, int64_t, int64_t, Vector3D<double>*);

template bool readOptionalField<float>(const std::string&, int64_t, int64_t, float*);
template bool readOptionalField<double>(const std::string&, int64_t, int64_t, double*);
template bool readOptionalField<Vector3D<float> >(const std::string&, int64_t, int64_t, Vector3D<float>*);
template bool readOptionalField<Vector3D<double> >(const std::string&, int64_t, int64_t, Vector3D<double>*);

} // namespace NChilada
