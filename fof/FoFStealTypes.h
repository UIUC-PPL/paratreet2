#ifndef PARATREET_FOFSTEALTYPES_H_
#define PARATREET_FOFSTEALTYPES_H_

// Shipment types for S3 coordinator-mediated phaseB stealing
// (design/phaseab-balancing.md section 19). Ported from the phaseb-steal
// branch (FoFStealTypes.h there): the flatten/rebuild transport survived
// that branch's post-mortem intact — what S3 replaces is the
// helper-initiated request/grant protocol, not the data plane. Defined
// ahead of fof.decl.h because the generated entry declarations reference
// these types.

#include "common.h"
#include "Node.h"
#include "Particle.h"

#include <utility>
#include <vector>

namespace paratreet {

// One flattened subtree of a shipped phaseB pool unit: nodes in preorder,
// key 0 marking an absent child slot (real keys start at 1), leaf
// particles appended in the same preorder.
template <typename Data>
struct StealTree {
  std::vector<std::pair<Key, SpatialNode<Data>>> nodes;
  std::vector<Particle> particles;
  void pup(PUP::er& p) {
    p | nodes;
    p | particles;
  }
};

// A shipment carries DEDUPLICATED subtree blobs plus one index pair per
// unit (ship-once, use-many; a child node appears in up to eight units,
// and per-unit flattening measured 1.3-5.6 ms — the old branch's grant
// ceiling). Under S3 a shipment is one KD partition's unclaimed units.
template <typename Data>
struct StealShipment {
  int origin_node = -1;   // process whose tips these edges describe
  int part_idx = -1;      // donor partition (bookkeeping/stats)
  double m2 = 0;          // predicted cost shipped (stats)
  std::vector<StealTree<Data>> trees;             // distinct subtrees
  std::vector<std::pair<int, int>> unit_pairs;    // indices into trees
  void pup(PUP::er& p) {
    p | origin_node;
    p | part_idx;
    p | m2;
    p | trees;
    p | unit_pairs;
  }
};

} // namespace paratreet

#endif // PARATREET_FOFSTEALTYPES_H_
