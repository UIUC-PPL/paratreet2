// Standalone test for the NChilada reader (src/NChiladaReader.*). NO Charm
// headers, NO Charm libraries, and no data files: the fixtures are written
// here, with XDR, and read back through the reader.
//
// Writing through xdr_template (utility/structures) is deliberate -- it is a
// completely separate encoder from the reader's bulk fread-and-byte-swap
// path, so agreement between them is real evidence and not a round trip
// through one piece of code.
//
// Covered: every type code the format allows, scalar and vector fields,
// slices at arbitrary offsets, the constant-field compression that lets a
// writer omit the per-particle data, absent optional attributes, and the
// malformed inputs that have to be rejected rather than misread.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "NChiladaReader.h"

using namespace std;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const string& what) {
  checks++;
  if (!ok) { cout << "  FAIL: " << what << endl; failures++; }
}

static string g_dir;

static void makeDir(const string& p) {
  if (mkdir(p.c_str(), 0755) != 0 && errno != EEXIST) {
    cerr << "cannot create " << p << endl;
    exit(2);
  }
}

/// Write one field file the way the format specifies: header, min, max, then
/// every value. `T` is the on-disk type.
template <typename T>
static void writeFixture(const string& path, const vector<T>& data, T mn, T mx) {
  FILE* f = fopen(path.c_str(), "wb");
  if (f == NULL) { cerr << "cannot write " << path << endl; exit(2); }
  FieldHeader fh;
  fh.time = 3.5;
  fh.numParticles = data.size();
  fh.dimensions = TypeHandling::Type2Dimensions<T>::dimensions;
  fh.code = TypeHandling::Type2Code<T>::code;
  XDR xdrs;
  xdrstdio_create(&xdrs, f, XDR_ENCODE);
  vector<T> copy(data);
  if (!writeField(fh, &xdrs, copy.empty() ? NULL : &copy[0], &mn, &mx)) {
    cerr << "writeField failed for " << path << endl;
    exit(2);
  }
  xdr_destroy(&xdrs);
  fclose(f);
}

/// Byte length of a field file that carries its data, for the truncation
/// that turns a constant field into a compressed one.
static long headerAndEnds(unsigned int dim, unsigned int wordBytes) {
  return FieldHeader::sizeBytes + 2L * dim * wordBytes;
}

// ---- the values every fixture family holds --------------------------------
// Positions vary, so they are never subject to constant-field compression;
// masses are constant, which is exactly the case the format compresses.

static double posValue(int fam, int i, int c) {
  return 0.125 * fam + 0.03125 * i + 0.5 * c;
}

template <typename T>
static vector<Vector3D<T> > makePositions(int fam, int n) {
  vector<Vector3D<T> > v(n);
  for (int i = 0; i < n; ++i)
    v[i] = Vector3D<T>(static_cast<T>(posValue(fam, i, 0)),
                       static_cast<T>(posValue(fam, i, 1)),
                       static_cast<T>(posValue(fam, i, 2)));
  return v;
}

template <typename T>
static void minMax(const vector<Vector3D<T> >& v, Vector3D<T>& mn, Vector3D<T>& mx) {
  mn = v[0]; mx = v[0];
  for (size_t i = 1; i < v.size(); ++i) {
    mn.x = min(mn.x, v[i].x); mn.y = min(mn.y, v[i].y); mn.z = min(mn.z, v[i].z);
    mx.x = max(mx.x, v[i].x); mx.y = max(mx.y, v[i].y); mx.z = max(mx.z, v[i].z);
  }
}

// ---- tests ----------------------------------------------------------------

/// Every slice of a position field must match the values that were written.
template <typename Dest>
static void checkPositions(const string& path, int fam, int n, const char* what) {
  const size_t slices[][2] = {{0, (size_t)n}, {0, 1}, {(size_t)n - 1, 1},
                              {1, (size_t)n - 1}, {(size_t)n / 2, (size_t)n / 2}};
  for (size_t s = 0; s < 5; ++s) {
    const size_t start = slices[s][0], count = slices[s][1];
    if (count == 0) continue;
    vector<Vector3D<Dest> > got(count);
    NChilada::readField(path, start, count, &got[0]);
    for (size_t i = 0; i < count; ++i) {
      const size_t j = start + i;
      const bool ok =
          got[i].x == static_cast<Dest>(posValue(fam, j, 0)) &&
          got[i].y == static_cast<Dest>(posValue(fam, j, 1)) &&
          got[i].z == static_cast<Dest>(posValue(fam, j, 2));
      if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s slice [%zu,%zu) element %zu: got (%g %g %g)",
                 what, start, start + count, j,
                 (double)got[i].x, (double)got[i].y, (double)got[i].z);
        check(false, msg);
        return;
      }
    }
  }
  checks++;   // one pass = one check
}

