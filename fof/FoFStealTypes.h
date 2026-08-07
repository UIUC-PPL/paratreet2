#ifndef PARATREET_FOFSTEALTYPES_H_
#define PARATREET_FOFSTEALTYPES_H_

// Shipment types for phaseB cross-process stealing (design/
// phaseb-offload.md stages 1-2). Defined ahead of fof.decl.h because the
// generated entry declarations reference them.

#include "common.h"
#include "Node.h"
#include "Particle.h"

#include <utility>
#include <vector>

namespace paratreet {

// One flattened tree of a stolen phaseB pool unit: nodes in preorder,
// key 0 marking an absent child slot (real keys start at 1), leaf
// particles appended in the same preorder. Trees come in pairs:
// trees[2k] and trees[2k+1] are unit k's two sides.
template <typename Data>
struct StealTree {
  std::vector<std::pair<Key, SpatialNode<Data>>> nodes;
  std::vector<Particle> particles;
  void pup(PUP::er& p) {
    p | nodes;
    p | particles;
  }
};

template <typename Data>
struct StealShipment {
  int victim_node = -1;
  std::vector<StealTree<Data>> trees; // 2 per unit
  void pup(PUP::er& p) {
    p | victim_node;
    p | trees;
  }
};

// Per-process phaseB accounting record (gathered after the phase).
struct StealAcct {
  int process = -1;
  long pool_units = 0;
  double wall_b = 0;
  long out_units = 0;
  long in_units = 0;
  long denials = 0;
  double flatten_ms = 0;
};

} // namespace paratreet

#endif // PARATREET_FOFSTEALTYPES_H_
