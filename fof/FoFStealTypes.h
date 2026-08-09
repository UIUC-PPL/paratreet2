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

// A shipment carries DEDUPLICATED subtree blobs plus one index pair per
// unit (ship-once, use-many). Pool units are child-level fragments of
// subtree pairs, so one child appears in up to eight units; flattening
// per unit re-serialized it every time, and that cost — 1.3 to 5.6 ms
// per unit, measured 2026-08-06 — was the ceiling on how fast a donor
// could shed work (design note section 11).
template <typename Data>
struct StealShipment {
  int victim_node = -1;
  std::vector<StealTree<Data>> trees;             // distinct subtrees
  std::vector<std::pair<int, int>> unit_pairs;    // indices into trees
  void pup(PUP::er& p) {
    p | victim_node;
    p | trees;
    p | unit_pairs;
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
  long flat_hits = 0;    // shipments served from the flatten memo
  long flat_misses = 0;  // distinct subtrees actually flattened
  long deny_not_ready = 0;
  long deny_not_needy = 0;
  long grants = 0;
  long probes_served = 0;
  long helper_rounds = 0;
  long helper_requests = 0;
  // Own work versus borrowed work, index 0 and 1 (design note section 16).
  long ex_units[2] = {0, 0};
  long ex_walk_us[2] = {0, 0};
  long ex_edges[2] = {0, 0};
  long ex_dupes[2] = {0, 0};
  long ex_cert_hit[2] = {0, 0};
  long ex_cert_miss[2] = {0, 0};
  long ex_star_parts[2] = {0, 0};
  long ex_shortcut[2] = {0, 0};
  long seen_resets = 0;
};

} // namespace paratreet

#endif // PARATREET_FOFSTEALTYPES_H_
