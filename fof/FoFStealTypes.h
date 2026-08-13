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

// One flattened subtree of a shipped phaseB pool unit: nodes in
// preorder, leaf particles appended in the same preorder. Structure is
// carried as integer offsets (parent index + child slot) instead of
// absent-slot marker records, so the receiver can rebuild with ONE
// arena allocation and a single linear pass — child/parent pointers are
// just arena + index (the varsize-message offset->pointer idiom; a
// literal in-buffer swizzle is barred by FullNode's vtable/atomics and
// would ship FullNode-sized slots, ~2x the bytes of this form).
template <typename Data>
struct StealTree {
  struct WireNode {
    Key key = Key(0);
    int32_t parent = -1;  // index into nodes[]; -1 = root
    int8_t slot = -1;     // child slot in parent
    SpatialNode<Data> sn;
    void pup(PUP::er& p) {
      p | key;
      p | parent;
      p | slot;
      p | sn;
    }
  };
  std::vector<WireNode> nodes;  // preorder; absent children omitted
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
