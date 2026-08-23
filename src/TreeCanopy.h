#ifndef PARATREET_TREECANOPY_H_
#define PARATREET_TREECANOPY_H_

#include "paratreet.decl.h"
#include "templates.h"
#include "Node.h"
#include "CacheManager.h"

template<typename Data>
class CProxy_TreePiece;

template<typename Data>
class CProxy_CacheManager;

template <typename Data>
class TreeCanopy : public CBase_TreeCanopy<Data> {
private:
  SpatialNode<Data> my_sn;
  int recv_count = 0;
  int tp_index; // If -1, sits above TreePieces
  CProxy_TreePiece<Data> tp_proxy;
  CProxy_CacheManager<Data> cm_proxy;
  CProxy_Driver<Data> d_proxy;
public:
  TreeCanopy() {
    this->setMigratable(false); // Disable migration for TreeCanopy objects
  }
  TreeCanopy(CkMigrateMessage * msg) {
    this->setMigratable(false); // Disable migration for TreeCanopy objects
    delete msg;
  };
  void reset();
  void recvProxies(TPHolder<Data>, int, CProxy_CacheManager<Data>, DPHolder<Data>);
  void recvData(SpatialNode<Data>, int);
  void requestData(int);
  void pup(PUP::er& p);
};

template <typename Data>
void TreeCanopy<Data>::reset() {
  recv_count = 0;
}

template <typename Data>
void TreeCanopy<Data>::recvProxies(TPHolder<Data> tp_holder, int tp_index_,
                                   CProxy_CacheManager<Data> cm_proxy_, DPHolder<Data> dp_holder) {
  tp_proxy = tp_holder.proxy;
  tp_index = tp_index_;
  cm_proxy = cm_proxy_;
  d_proxy = dp_holder.proxy;
}

// Collect-side companion to the -s cap (num_share_nodes). Driver::loadCache
// caps what it SHIPS, but every canopy element messages the driver
// regardless, so all of them arrive and are sorted before anything is
// capped — 34,835 x 2 = 69,670 point-to-point messages onto one PE at 128
// nodes (relay95), of which the shipped prefix is a few hundred.
//
// The element index IS the prefix-coded key (parent = index / branch_factor),
// and Driver::sortStorage sorts by key, so the shipped prefix is exactly the
// smallest indices — i.e. the shallowest levels. Keeping only indices below a
// power of branch_factor therefore keeps a whole number of levels and cannot
// drop anything the ship would have used before it uses everything above it.
//
// Under-collecting is a PERFORMANCE question, not a correctness one: a
// process that lacks a canopy entry fetches it during the walk (verified on
// the laptop — exact at every cap down to -s 1). Unset (-s 0) keeps today's
// behaviour: no gate, every canopy reports.
static inline uint64_t canopyCollectLimit(int branch_factor) {
  const long n = paratreet::getConfiguration().num_share_nodes;
  if (n <= 0 || branch_factor < 2) return 0;   // unset: collect everything
  // Keep whole levels: the smallest power of b whose complete-tree prefix
  // (b^(d+1)-1)/(b-1) is at least n, i.e. b^(d+1) >= n*(b-1)+1.
  const uint64_t want = (uint64_t)n * (uint64_t)(branch_factor - 1) + 1;
  uint64_t limit = 1;
  while (limit < want) {
    const uint64_t next = limit * (uint64_t)branch_factor;
    if (next < limit) return 0;               // overflow: collect everything
    limit = next;
  }
  return limit;                                // keep indices < limit
}

template <typename Data>
void TreeCanopy<Data>::recvData(SpatialNode<Data> child, int branch_factor) {
  // Starting a fresh accumulation round: clear data left from the
  // previous round (initial build or a later upwardPass), which was
  // never reset and would otherwise double-accumulate
  if (recv_count == 0) my_sn.data = Data();
  // Accumulate data received from TreePiece or children TreeCanopies
  my_sn.data += child.data;
  my_sn.depth = child.depth - 1;

  // If data from all children has been received, send the accumulated data
  // to Driver and to the parent TreeCanopy
  if (++recv_count == branch_factor) {
    // Only the levels the ship can actually use are collected (see
    // canopyCollectLimit). The upward aggregation below is UNGATED — it is
    // how data reaches the root and must run for every element.
    const uint64_t collect_limit = canopyCollectLimit(branch_factor);
    if (collect_limit == 0 || (uint64_t)this->thisIndex < collect_limit)
      d_proxy.recvTC(std::make_pair(this->thisIndex, my_sn));

    if (this->thisIndex == 1) {
      //cm_proxy.restoreData(std::make_pair(1, data));
    } else {
      this->thisProxy[this->thisIndex / branch_factor].recvData(my_sn, branch_factor);
    }

    reset();
  }
}

template <typename Data>
void TreeCanopy<Data>::requestData(int cm_index) {
  if (tp_index >= 0) tp_proxy[tp_index].requestNodes(this->thisIndex, cm_index);
  else cm_proxy[cm_index].restoreData(std::make_pair(this->thisIndex, my_sn));
}

template <typename Data>
void TreeCanopy<Data>::pup(PUP::er& p) {
  p | tp_proxy;
  p | tp_index;
  p | cm_proxy;
  p | d_proxy;
  p | my_sn;
  p | recv_count;
}
#endif // PARATREET_TREECANOPY_H_
