#ifndef PARATREET_NCHILADA_READER_H_
#define PARATREET_NCHILADA_READER_H_

// NChilada input support (design note: the Tipsy header stores its particle
// counts in 32-bit fields, so a Tipsy snapshot tops out near 2^31 particles;
// NChilada is the format used past that point).
//
// An NChilada "file" is a DIRECTORY, not a file:
//
//     <dir>/description.xml     optional, purely descriptive -- not read here
//     <dir>/gas/{pos,vel,mass,soft,...}
//     <dir>/dark/{pos,vel,mass,soft,...}
//     <dir>/star/{pos,vel,mass,soft,...}
//
// Each leaf is one "field file" holding a single attribute for a single
// particle family, XDR-encoded (big-endian) as
//
//     FieldHeader (28 bytes) | min value | max value | numParticles values
//
// where a value is `dimensions` (1 or 3) words of the header's type code.
// Families are absent when empty; a family's particle i is the same particle
// in every one of its field files. The global particle ordering that the rest
// of ParaTreeT sees is gas, then dark, then star -- the same convention Tipsy
// uses and the one ChaNGa's TreePiece::loadNChilada assumes.
//
// A field whose min equals its max may omit the per-particle data entirely
// (the value is recovered from min), so that case must never be read through.
//
// This reader deliberately does NOT go through utility/structures'
// NChilReader (one xdr_template call per element) or tree_xdr.h's readField
// (which allocates the whole field and carries a 32-bit-position workaround).
// Field data is fixed-width big-endian, so a range is read as a bulk fread
// plus a byte swap, and seeking uses fseeko so multi-GB offsets need no
// kludge.

#include <cstdint>
#include <string>

#include "Vector3D.h"
#include "tree_xdr.h"   // FieldHeader, XDRException, TypeHandling codes

namespace NChilada {

/// Per-family particle counts of a snapshot directory, filled from the
/// `pos` field header of each family.
struct Header {
  double time = 0.0;
  int64_t nsph = 0;
  int64_t ndark = 0;
  int64_t nstar = 0;
  int64_t nbodies = 0;
};

/// True when `path` exists and is a directory, i.e. is to be read as an
/// NChilada snapshot rather than as a Tipsy file.
bool isDirectory(const std::string& path);

/// Number of particles recorded in field file `path`, or 0 when the file
/// does not exist (an absent family or an absent optional attribute).
/// When `time` is non-null it receives the field header's time stamp.
/// Throws XDRException if the file exists but is not a field file.
int64_t fieldCount(const std::string& path, double* time = nullptr);

/// Fill `h` from <dirname>/{gas,dark,star}/pos. Returns false when the
/// directory holds no particles at all.
bool loadHeader(const std::string& dirname, Header& h);

/// Read elements [start, start + count) of field file `path` into `dest`,
/// converting from whatever type the file holds. `T` is the destination
/// element type and selects the dimensionality the file must declare: a
/// scalar (Real) reads a 1-dimensional field, a Vector3D a 3-dimensional
/// one. Throws XDRException on a missing or malformed file, or on a range
/// that runs off the end of the field.
template <typename T>
void readField(const std::string& path, int64_t start, int64_t count, T* dest);

/// As readField, but returns false and leaves `dest` untouched when the
/// field file does not exist. Used for attributes ParaTreeT can default
/// (velocity, softening) so a position/mass-only snapshot still loads.
template <typename T>
bool readOptionalField(const std::string& path, int64_t start, int64_t count,
                       T* dest);

} // namespace NChilada

#endif // PARATREET_NCHILADA_READER_H_
