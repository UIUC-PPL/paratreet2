#include "TipsyFile.h"
#include "NChiladaReader.h"
#include "Reader.h"
#include "Utility.h"
#include "Modularization.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>
#include <cerrno>

Reader::Reader() : particle_index(0) {}

namespace {

/// Split the global particle range evenly over the Reader branches and
/// return this branch's [start, start + count). Both input formats -- and
/// ChaNGa's loaders -- use exactly this arithmetic, so a snapshot converted
/// from one format to the other puts the same particles on the same Reader.
void myParticleRange(int64_t n_total, int reader_index, int64_t& start,
                     int64_t& count) {
  count = n_total / n_readers;
  const int64_t excess = n_total % n_readers;
  start = count * reader_index;
  if (reader_index < excess) {
    count++;
    start += reader_index;
  } else {
    start += excess;
  }
}

/// Number of elements [lo, hi) and [a, b) have in common.
int64_t overlap(int64_t lo, int64_t hi, int64_t a, int64_t b) {
  return std::max<int64_t>(0, std::min(hi, b) - std::max(lo, a));
}

/// Fill dest[0, count) from the NChilada family directory `dir`, starting
/// at that family's particle `start`. Each attribute is a separate field
/// file, so it is staged through a bounded buffer and scattered into the
/// particle structs: the temporary stays a few MB however large a share of
/// the snapshot this Reader holds.
void loadNCFamily(const std::string& dir, Particle::Type type, int64_t start,
                  int64_t count, Particle* dest) {
  const int64_t kChunk = 1 << 18;   // particles staged at a time
  std::vector<Vector3D<Real>> vecs;
  std::vector<Real> scalars;

  for (int64_t done = 0; done < count; done += kChunk) {
    const int64_t n = std::min<int64_t>(kChunk, count - done);
    const int64_t s = start + done;
    Particle* p = dest + done;

    vecs.resize(n);
    NChilada::readField(dir + "/pos", s, n, &vecs[0]);
    for (int64_t i = 0; i < n; ++i) p[i].position = vecs[i];

    // Velocity and softening are defaulted rather than required. FoF needs
    // only positions (and masses for the bounding box), so a snapshot cut
    // down to those still loads; the reported kinetic energy is then zero.
    if (NChilada::readOptionalField(dir + "/vel", s, n, &vecs[0])) {
      for (int64_t i = 0; i < n; ++i) p[i].velocity = vecs[i];
    } else {
      for (int64_t i = 0; i < n; ++i) p[i].velocity = Vector3D<Real>(0.0);
    }

    scalars.resize(n);
    NChilada::readField(dir + "/mass", s, n, &scalars[0]);
    for (int64_t i = 0; i < n; ++i) p[i].mass = scalars[i];

    if (NChilada::readOptionalField(dir + "/soft", s, n, &scalars[0])) {
      for (int64_t i = 0; i < n; ++i) p[i].soft = scalars[i];
    } else {
      for (int64_t i = 0; i < n; ++i) p[i].soft = 0.0;
    }

    for (int64_t i = 0; i < n; ++i) p[i].type = type;
  }
}

} // anonymous namespace

// Picks the input format. An NChilada snapshot is a DIRECTORY of
// per-attribute field files, a Tipsy snapshot a single file -- the same
// test ChaNGa's Main::setupICs uses to choose a loader. NChilada exists
// because the Tipsy header's counts are 32-bit, capping a Tipsy snapshot
// near 2^31 particles.
void Reader::load(std::string input_file, const CkCallback& cb) {
  struct stat s;
  if (stat(input_file.c_str(), &s) != 0) {
    CkPrintf("Reader %d cannot stat input %s: %s\n", thisIndex,
        input_file.c_str(), strerror(errno));
    CkAbort("Input file missing or unreadable -- see stdout");
  }
  if (S_ISDIR(s.st_mode)) loadNChilada(input_file, cb);
  else loadTipsy(input_file, cb);
}

