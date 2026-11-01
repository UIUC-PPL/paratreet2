#ifndef FOF_PLUGIN_API_H_
#define FOF_PLUGIN_API_H_

// FoF3 as an in-situ plug-in (design/plugin-interface.md): the interface a
// host Charm++ program includes. POD types only and no paratreet headers,
// so a host whose own chares share names with paratreet's (ChaNGa's
// TreePiece, Main) can include it next to its own generated headers.
//
// Link with `charmc -module fofplugin` (libmodulefofplugin.a), which
// registers the plug-in's Charm++ modules. Do not add `extern module
// fofplugin` to the host's .ci: the host's generated header would then
// include paratreet's. The host defines the `peanoKey` global the
// N-BodyShop structures library expects (ChaNGa already does).
//
// Sequence, per FoF call, with the host at a quiescent point (FoF uses
// global quiescence detection, which waits on the whole program):
//   1. on every PE holding particles: stage(...), possibly several times;
//   2. on PE 0, from a threaded entry: run(params, done) after whenReady;
//   3. after `done`, on each PE: labels() gives one label per staged
//      particle, in staging order; then clear() before the next call.

#include <cstddef>
#include <cstdint>
#include "charm++.h"

namespace fofplugin {

struct Particle {
  double x, y, z;
  uint64_t id;      // unique, < 2^43; the label is the group's minimum id
  float mass = 1.f;
  float vx = 0.f, vy = 0.f, vz = 0.f;
};

struct Params {
  // Linking length in the host's length units. If <= 0, b_factor times the
  // mean interparticle spacing of the staged particles' bounding cube, as
  // FoF3's -b does.
  double linking_length = 0.0;
  double b_factor = 0.2;
  // Periodic box side (same on every axis); 0 = not periodic.
  double period = 0.0;
  // Particles per tree leaf: FoF3 uses 128 on the GPU arm, 12 on the CPU.
  int leaf_size = 128;
  // UF_2 edge batches streamed during the walk (FoF3's -E).
  long stream_batch = 16;
  // Host decomposition: the particles were staged with stageCell, as
  // octree cells of the cube [universe_lo, universe_lo + universe_side)^3,
  // and FoF keeps each cell on the PE that staged it instead of
  // redistributing particles. Every particle must lie strictly inside.
  bool host_cells = false;
  double universe_lo[3] = {0, 0, 0};
  double universe_side = 0.0;
  // Halo catalog and minimum group size. With min_group_size > 1, particles
  // in smaller groups get label -1 ("no group"). With catalog, groups()
  // lists the groups of at least min_group_size particles.
  long min_group_size = 1;
  bool catalog = false;
};

// One halo, as groups() lists it. Under a period, x / lo / hi are in the
// image of one member and may extend past the box by the group's size.
struct Group {
  uint64_t id;        // minimum particle id: the label its particles carry
  uint64_t n;         // particles
  double mass;
  double x[3];        // centre of mass
  double v[3];        // mass-weighted mean velocity
  double lo[3], hi[3];  // bounding box
};

// Fires `cb` once the plug-in has finished startup (paratreet's
// initialization runs alongside the host's). Call on PE 0.
void whenReady(const CkCallback& cb);

// Copy n particles into the calling PE's staging area. Returns the staging
// index of the first: labels() reports them at the same indices.
size_t stage(const Particle* p, size_t n);

// As stage(), for host decomposition (Params::host_cells): the n particles
// form the octree cell (ix, iy, iz) at `level` (0 = the whole cube, side
// universe_side / 2^level). Cells may come in any number of calls and at
// mixed levels, but must not overlap; a cell staged on several PEs stays
// where most of its particles are. Missing regions become empty cells.
size_t stageCell(const Particle* p, size_t n, int level, uint32_t ix,
                 uint32_t iy, uint32_t iz);

// Run FoF over everything staged on all PEs. PE 0, threaded context.
void run(const Params& params, const CkCallback& done);

// After `done`: this PE's labels, one per staged particle in staging order.
// Each is the minimum id in that particle's group.
const long* labels(size_t& n);

// After `done`, with Params::catalog: the groups this PE holds the catalog
// row for. Every group is listed on exactly one PE.
const Group* groups(size_t& n);

// Forget this PE's staged particles, labels and catalog rows.
void clear();

// One particle's term in a label digest (FoF3's FOF3STAT label_digest):
// the wrapping sum of labelDigestTerm(id, label) over all particles does
// not depend on where they live, so hosts can compare it with an offline
// FoF3 run of the same particles.
inline uint64_t labelDigestTerm(long id, long label) {
  uint64_t z = uint64_t(id) * 0x9e3779b97f4a7c15ull ^ uint64_t(label);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

}  // namespace fofplugin

#endif  // FOF_PLUGIN_API_H_
