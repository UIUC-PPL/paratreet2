/* tipsy2nchilada: convert a Tipsy snapshot into an NChilada directory.
 *
 *   ./tipsy2nchilada <in.tipsy> <out-directory> [float|double]
 *
 * The optional third argument sets the on-disk type of the position and
 * velocity fields (default float, matching Tipsy); passing `double` produces
 * a snapshot that exercises the reader's type conversion, since real
 * NChilada snapshots are written with whichever precision the simulation
 * ran at.
 *
 * The output is the layout ParaTreeT's Reader (src/NChiladaReader.h) and
 * ChaNGa's TreePiece::loadNChilada expect:
 *
 *     <dir>/description.xml
 *     <dir>/{gas,dark,star}/{pos,vel,mass,soft}
 *
 * with an empty family's directory left out entirely. Each field file is
 * FieldHeader | min | max | numParticles values, XDR-encoded.
 *
 * This is a test-input tool: it holds the whole snapshot in memory, which is
 * fine for the Tipsy-sized inputs it can read in the first place (Tipsy's
 * header counts are 32-bit). Production NChilada snapshots come out of the
 * simulation that generated them.
 */

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "TipsyFile.h"
#include "tree_xdr.h"

using namespace std;

static void fail(const string& what) {
  cerr << "tipsy2nchilada: " << what << endl;
  exit(1);
}

static void makeDir(const string& path) {
  if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
    fail("couldn't create directory " + path + ": " + strerror(errno));
}

// Min/max of a field. For a vector field these are the componentwise
// extremes -- the corners of the bounding box, which is what ChaNGa writes
// and what a reader compares to detect a constant field.
static void accumulate(float v, float& mn, float& mx) {
  if (v < mn) mn = v;
  if (v > mx) mx = v;
}

static void accumulate(double v, double& mn, double& mx) {
  if (v < mn) mn = v;
  if (v > mx) mx = v;
}

template <typename T>
static void accumulate(const Vector3D<T>& v, Vector3D<T>& mn, Vector3D<T>& mx) {
  accumulate(v.x, mn.x, mx.x);
  accumulate(v.y, mn.y, mx.y);
  accumulate(v.z, mn.z, mx.z);
}

template <typename T>
static void writeNCField(const string& path, double time, vector<T>& data) {
  if (data.empty()) return;

  T mn = data[0], mx = data[0];
  for (size_t i = 1; i < data.size(); ++i) accumulate(data[i], mn, mx);

  FILE* f = fopen(path.c_str(), "wb");
  if (f == NULL) fail("couldn't open " + path + " for writing");

  FieldHeader fh;
  fh.time = time;
  fh.numParticles = data.size();
  fh.dimensions = TypeHandling::Type2Dimensions<T>::dimensions;
  fh.code = TypeHandling::Type2Code<T>::code;

  XDR xdrs;
  xdrstdio_create(&xdrs, f, XDR_ENCODE);
  // writeField (tree_xdr.h) lays down header, min, max, then the values.
  if (!writeField(fh, &xdrs, &data[0], &mn, &mx))
    fail("couldn't write " + path);
  xdr_destroy(&xdrs);
  if (fclose(f) != 0) fail("couldn't close " + path);
}

/// One particle family's four attributes, converted to the requested
/// on-disk precision. Positions and velocities arrive as Vector3D<Real>
/// from the Tipsy structures; mass and softening stay float, as Tipsy
/// stores them.
template <typename PosT>
static void writeFamily(const string& dir, double time,
                        const vector<Vector3D<Tipsy::Real> >& pos,
                        const vector<Vector3D<Tipsy::Real> >& vel,
                        const vector<float>& mass, const vector<float>& soft) {
  if (pos.empty()) return;
  makeDir(dir);

  vector<Vector3D<PosT> > p(pos.size()), v(vel.size());
  for (size_t i = 0; i < pos.size(); ++i) p[i] = Vector3D<PosT>(pos[i]);
  for (size_t i = 0; i < vel.size(); ++i) v[i] = Vector3D<PosT>(vel[i]);

  writeNCField(dir + "/pos", time, p);
  writeNCField(dir + "/vel", time, v);
  vector<float> m(mass), s(soft);
  writeNCField(dir + "/mass", time, m);
  writeNCField(dir + "/soft", time, s);
}