void Reader::loadNChilada(const std::string& dirname, const CkCallback& cb) {
  try {
    NChilada::Header h;
    if (!NChilada::loadHeader(dirname, h)) {
      CkPrintf("Reader %d found no particles under NChilada directory %s "
               "(expected gas/pos, dark/pos or star/pos)\n", thisIndex,
               dirname.c_str());
      CkAbort("NChilada reading failure in Reader -- see stdout");
    }
    start_time = h.time;

    int64_t start_particle = 0, n_particles = 0;
    myParticleRange(h.nbodies, thisIndex, start_particle, n_particles);
    const int64_t end_particle = start_particle + n_particles;

    particles.resize(n_particles);

    // Global ordering is gas, then dark, then star -- the Tipsy convention,
    // and what ChaNGa's loadNChilada assumes -- so this branch's slice is a
    // contiguous piece of at most one family each.
    const int64_t gas_end = h.nsph;
    const int64_t dark_end = h.nsph + h.ndark;
    const int64_t n_gas = overlap(start_particle, end_particle, 0, gas_end);
    const int64_t n_dark = overlap(start_particle, end_particle, gas_end, dark_end);
    const int64_t n_star = overlap(start_particle, end_particle, dark_end, h.nbodies);

    int64_t off = 0;
    if (n_gas > 0) {
      loadNCFamily(dirname + "/gas", Particle::Type::eGas,
                   std::max<int64_t>(start_particle, 0), n_gas, &particles[off]);
      off += n_gas;
    }
    if (n_dark > 0) {
      loadNCFamily(dirname + "/dark", Particle::Type::eDark,
                   std::max<int64_t>(start_particle, gas_end) - gas_end, n_dark,
                   &particles[off]);
      off += n_dark;
    }
    if (n_star > 0) {
      loadNCFamily(dirname + "/star", Particle::Type::eStar,
                   std::max<int64_t>(start_particle, dark_end) - dark_end, n_star,
                   &particles[off]);
      off += n_star;
    }
    CkAssert(off == n_particles);

    BoundingBox box;
    box.pe = 0.0;
    box.ke = 0.0;
    box.n_sph = n_gas;
    box.n_dark = n_dark;
    box.n_star = n_star;
    for (int64_t i = 0; i < n_particles; ++i) {
      particles[i].order = start_particle + i;
      box.grow(particles[i].position);
      box.mass += particles[i].mass;
      box.ke += particles[i].mass * particles[i].velocity.lengthSquared();
    }
    box.ke /= 2.0;
    box.n_particles = particles.size();

    #if DEBUG
    std::cout << "[Reader " << thisIndex << "] Built bounding box: " << box << std::endl;
    #endif

    contribute(sizeof(BoundingBox), &box, BoundingBox::reducer(), cb);
  }
  catch (XDRException& e) {
    CkPrintf("Reader %d failed to read NChilada directory %s: %s\n", thisIndex,
        dirname.c_str(), e.getText().c_str());
    CkAbort("NChilada reading failure in Reader -- see stdout");
  }
}