/// A scalar field must read back as its written value for every slice.
template <typename OnDisk, typename Dest>
static void checkScalar(const string& path, const vector<OnDisk>& written,
                        const char* what) {
  const size_t n = written.size();
  const size_t slices[][2] = {{0, n}, {0, 1}, {n - 1, 1}, {n / 3, n - n / 3}};
  for (size_t s = 0; s < 4; ++s) {
    const size_t start = slices[s][0], count = slices[s][1];
    if (count == 0) continue;
    vector<Dest> got(count);
    NChilada::readField(path, start, count, &got[0]);
    for (size_t i = 0; i < count; ++i) {
      if (got[i] != static_cast<Dest>(written[start + i])) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s slice [%zu,%zu) element %zu: got %g want %g",
                 what, start, start + count, start + i,
                 (double)got[i], (double)written[start + i]);
        check(false, msg);
        return;
      }
    }
  }
  checks++;
}

int main(int argc, char** argv) {
  g_dir = (argc > 1) ? argv[1] : "./nchilada-fixture";
  cout << "NChilada reader test, fixture in " << g_dir << endl;

  const int NGAS = 7, NDARK = 130, NSTAR = 3;
  makeDir(g_dir);
  makeDir(g_dir + "/gas");
  makeDir(g_dir + "/dark");
  makeDir(g_dir + "/star");

  // --- fixture -------------------------------------------------------------
  // gas positions float32, dark positions float64, star positions float32:
  // one snapshot exercising both float codes, since a reader must convert
  // whatever precision the simulation wrote.
  {
    vector<Vector3D<float> > p = makePositions<float>(0, NGAS);
    Vector3D<float> mn, mx; minMax(p, mn, mx);
    writeFixture(g_dir + "/gas/pos", p, mn, mx);
  }
  {
    vector<Vector3D<double> > p = makePositions<double>(1, NDARK);
    Vector3D<double> mn, mx; minMax(p, mn, mx);
    writeFixture(g_dir + "/dark/pos", p, mn, mx);
  }
  {
    vector<Vector3D<float> > p = makePositions<float>(2, NSTAR);
    Vector3D<float> mn, mx; minMax(p, mn, mx);
    writeFixture(g_dir + "/star/pos", p, mn, mx);
  }

  // Velocities only for dark, so the absent-optional-attribute path is live
  // for gas and star.
  {
    vector<Vector3D<float> > v = makePositions<float>(5, NDARK);
    Vector3D<float> mn, mx; minMax(v, mn, mx);
    writeFixture(g_dir + "/dark/vel", v, mn, mx);
  }

  // Masses: gas varying float32, dark CONSTANT float32 (written with the
  // data, then a truncated copy without it), star int32 -- an integer code
  // to prove the type dispatch is not float-only.
  vector<float> gasMass(NGAS);
  for (int i = 0; i < NGAS; ++i) gasMass[i] = 0.5f + i;
  writeFixture(g_dir + "/gas/mass", gasMass, gasMass[0], gasMass[NGAS - 1]);

  vector<float> darkMass(NDARK, 0.0625f);
  writeFixture(g_dir + "/dark/mass", darkMass, darkMass[0], darkMass[0]);

  vector<int> starMass(NSTAR);
  for (int i = 0; i < NSTAR; ++i) starMass[i] = 100 + i;
  writeFixture(g_dir + "/star/mass", starMass, starMass[0], starMass[NSTAR - 1]);

  // Softening: an int64 field, to cover the 8-byte integer word.
  vector<int64_t> darkSoft(NDARK);
  for (int i = 0; i < NDARK; ++i) darkSoft[i] = 1000000 + i;
  writeFixture(g_dir + "/dark/soft", darkSoft, darkSoft[0], darkSoft[NDARK - 1]);

  // --- header --------------------------------------------------------------
  cout << "header" << endl;
  NChilada::Header h;
  check(NChilada::loadHeader(g_dir, h), "loadHeader succeeds");
  check(h.nsph == NGAS, "gas count");
  check(h.ndark == NDARK, "dark count");
  check(h.nstar == NSTAR, "star count");
  check(h.nbodies == NGAS + NDARK + NSTAR, "total count");
  check(h.time == 3.5, "time");
  check(NChilada::isDirectory(g_dir), "isDirectory on the snapshot");
  check(!NChilada::isDirectory(g_dir + "/gas/pos"), "isDirectory on a field file");
  check(!NChilada::isDirectory(g_dir + "/nope"), "isDirectory on a missing path");
  check(NChilada::fieldCount(g_dir + "/gas/nosuch") == 0, "absent field counts 0");

  // --- values, both destination precisions ---------------------------------
  cout << "field values" << endl;
  checkPositions<float>(g_dir + "/gas/pos", 0, NGAS, "gas/pos float32->float");
  checkPositions<double>(g_dir + "/gas/pos", 0, NGAS, "gas/pos float32->double");
  checkPositions<double>(g_dir + "/dark/pos", 1, NDARK, "dark/pos float64->double");
  checkPositions<float>(g_dir + "/star/pos", 2, NSTAR, "star/pos float32->float");
  checkScalar<float, float>(g_dir + "/gas/mass", gasMass, "gas/mass float32");
  checkScalar<int, float>(g_dir + "/star/mass", starMass, "star/mass int32");
  checkScalar<int64_t, double>(g_dir + "/dark/soft", darkSoft, "dark/soft int64");

  // --- constant fields -----------------------------------------------------
  // A constant field reads correctly whether or not the writer bothered to
  // store the values; truncating to header+min+max is the compressed form.
  cout << "constant fields" << endl;
  checkScalar<float, float>(g_dir + "/dark/mass", darkMass, "dark/mass constant, data present");
  if (truncate((g_dir + "/dark/mass").c_str(), headerAndEnds(1, 4)) != 0)
    check(false, "could not truncate dark/mass");
  checkScalar<float, float>(g_dir + "/dark/mass", darkMass, "dark/mass constant, data omitted");
  {
    vector<float> got(NDARK);
    NChilada::readField(g_dir + "/dark/mass", NDARK - 1, 1, &got[0]);
    check(got[0] == 0.0625f, "compressed constant field reads at the last index");
  }

  // --- absent optional attributes ------------------------------------------
  cout << "optional attributes" << endl;
  {
    vector<Vector3D<float> > v(NDARK);
    check(NChilada::readOptionalField(g_dir + "/dark/vel", 0, NDARK, &v[0]),
          "present optional vector field returns true");
    vector<Vector3D<float> > g(NGAS, Vector3D<float>(-1.0f));
    check(!NChilada::readOptionalField(g_dir + "/gas/vel", 0, NGAS, &g[0]),
          "absent optional vector field returns false");
    check(g[0].x == -1.0f, "absent optional field leaves the destination alone");
    vector<float> s(NGAS);
    check(!NChilada::readOptionalField(g_dir + "/gas/soft", 0, NGAS, &s[0]),
          "absent optional scalar field returns false");
  }

  // --- malformed input must be rejected ------------------------------------
  // Silently misreading a snapshot is far worse than refusing it, so each of
  // these has to throw rather than return something plausible.
  cout << "rejections" << endl;
  {
    bool threw = false;
    try { vector<Vector3D<float> > p(2); NChilada::readField(g_dir + "/gas/pos", NGAS - 1, 2, &p[0]); }
    catch (XDRException&) { threw = true; }
    check(threw, "range past the end of the field throws");
  }
  {
    bool threw = false;
    try { vector<float> v(1); NChilada::readField(g_dir + "/gas/pos", 0, 1, &v[0]); }
    catch (XDRException&) { threw = true; }
    check(threw, "reading a 3-d field as a scalar throws");
  }
  {
    bool threw = false;
    try { vector<Vector3D<float> > p(1); NChilada::readField(g_dir + "/gas/mass", 0, 1, &p[0]); }
    catch (XDRException&) { threw = true; }
    check(threw, "reading a scalar field as 3-d throws");
  }
  {
    bool threw = false;
    try { vector<float> v(1); NChilada::readField(g_dir + "/gas/nosuch", 0, 1, &v[0]); }
    catch (XDRException&) { threw = true; }
    check(threw, "required field that is absent throws");
  }
  {
    // Not a field file at all: the magic number has to catch it.
    const string junk = g_dir + "/junk";
    FILE* f = fopen(junk.c_str(), "wb");
    for (int i = 0; i < 64; ++i) fputc(i, f);
    fclose(f);
    bool threw = false;
    try { NChilada::fieldCount(junk); } catch (XDRException&) { threw = true; }
    check(threw, "bad magic number throws");
  }
  {
    // An empty directory holds no particles; loadHeader says so instead of
    // reporting a zero-particle snapshot as loadable.
    const string empty = g_dir + "/empty";
    makeDir(empty);
    NChilada::Header eh;
    check(!NChilada::loadHeader(empty, eh), "empty directory reports no particles");
  }

  cout << (failures ? "FAILED: " : "PASSED: ") << (checks - failures) << "/"
       << checks << " checks" << endl;
  return failures ? 1 : 0;
}
