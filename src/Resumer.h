#ifndef PARATREET_RESUMER_H_
#define PARATREET_RESUMER_H_

#include "paratreet.decl.h"
#include "common.h"
#include "Partition.h"

#include <queue>
#include <set>
#include <vector>

// Per-processor resumption POLICY for parked traversals (phase 2 of
// design/smp-cache-extraction.md): the WAITING STATE now lives on the
// cache placeholders themselves (TreeCache park/install), and this group
// only decides what to do with the opaques an install hands back — queue
// the resumed node per (traversal, element) and dispatch one goDown to
// the walking chare (Subtree or Partition, per use_subtree). The old
// waiting[key] side table is gone.
template <typename Data>
class Resumer : public CBase_Resumer<Data> {
public: // these need to be seen by other local chares
  CProxy_Partition<Data> part_proxy;
  CProxy_Subtree<Data> subtree_proxy;
  CacheManager<Data>* cm_local;
  std::map<std::pair<int, int>, std::queue<Node<Data>*>> all_resume_nodes;
  bool use_subtree = false;

  void reset() {
#if DEBUG
    // (Brace bit-rot fixed 2026-08-04: the block previously closed reset()
    // early and only compiled because DEBUG=0 everywhere.)
    for (auto& trav : all_resume_nodes) {
      if (!trav.second.empty()) CkAbort("did not complete last traversal");
    }
#endif
    all_resume_nodes.clear();
  }

  // Deliver an install's parked opaques for this lane: key identifies the
  // installed node (looked up in the shared cache tree), each opaque names
  // a (traversal, element) to resume. Duplicates within one delivery are
  // collapsed (the old waiting list kept one entry per pair).
  void process(Key key, const std::vector<uint64_t>& opaques) {
    auto node = cm_local->root->getDescendant(key);
    CkAssert(node && node->key == key);
    std::set<uint64_t> seen;
    for (auto o : opaques) {
      if (!seen.insert(o).second) continue;
      std::pair<int, int> pair(paratreet::parkedTravIdx(o),
                               paratreet::parkedElemIdx(o));
      auto && resume_nodes = all_resume_nodes[pair];
      bool should_resume = resume_nodes.empty();
      resume_nodes.push(node);
      if (should_resume) {
        // Credit for the goDown message (an in-flight goDown drains the
        // whole queue, so entries pushed onto a non-empty queue ride the
        // credit that goDown already holds).
        cm_local->walkCreditInc(1);
        if (use_subtree) subtree_proxy[pair.second].goDown(pair.first);
        else part_proxy[pair.second].goDown(pair.first);
      }
    }
    // This process() message's own credit (added by the sender before
    // the send); any continuation was transferred above.
    cm_local->walkCreditDec(1);
  }
};

#endif // PARATREET_RESUMER_H_
