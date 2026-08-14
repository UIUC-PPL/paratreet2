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

#include <type_traits>
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
  // POD mirror of exactly the SpatialNode fields SpatialNode::pup ships.
  // Kale's observation (2026-08-13): every real node is FullNode<Data,8>, so
  // the WIRE never needs polymorphism -- yet embedding SpatialNode by value
  // dragged in its `virtual ~SpatialNode()`, which (a) put a vtable pointer
  // in every wire node and (b) blocked bulk serialization, forcing ~15 field
  // pups per node. Frontier measured the per-element pack at ~242 ns/element
  // against ~8 ns/node for an equivalent bulk copy.
  // Laptop vetting note (2026-08-14): embedding Data (FragData) by value
  // re-imported a vtable through a side door — OrientedBox inherits
  // Shape, whose `virtual ~Shape()` puts a vptr inside FragData. The
  // bulk copy then ships raw vptr bytes (dead weight, and undefined
  // behaviour to memcpy). So the wire carries a POD mirror of exactly
  // what FragData::pup + SpatialNode::pup shipped: the box corners and
  // the three longs, plus SpatialNode's four scalars. This is
  // FoF-specific knowledge in a Data-templated struct — fine while the
  // fof module instantiates Data = FragData only (the static_asserts
  // below fail the build if that ever stops being true in a way that
  // matters).
  struct WireSpatial {
    Vector3D<Real> box_lesser, box_greater;  // OrientedBox corners
    long min_frag = 0;
    long max_frag = 0;
    long n_below = 0;
    int n_particles = -1;      // -1 on internal nodes, by design
    int depth = -1;
    uint64_t particle_min_index = UINT64_MAX;
    uint64_t particle_max_index = 0;
  };
  struct WireNode {
    Key key = Key(0);
    int32_t parent = -1;  // index into nodes[]; -1 = root
    int8_t slot = -1;     // child slot in parent
    WireSpatial sp;
  };
  std::vector<WireNode> nodes;  // preorder; absent children omitted
  std::vector<Particle> particles;
  // BULK. WireNode and Particle are now trivially copyable (no vtable, no
  // pointers -- parent/slot are already integer offsets from b797e73), so the
  // whole array moves in one raw-bytes call instead of per-element dispatch.
  // The raw-bytes pup below is legal ONLY while these hold; if Data (or
  // Particle) ever grows a heap member or virtual, this must become a
  // per-element pup again — fail the build rather than ship garbage.
  static_assert(std::is_trivially_copyable<WireNode>::value,
                "WireNode must stay trivially copyable for the bulk pup");
  static_assert(std::is_trivially_copyable<Particle>::value,
                "Particle must stay trivially copyable for the bulk pup");
  void pup(PUP::er& p) {
    size_t nn = nodes.size(), np = particles.size();
    p | nn;
    p | np;
    if (p.isUnpacking()) { nodes.resize(nn); particles.resize(np); }
    if (nn) p((char*)nodes.data(), nn * sizeof(WireNode));
    if (np) p((char*)particles.data(), np * sizeof(Particle));
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