void Reader::loadTipsy(const std::string& input_file, const CkCallback& cb) {
  try {
    // Open tipsy file
    Tipsy::TipsyReader r(input_file);
    if (!r.status()) {
      CkPrintf("Reader %d failed to open tipsy file %s\n", thisIndex, input_file.c_str());
      CkAbort("Tipsy reading failure in Reader -- see stdout");
    }

    // Read header and count particles
    Tipsy::header tipsyHeader = r.getHeader();
    int n_total = tipsyHeader.nbodies;
    int n_sph = tipsyHeader.nsph;
    int n_dark = tipsyHeader.ndark;
    int n_star = tipsyHeader.nstar;
    start_time = tipsyHeader.time;

    int n_particles = n_total / n_readers;
    int excess = n_total % n_readers;
    // 64-bit: the product overflows int at ~2.1e9 total particles.
    long start_particle = (long)n_particles * thisIndex;
    if (thisIndex < (unsigned int)excess) {
      n_particles++;
      start_particle += thisIndex;
    } else {
      start_particle += excess;
    }

    // Prepare bounding box
    BoundingBox box;
    box.pe = 0.0;
    box.ke = 0.0;

    // Reserve space
    particles.resize(n_particles);

    // Read particles and grow bounding box
    if (!r.seekParticleNum(start_particle)) {
      CkAbort("Could not seek to particle\n");
    }

    Tipsy::gas_particle gp;
    Tipsy::dark_particle dp;
    Tipsy::star_particle sp;

    for (unsigned int i = 0; i < n_particles; i++) {
      //particles[i].potential = particles[i].u = particles[i].soft = 0u;
      if (start_particle + i < (unsigned int)n_sph) {
        if (!r.getNextGasParticle(gp)) {
          CkAbort("Could not read gas particle\n");
        }
        particles[i].mass = gp.mass;
        particles[i].soft = gp.hsmooth;
        particles[i].position = gp.pos;
        particles[i].velocity = gp.vel;
        //particles[i].u = gp.temp * gasConstant / gammam1 / meanMolWeight;
        particles[i].type = Particle::Type::eGas;
        box.n_sph++;
      }
      else if (start_particle + i < (unsigned int)n_sph + (unsigned int)n_dark) {
        if (!r.getNextDarkParticle(dp)) {
          CkAbort("Could not read dark particle\n");
        }
        particles[i].mass = dp.mass;
        particles[i].position = dp.pos;
        particles[i].velocity = dp.vel;
        particles[i].soft = dp.eps;
        particles[i].type = Particle::Type::eDark;
        box.n_dark++;
      }
      else {
        if (!r.getNextStarParticle(sp)) {
          CkAbort("Could not read star particle\n");
        }
        particles[i].mass = sp.mass;
        particles[i].position = sp.pos;
        particles[i].velocity = sp.vel;
        particles[i].type = Particle::Type::eStar;
        box.n_star++;
      }
      particles[i].order = start_particle + i;
      //particles[i].velocity_predicted = particles[i].velocity;
      //particles[i].u_predicted = particles[i].u;
      box.grow(particles[i].position);
      box.mass += particles[i].mass;
      box.ke += particles[i].mass * particles[i].velocity.lengthSquared();
      box.pe = 0.0;
    }

    box.ke /= 2.0;
    box.n_particles = particles.size();

    #if DEBUG
    std::cout << "[Reader " << thisIndex << "] Built bounding box: " << box << std::endl;
    #endif

    // Reduce to universal bounding box
    contribute(sizeof(BoundingBox), &box, BoundingBox::reducer(), cb);
  }
  catch (std::ios_base::failure) {
    CkPrintf("Reader %d failed to open tipsy file %s\n", thisIndex, input_file.c_str());
    CkAbort("Tipsy reading failure in Reader -- see stdout");
  }
}


void Reader::setSoft(const double dSoft, const CkCallback& cb) {
    for (std::vector<Particle>::iterator it = particles.begin();
         it != particles.end(); ++it) {
        it->soft = dSoft;
    }
    contribute(cb);
}

void Reader::computeUniverseBoundingBox(const CkCallback& cb) {
  BoundingBox box;
  for (std::vector<Particle>::const_iterator it = particles.begin();
       it != particles.end(); ++it) {
    box.grow(it->position);
    box.mass += it->mass;
    box.ke += 0.5 * it->mass * it->velocity.lengthSquared();
    box.n_particles += 1;
  }
  contribute(sizeof(BoundingBox), &box, BoundingBox::reducer(), cb);
}

void Reader::assignKeys(BoundingBox universe_, const CkCallback& cb) {
  // Generate particle keys
  universe = universe_;

  treespec.ckLocalBranch()->getPartitionDecomposition()->assignKeys(universe, particles);

  // Back to callee
  contribute(cb);
}

void Reader::countAssignments(const std::vector<GenericSplitter>& states, bool is_treepiece, const CkCallback& cb, bool weight_by_partition) {
  auto decomp = is_treepiece ? treespec.ckLocalBranch()->getTreePieceDecomposition() : treespec.ckLocalBranch()->getPartitionDecomposition();
  decomp->countAssignments(states, particles, this, cb, weight_by_partition);
}

void Reader::doSplit(const std::vector<GenericSplitter>& splits, bool is_treepiece, const CkCallback& cb) {
  auto decomp = is_treepiece ? treespec.ckLocalBranch()->getTreePieceDecomposition() : treespec.ckLocalBranch()->getPartitionDecomposition();
  decomp->doSplit(splits, this, cb);
}

