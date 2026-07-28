#ifndef PARATREET_MULTIDATA_H_
#define PARATREET_MULTIDATA_H_

#include "Particle.h"
#include "Node.h"
#include "common.h"
#include "paratreet.decl.h"

#include <vector>
#include <iterator>
#include <utility>

// Remote-particle slimming (design/cached-particle-slimming.md): the
// particles of cache-shipped subtree copies are CONVERTED ONCE here, at
// pack time, into the application's CachedParticle type (see
// CachedParticleOf in Particle.h) and stay in that type on the wire AND
// in cached-leaf storage. For a non-opting app CachedParticle is
// Particle and behavior is byte-identical to the unparameterized
// framework. CONTRACT for an opting app: cached copies carry ONLY the
// fields CachedParticle keeps — code paths that need full particles
// from cached leaves (resetCachedParticles, refreshSubtreeCopy's
// particle refresh) abort with a clear message for opting apps; the
// OWNED-particle exchange (ParticleMsg: decomposition flush,
// Partition<->Subtree routing) is a different path and always ships
// full particles. This supersedes the older pupRemoteParticle wire-only
// mechanism (removed).
template <typename Data>
struct MultiData {
  using CachedP = typename CachedParticleOf<Data>::type;
  std::vector<CachedP> particles;
  std::vector<std::pair<Key, SpatialNode<Data>>> nodes;
  int cm_index = -1;
  int tp_index = -1;

  MultiData();
  MultiData(const Particle*, int, Node<Data>**, int, int, int);
  // Assign from full particles, converting once (same conversion point
  // semantics as the constructor). Used by Subtree's flat_subtree.
  void setParticles(const std::vector<Particle>& src) {
    particles.clear();
    particles.reserve(src.size());
    for (const auto& p : src) particles.emplace_back(CachedP(p));
  }
  void pup(PUP::er& p);
  void clear();
};

template <typename Data>
MultiData<Data>::MultiData() {}

template <typename Data>
inline MultiData<Data>::MultiData(const Particle* particlesi, int n_particles, Node<Data>** nodesi, int n_nodes, int cm_indexi, int tp_indexi) {
  cm_index      = cm_indexi;
  tp_index      = tp_indexi;
  particles.reserve(n_particles);
  for (int i = 0; i < n_particles; i++)
    particles.emplace_back(CachedP(particlesi[i])); // the ONE conversion point
  std::transform(nodesi, nodesi + n_nodes, std::back_inserter(nodes), [] (Node<Data>* node) {
    SpatialNode<Data> copy = *node;
    return std::make_pair(node->key, copy);
  });
}

template <typename Data>
void MultiData<Data>::pup(PUP::er& p) {
  p | particles; // CachedP on the wire (== Particle for non-opting apps)
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
