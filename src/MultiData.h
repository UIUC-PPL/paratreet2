#ifndef PARATREET_MULTIDATA_H_
#define PARATREET_MULTIDATA_H_

#include "Particle.h"
#include "Node.h"
#include "common.h"
#include "paratreet.decl.h"

#include <vector>
#include <iterator>
#include <utility>

// Opt-in remote-particle slimming (same compile-time opt-in idiom as
// maybeSetKeys): a Data type that defines
//     static void pupRemoteParticle(PUP::er&, Particle&);
// has the particles of cache-shipped subtree copies serialized by that
// function instead of the full Particle pup, cutting wire volume for
// traversals that read only a few fields from REMOTE particles (FoF:
// position + group_number = 20 of ~112 bytes, ~5x). Receivers reconstruct
// full Particle objects whose unshipped fields hold default values, so
// cached-leaf storage and every particle type are unchanged (cache MEMORY
// reduction is a separate, deeper change). CONTRACT for an opting-in app:
// nothing may read an unshipped field from a cache-shipped particle — that
// includes visitors AND cache-lifecycle machinery (resetCachedParticles
// reads key/partition_idx on cached leaves in multi-iteration flows; FoF
// is single-iteration and its walk reads only the two shipped fields).
// The OWNED-particle exchange (ParticleMsg: decomposition flush,
// Partition<->Subtree routing) is a different path and always ships full
// particles.
template <typename D>
inline auto pupParticlesDispatch(PUP::er& p, std::vector<Particle>& v, int)
    -> decltype(D::pupRemoteParticle(std::declval<PUP::er&>(),
                                     std::declval<Particle&>()),
                void()) {
  size_t n = v.size();
  p | n;
  if (p.isUnpacking()) v.assign(n, Particle());
  for (auto& part : v) D::pupRemoteParticle(p, part);
}
template <typename D>
inline void pupParticlesDispatch(PUP::er& p, std::vector<Particle>& v, long) {
  p | v; // no opt-in: full particles
}

template <typename Data>
struct MultiData {
  std::vector<Particle> particles;
  std::vector<std::pair<Key, SpatialNode<Data>>> nodes;
  int cm_index = -1;
  int tp_index = -1;

  MultiData();
  MultiData(Particle*, int, Node<Data>**, int, int, int);
  void pup(PUP::er& p);
  void clear();
};

template <typename Data>
MultiData<Data>::MultiData() {}

template <typename Data>
inline MultiData<Data>::MultiData(Particle* particlesi, int n_particles, Node<Data>** nodesi, int n_nodes, int cm_indexi, int tp_indexi) {
  cm_index      = cm_indexi;
  tp_index      = tp_indexi;
  std::copy(particlesi, particlesi + n_particles, std::back_inserter(particles));
  std::transform(nodesi, nodesi + n_nodes, std::back_inserter(nodes), [] (Node<Data>* node) {
    SpatialNode<Data> copy = *node;
    return std::make_pair(node->key, copy);
  });
}

template <typename Data>
void MultiData<Data>::pup(PUP::er& p) {
  pupParticlesDispatch<Data>(p, particles, 0); // slim if Data opts in
  p | nodes;
  p | cm_index;
  p | tp_index;
}

template <typename Data>
void MultiData<Data>::clear() {
  nodes.clear();
  particles.clear();
}

#endif // PARATREET_MULTIDATA_H_