static void writeDescription(const string& dir, bool gas, bool dark, bool star) {
  ofstream desc((dir + "/description.xml").c_str());
  desc << "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n";
  desc << "<simulation>\n";
  const char* families[3] = {"star", "dark", "gas"};
  const bool present[3] = {star, dark, gas};
  for (int i = 0; i < 3; ++i) {
    if (!present[i]) continue;
    desc << "\t<family name=\"" << families[i] << "\">\n";
    // Attribute names follow ChaNGa's description.xml convention: the long
    // name for the attribute, the on-disk file name for the link.
    desc << "\t\t<attribute name=\"mass\" link=\"" << families[i] << "/mass\"/>\n";
    desc << "\t\t<attribute name=\"position\" link=\"" << families[i] << "/pos\"/>\n";
    desc << "\t\t<attribute name=\"soft\" link=\"" << families[i] << "/soft\"/>\n";
    desc << "\t\t<attribute name=\"velocity\" link=\"" << families[i] << "/vel\"/>\n";
    desc << "\t</family>\n";
  }
  desc << "</simulation>\n";
}

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    cerr << "usage: ./tipsy2nchilada <input Tipsy file> <output NChilada "
            "directory> [float|double]" << endl;
    return 1;
  }
  const string infile(argv[1]);
  const string outdir(argv[2]);
  const bool doublePos = (argc == 4 && string(argv[3]) == "double");
  if (argc == 4 && !doublePos && string(argv[3]) != "float")
    fail("third argument must be 'float' or 'double'");

  Tipsy::TipsyFile tf(infile);
  if (!tf.loadedSuccessfully()) fail("couldn't load Tipsy file " + infile);

  const double time = tf.h.time;
  cout << infile << ": " << tf.h.nbodies << " particles (" << tf.h.nsph
       << " gas, " << tf.h.ndark << " dark, " << tf.h.nstar << " star) at time "
       << time << endl;

  makeDir(outdir);

  // Families are written in the order the global particle index runs: gas,
  // then dark, then star. Softening comes from each family's own Tipsy
  // field -- smoothing length for gas, gravitational softening otherwise.
  {
    vector<Vector3D<Tipsy::Real> > pos(tf.gas.size()), vel(tf.gas.size());
    vector<float> mass(tf.gas.size()), soft(tf.gas.size());
    for (size_t i = 0; i < tf.gas.size(); ++i) {
      pos[i] = tf.gas[i].pos;
      vel[i] = tf.gas[i].vel;
      mass[i] = tf.gas[i].mass;
      soft[i] = tf.gas[i].hsmooth;
    }
    if (doublePos) writeFamily<double>(outdir + "/gas", time, pos, vel, mass, soft);
    else writeFamily<float>(outdir + "/gas", time, pos, vel, mass, soft);
  }
  {
    vector<Vector3D<Tipsy::Real> > pos(tf.darks.size()), vel(tf.darks.size());
    vector<float> mass(tf.darks.size()), soft(tf.darks.size());
    for (size_t i = 0; i < tf.darks.size(); ++i) {
      pos[i] = tf.darks[i].pos;
      vel[i] = tf.darks[i].vel;
      mass[i] = tf.darks[i].mass;
      soft[i] = tf.darks[i].eps;
    }
    if (doublePos) writeFamily<double>(outdir + "/dark", time, pos, vel, mass, soft);
    else writeFamily<float>(outdir + "/dark", time, pos, vel, mass, soft);
  }
  {
    vector<Vector3D<Tipsy::Real> > pos(tf.stars.size()), vel(tf.stars.size());
    vector<float> mass(tf.stars.size()), soft(tf.stars.size());
    for (size_t i = 0; i < tf.stars.size(); ++i) {
      pos[i] = tf.stars[i].pos;
      vel[i] = tf.stars[i].vel;
      mass[i] = tf.stars[i].mass;
      soft[i] = tf.stars[i].eps;
    }
    if (doublePos) writeFamily<double>(outdir + "/star", time, pos, vel, mass, soft);
    else writeFamily<float>(outdir + "/star", time, pos, vel, mass, soft);
  }

  writeDescription(outdir, !tf.gas.empty(), !tf.darks.empty(), !tf.stars.empty());

  cout << "wrote " << outdir << " (" << (doublePos ? "float64" : "float32")
       << " positions and velocities)" << endl;
  return 0;
}
