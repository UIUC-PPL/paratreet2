#ifndef PARATREET_DEVICETREE_H_
#define PARATREET_DEVICETREE_H_

// Flat, POD, pointer-free form of a TreePiece's local tree
// (design/phase1-gpu.md section 3.2; design/walk-unification.md stage 0).
//
// The live tree is device-hostile by construction: SpatialNode is
// polymorphic, Node::getChild is pure virtual over atomic child pointers,
// and every link is a host address. This is the same tree with dense
// integer indices instead — uploadable as one memcpy, traversable with a
// stack of ints, and (on the host) usable to key memos by node index
// instead of by pointer.
//
// Built once per tree build, from the walk that already runs there, so it
// never appears in a phase-1 wall. Data-agnostic: it reads only
// Data::box, which the Data concept already requires, and derives
// everything else from the tree's own structure.
//
// LAYOUT CONTRACT (what the device traversal relies on):
//   - index 0 is the root;
//   - a node's children are CONTIGUOUS at [child_begin, child_begin +
//     n_children), so a child is addressed by an add, not a lookup;
//   - children always have a HIGHER index than their parent (breadth
//     first), so a reverse linear scan is a valid bottom-up pass;
//   - EMPTY nodes are dropped entirely — every node here has n_below > 0;
//   - a node's particles are the contiguous range [part_begin,
//     part_begin + n_below) of the owning TreePiece's particle block,
//     which is what makes "union everything under this node" a flat loop
//     rather than a subtree walk.

#include "common.h"
#include "Node.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace paratreet {

struct DNode {
  float lo[3], hi[3];  // bounding box (Real is float; see src/common.h)
  int part_begin;      // first particle index in the TreePiece's block
  int n_below;         // particles under this node (> 0 always)
  int child_begin;     // index of first child, or -1 for a leaf
  int n_children;      // children, contiguous from child_begin
};
static_assert(sizeof(DNode) == 40, "DNode must stay a tight POD");

// Off by default; PARATREET_DEVICE_TREE=1 enables the build-time emit.
// A/B arm for the stage-1 gate ("CPU results unchanged"), and the switch
// that keeps the cost out of CPU-only runs.
inline bool deviceTreeEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("PARATREET_DEVICE_TREE");
    return e && std::atoi(e) != 0;
  }();
  return on;
}

// PARATREET_DEVICE_TREE_VERIFY=1 checks every flat tree against the live
// tree it came from as it is built (verifyFlatTree below). Cheap enough
// to leave on in regression runs, and it is the stage-1 gate.
inline bool deviceTreeVerify() {
  static const bool on = [] {
    const char* e = std::getenv("PARATREET_DEVICE_TREE_VERIFY");
    return e && std::atoi(e) != 0;
  }();
  return on;
}