void Reader::getAllSfcKeys(const CkCallback& cb)
{
  std::vector<Key> keys;
  for (const auto& p : particles)
    keys.push_back(p.key);

  contribute(keys.size() * sizeof(Key), &keys[0], CkReduction::set, cb);
}

void Reader::getAllPositions(const CkCallback& cb)
{
  std::vector<std::pair<int, Vector3D<Real>>> positions;
  for (const auto& p : particles)
    positions.emplace_back(p.partition_idx, p.position);

  contribute(positions.size() * sizeof(Vector3D<Real>), &positions[0], CkReduction::set, cb);
}

void Reader::pickSamples(const int oversampling_ratio, const CkCallback& cb) {
  Key* sample_keys = new Key[oversampling_ratio];

  // Not random, just equal intervals
  for (int i = 0; i < oversampling_ratio; i++) {
    int index = (particles.size() / (oversampling_ratio + 1)) * (i + 1);
    sample_keys[i] = particles[index].key;
  }

  // Accumulate samples
  contribute(sizeof(Key) * oversampling_ratio, sample_keys, CkReduction::concat, cb);
  delete[] sample_keys;
}

void Reader::prepMessages(const std::vector<Key>& splitter_keys, const CkCallback& cb) {
  // Place particles in respective buckets
  std::vector<std::vector<Particle>> send_vectors(n_readers);
  for (int i = 0; i < particles.size(); i++) {
    // Use upper bound splitter index to determine Reader index
    // [lower splitter, upper splitter)
    int bucket = Utility::binarySearchG(particles[i].key, &splitter_keys[0], 0, splitter_keys.size()) - 1;
    send_vectors[bucket].push_back(particles[i]);
  }

  // Prepare particle messages
  int old_total = particles.size();
  int new_total = 0;
  for (int bucket = 0; bucket < n_readers; bucket++) {
    int n_particles = send_vectors[bucket].size();
    new_total += n_particles;

    if (n_particles > 0) {
      ParticleMsg* msg = new (n_particles) ParticleMsg(&send_vectors[bucket][0], n_particles);
      particle_messages.push_back(msg);
    }
    else {
      particle_messages.push_back(NULL);
    }
  }

  // Check if all particles are assigned to buckets
  if (new_total != old_total)
    CkAbort("Failed to move all particles into buckets");

  contribute(cb);
}

void Reader::redistribute() {
  // Send particles home
  for (int bucket = 0; bucket < n_readers; bucket++) {
    if (particle_messages[bucket] != NULL) {
#if DEBUG
      CkPrintf("[Reader %d] Sending %d particles to Reader %d\n", thisIndex,
          particle_messages[bucket]->n_particles, bucket);
#endif
      thisProxy[bucket].receive(particle_messages[bucket]);
    }
  }
}

void Reader::receive(ParticleMsg* msg) {
  // Copy particles to local vector
  particles.resize(particle_index + msg->n_particles);
  std::memcpy(&particles[particle_index], msg->particles, msg->n_particles * sizeof(Particle));
  particle_index += msg->n_particles;
  delete msg;
  // SFCsplitters.push_back(Key(0)); // Maybe use something different than splitters variable?
}

void Reader::localSort(const CkCallback& cb) {
  std::sort(particles.begin(), particles.end());

  contribute(cb);
}

void Reader::checkSort(const Key last, const CkCallback& cb) {
  bool sorted = true;

  if (particles.size() > 0) {
    if (last > particles[0].key) {
      sorted = false;
    }
    else {
      for (int i = 0; i < particles.size()-1; i++) {
        if (particles[i] > particles[i+1]) {
          sorted = false;
          break;
        }
      }
    }
  }

  if (sorted)
    CkPrintf("[Reader %d] Particles sorted\n", thisIndex);
  else
    CkPrintf("[Reader %d] Particles NOT sorted\n", thisIndex);

  if (thisIndex < n_readers-1) {
    Key my_last;
    if (particles.size() > 0)
      my_last = particles.back().key;
    else
      my_last = Key(0);

    thisProxy[thisIndex+1].checkSort(my_last, cb);
    contribute(cb);
  }
  else {
    contribute(cb);
  }
}