// Flatten root's subtree into out. `base` is the TreePiece's particle
// block, so part_begin is relative to it. Call AFTER populateTree(): node
// boxes are accumulated bottom-up there, and empty-node pruning needs
// n_particles to be settled.
template <typename Data>
void flattenTree(Node<Data>* root, const Particle* base,
                 std::vector<DNode>& out) {
  out.clear();
  if (root == nullptr || root->n_particles == 0) return;

  // Parallel array of the source pointers, alive only during the build.
  std::vector<Node<Data>*> src;
  src.reserve(64);
  out.reserve(64);

  auto emit = [&](Node<Data>* n) {
    DNode d;
    const auto& b = n->data.box;
    d.lo[0] = (float)b.lesser_corner.x;
    d.lo[1] = (float)b.lesser_corner.y;
    d.lo[2] = (float)b.lesser_corner.z;
    d.hi[0] = (float)b.greater_corner.x;
    d.hi[1] = (float)b.greater_corner.y;
    d.hi[2] = (float)b.greater_corner.z;
    d.child_begin = -1;
    d.n_children = 0;
    if (n->isLeaf()) {
      d.part_begin = (int)(n->particles() - base);
      d.n_below = n->n_particles;
    } else {
      d.part_begin = -1;  // filled by the reverse pass
      d.n_below = 0;      // ditto
    }
    out.push_back(d);
    src.push_back(n);
  };

  emit(root);
  // Breadth first, which is what makes children contiguous AND ordered
  // after their parent. Both vectors grow as the loop runs.
  for (size_t i = 0; i < out.size(); i++) {
    Node<Data>* n = src[i];
    if (n->isLeaf()) continue;
    const int begin = (int)out.size();
    int k = 0;
    for (int c = 0; c < n->n_children; c++) {
      Node<Data>* ch = n->getChild(c);
      // n_particles is -1 on internal nodes BY DESIGN (Node.h), so the
      // test for "has no particles" must be == 0, never <= 0. Getting
      // this wrong drops every internal node and silently produces a
      // tree of depth 1 (the same rule firstFlat learned the hard way,
      // FoFPhase1.h).
      if (ch == nullptr || ch->n_particles == 0) continue;
      emit(ch);
      k++;
    }
    if (k) {
      out[i].child_begin = begin;
      out[i].n_children = k;
    }
  }

  // Bottom-up over the flat array: a reverse linear scan, contiguous and
  // pointer-free, instead of a second recursive descent. Children are
  // key-ordered and particles are key-sorted, so the first child holds
  // the lowest particle offset.
  for (int i = (int)out.size() - 1; i >= 0; i--) {
    if (out[i].child_begin < 0) continue;  // leaf: already final
    int total = 0;
    for (int c = 0; c < out[i].n_children; c++)
      total += out[out[i].child_begin + c].n_below;
    out[i].n_below = total;
    out[i].part_begin = out[out[i].child_begin].part_begin;
  }
}

// Structural check of a flat tree against the live tree it came from.
// Not a debug print: this is the stage-1 gate, and it is what catches an
// off-by-one in part_begin — which would otherwise show up much later as
// a wrong FoF label on a handful of particles.
template <typename Data>
bool verifyFlatTree(Node<Data>* root, const Particle* base,
                    const std::vector<DNode>& flat, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  if (root == nullptr || root->n_particles == 0) return flat.empty();
  if (flat.empty()) return fail("flat tree empty for a non-empty root");

  // Walk both together. `fi` is the flat index corresponding to `n`.
  std::vector<std::pair<Node<Data>*, int> > stack;
  stack.push_back(std::make_pair(root, 0));
  long leaf_particles = 0;
  int visited = 0;
  while (!stack.empty()) {
    Node<Data>* n = stack.back().first;
    const int fi = stack.back().second;
    stack.pop_back();
    visited++;
    if (fi < 0 || fi >= (int)flat.size())
      return fail("flat index out of range");
    const DNode& d = flat[fi];

    if (d.n_below <= 0) return fail("node with no particles was emitted");
    const auto& b = n->data.box;
    if (d.lo[0] != (float)b.lesser_corner.x ||
        d.hi[2] != (float)b.greater_corner.z)
      return fail("box mismatch at flat index " + std::to_string(fi));

    if (n->isLeaf()) {
      if (d.child_begin != -1) return fail("leaf carries children");
      if (d.n_below != n->n_particles) return fail("leaf n_below mismatch");
      if (d.part_begin != (int)(n->particles() - base))
        return fail("leaf part_begin mismatch");
      leaf_particles += d.n_below;
      continue;
    }
    int k = 0;
    for (int c = 0; c < n->n_children; c++) {
      Node<Data>* ch = n->getChild(c);
      if (ch == nullptr || ch->n_particles == 0) continue;
      if (k >= d.n_children) return fail("fewer children emitted than live");
      stack.push_back(std::make_pair(ch, d.child_begin + k));
      k++;
    }
    if (k != d.n_children) return fail("child count mismatch");
  }
  if (visited != (int)flat.size())
    return fail("flat tree has " + std::to_string((int)flat.size() - visited) +
                " unreachable nodes");
  if (leaf_particles != flat[0].n_below)
    return fail("root n_below (" + std::to_string(flat[0].n_below) +
                ") != particles under the leaves (" +
                std::to_string(leaf_particles) + ")");
  return true;
}

}  // namespace paratreet

#endif  // PARATREET_DEVICETREE_H_
