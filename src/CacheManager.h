#ifndef PARATREET_CACHEMANAGER_H_
#define PARATREET_CACHEMANAGER_H_

#include "paratreet.decl.h"
#include <type_traits>
#include "common.h"
#include "Utility.h"
#include "templates.h"
#include "MultiData.h"
#include "TreeCache.h"

#include <map>
#include <unordered_map>
#include <vector>

extern CProxy_TreeSpec treespec;

// The Charm++ shim over the passive TreeCache core (phase 1 of
// design/smp-cache-extraction.md). This nodegroup owns everything the
// core deliberately excludes: the entry methods and message transport
// (node requests, replies, starter packs, TreePiece copies), the Resumer
// wake-up policy (process()), and the Partition registry. All tree/pool/
// registry state and the install/placeholder discipline live in `core`;
// the public reference aliases below keep the historical member names
// (cm_local->root etc.) valid for TreePiece/Partition/Resumer/Traverser.
//
// CONCURRENCY: see the design note at the top of TreeCache.h — lock-free
// hot path, narrow maps_lock, phase-separated mutation. Do not add locks
// here or there.
template <typename Data>
class CacheManager : public CBase_CacheManager<Data> {
public:
  TreeCache<Data> core;
  using CachedP = typename TreeCache<Data>::CachedP;

  // Historical member names, aliased into the core (this chare is never
  // migrated/pupped, so reference members are safe).
  Node<Data>*& root = core.root;
  typename TreeCache<Data>::NodeLookup& local_tps = core.local_tps;
  typename TreeCache<Data>::NodeLookup& leaf_lookup = core.leaf_lookup;
  std::map<Key, std::vector<int>>& treepiece_copy_started = core.treepiece_copy_started;

  std::map<int, Partition<Data>*> partition_lookup; // managed by Partition
  CProxy_Resumer<Data> r_proxy;

  CacheManager() { }

  void initialize(const CkCallback& cb) {
    auto node_size = this->isNodeGroup() ? CmiNodeSize(CkMyNode()) : 1;
    auto& config = paratreet::getConfiguration();
    core.init(node_size, this->isNodeGroup(), config.branchFactor(),
              std::max(config.pool_elem_size, 128));
    this->contribute(cb);
  }

  void lockMaps() { core.lockMaps(); }
  void unlockMaps() { core.unlockMaps(); }

  // Cache memory accounting (app-agnostic), per process. Call at a phase
  // boundary (quiescence-separated from fills). Contributes a 2-element
  // tuple: sums of {pool_bytes, cached_nodes, cached_leaves,
  // cached_particles, total_bytes} and the max per-process total_bytes
  // (skew).
  void cacheStats(const CkCallback& cb) {
    auto s = core.stats();
    // used_nodes (pool slots incl. PLACEHOLDERS) is reported alongside
    // cached_nodes (fetched content): their difference is the frontier
    // placeholder population, which is what makes a deeper -D cost more
    // requests than it saves (relay91).
    long sums[6] = {s.pool_bytes, s.cached_nodes, s.cached_leaves,
                    s.cached_particles, s.total_bytes, s.used_nodes};
    CkReduction::tupleElement elems[2] = {
        CkReduction::tupleElement(sizeof(sums), sums, CkReduction::sum_long),
        CkReduction::tupleElement(sizeof(long), &s.total_bytes,
                                  CkReduction::max_long)};
    CkReductionMsg* msg = CkReductionMsg::buildFromTuple(elems, 2);
    msg->setCallback(cb);
    this->contribute(msg);
  }

  void addDisplacedLeaf(Node<Data>* leaf) {
    core.addDisplacedLeaf(CkMyRank(), leaf);
  }

  void resetCachedParticles(PPHolder<Data> pp_holder) {
    std::map<int, std::vector<Key>> partitions_to_request;
    core.resetCachedParticles(CkMyRank(), partitions_to_request);
    for (auto& pair : partitions_to_request) {
      pp_holder.proxy[pair.first].requestParticleUpdates(this->thisIndex, pair.second);
    }
  }

  void receiveParticleUpdates(const std::vector<Particle>& particles_received) {
    size_t replaced = core.receiveParticleUpdates(particles_received);
    if (replaced != particles_received.size()) CkAbort("broken");
  }

  void destroy(bool restore) {
    core.destroy();
  }

  Node<Data>* makeNode(Key key, typename Node<Data>::Type type, int depth, int n_particles, Particle* particles, Node<Data>* parent, int tp_index, int cm_index) {
    return core.makeNode(CkMyRank(), key, type, depth, n_particles, particles, parent, tp_index, cm_index);
  }

  template <typename Visitor>
  void startPrefetch(DPHolder<Data>, CkCallback);
  void startParentPrefetch(DPHolder<Data>, CkCallback);
  void requestNodes(std::pair<Key, int>);
  void serviceRequest(Node<Data>*, int);
  void recvStarterPack(std::pair<Key, SpatialNode<Data>>* pack, int n, CkCallback);
  void addCache(MultiData<Data>);
  void receiveTreePiece(MultiData<Data>, PPHolder<Data>);
  void refreshTreePieceCopy(MultiData<Data>);
  void restoreData(std::pair<Key, SpatialNode<Data>>);
  void connect(Node<Data>*);

  // --- Walk-completion credit counter (see TreeCache::walk_credits).
  // armWalkCompletion runs on every process BEFORE the walk broadcast
  // (the caller waits on `ack`): counter := 1 (the seeding open credit),
  // done callback stored. Each walk element contributes to an array
  // reduction after its seeding descent; that reduction broadcasts
  // releaseOpenCredit, so zero is unreachable until every element has
  // seeded. fireWalkDone contributes each process's branch into the
  // done reduction exactly once (the armed->fired exchange guards it).
  void armWalkCompletion(const CkCallback& done, const CkCallback& ack) {
    walk_done_cb_ = done;
    core.walk_credits.store(1, std::memory_order_relaxed);
    core.walk_state.store(1, std::memory_order_release);
    this->contribute(ack);
  }
  void releaseOpenCredit() { walkCreditDec(1); }
  void fireWalkDone() { this->contribute(walk_done_cb_); }
  bool walkArmed() const {
    return core.walk_state.load(std::memory_order_acquire) == 1;
  }
  void walkCreditInc(long n) {
    if (walkArmed()) core.walk_credits.fetch_add(n, std::memory_order_acq_rel);
  }
  void walkCreditDec(long n) {
    if (!walkArmed()) return;
    if (core.walk_credits.fetch_sub(n, std::memory_order_acq_rel) == n) {
      int expect = 1;
      if (core.walk_state.compare_exchange_strong(expect, 2))
        this->thisProxy[this->thisIndex].fireWalkDone();
    }
  }

private:
  CkCallback walk_done_cb_;
  void process(Node<Data>*, const std::vector<uint64_t>& parked);
};

template <typename Data>
template <typename Visitor>
void CacheManager<Data>::startPrefetch(DPHolder<Data> dp_holder, CkCallback cb) {
  dp_holder.proxy.template prefetch<Visitor>(core.nodewide_data, this->thisIndex, cb);
}

template <typename Data>
void CacheManager<Data>::startParentPrefetch(DPHolder<Data> dp_holder, CkCallback cb) {
  std::vector<Key> request_list (core.prefetch_set.begin(), core.prefetch_set.end());
  dp_holder.proxy.request(request_list.data(), request_list.size(), this->thisIndex, cb);
}

// Store/connect an incoming TreePiece's local root (TreePiece::initCache).
template <typename Data>
void CacheManager<Data>::connect(Node<Data>* node) {
  core.connectRoot(node);
  // XXX: May need to call process() for dual tree walk
}

template <typename Data>
void CacheManager<Data>::recvStarterPack(std::pair<Key, SpatialNode<Data>>* pack, int n, CkCallback cb) {
#if !DEBUG
  if (this->thisIndex == 0)
#endif
  CkPrintf("[CacheManager %d] receiving starter pack, size = %d\n", this->thisIndex, n);

  CkAssert(n == 0 || pack[0].first == Key(1));
  std::vector<uint64_t> parked; // starter pack precedes all walks: stays empty
  for (int i = 0; i < n; i++) {
    // uncomment conditional if prefetch() is ever restored
    // if (!local_tps.count(pack[i].first))
    core.installBoundary(CkMyRank(), pack[i], parked);
  }
  if (n == 0) root = local_tps[1];
  CkAssert(root);
  this->contribute(cb);
}

template <typename Data>
void CacheManager<Data>::receiveTreePiece(MultiData<Data> multidata, PPHolder<Data> pp_holder) {
  std::vector<uint64_t> parked; // add_to_tps path never displaces a placeholder
  core.installSubtree(CkMyRank(), multidata.particles.data(), multidata.particles.size(),
                      multidata.nodes.data(), multidata.nodes.size(),
                      multidata.cm_index, multidata.tp_index, true, parked);
  lockMaps();
  auto copy_out = treepiece_copy_started[multidata.tp_index];
  unlockMaps();
  for (auto && partition : copy_out) {
    pp_holder.proxy[partition].makeLeaves(multidata.tp_index);
  }
}

// In-place refresh of a shipped TreePiece copy; see TreeCache::
// refreshTreePieceCopy for the phase-separation contract (callers run this
// quiescence-separated from traversals, before loadCache/startDown — see
// examples/annotate preTraversalFn).
template <typename Data>
void CacheManager<Data>::refreshTreePieceCopy(MultiData<Data> multidata) {
  core.refreshTreePieceCopy(multidata.particles.data(),
                          multidata.nodes.data(), multidata.nodes.size());
}

template <typename Data>
void CacheManager<Data>::addCache(MultiData<Data> multidata) {
  std::vector<uint64_t> parked;
  Node<Data>* top_node =
      core.installSubtree(CkMyRank(), multidata.particles.data(), multidata.particles.size(),
                          multidata.nodes.data(), multidata.nodes.size(),
                          multidata.cm_index, multidata.tp_index, false, parked);
  process(top_node, parked);
  // The request that this reply answers held one walk credit (added at
  // issue in handleRemoteNode); the waiters' resume credits were added
  // inside process() above, so removing the request credit here cannot
  // expose a transient zero.
  walkCreditDec(1);
}

template <typename Data>
void CacheManager<Data>::requestNodes(std::pair<Key, int> param) {
  Node<Data>* node = core.findLocalNode(param.first);
  if (!node) {
    CkPrintf("CacheManager::requestNodes: node not found for key %lu on cm %d\n", param.first, this->thisIndex);
    CkAbort("CacheManager::requestNodes: node not found");
  }
  serviceRequest(node, param.second);
}

template <typename Data>
void CacheManager<Data>::serviceRequest(Node<Data>* node, int cm_index) {
  if (cm_index == this->thisIndex) return; // you'll get it later!
  std::vector<Node<Data>*> sending_nodes;
  std::vector<Particle> sending_particles;
  auto& config = paratreet::getConfiguration();
  core.collectSubtree(config.cache_share_depth, node->depth, sending_nodes, sending_particles, node);
  MultiData<Data> multidata (sending_particles.data(), sending_particles.size(), sending_nodes.data(), sending_nodes.size(), this->thisIndex, node->tp_index);
  this->thisProxy[cm_index].addCache(multidata);
}

template <typename Data>
void CacheManager<Data>::restoreData(std::pair<Key, SpatialNode<Data>> param) {
  std::vector<uint64_t> parked;
  Node<Data>* node = core.installBoundary(CkMyRank(), param, parked);
  process(node, parked);
  walkCreditDec(1); // request credit, as in addCache above
}

// Wake exactly the walkers that parked on the just-installed node: the
// install drained their opaques from the placeholder, each opaque carries
// its lane (paratreet::parkedLane), and one Resumer::process message per
// waiting lane delivers that lane's opaques. This replaces the old
// requested-bitmask filter (which notified whole lanes and made each
// Resumer re-look the key up in its waiting[] side table); the
// exactly-once guarantee now lives in TreeCache's park/install contract.
template <typename Data>
void CacheManager<Data>::process(Node<Data>* node, const std::vector<uint64_t>& parked) {
  if (parked.empty()) return;
  std::map<int, std::vector<uint64_t>> by_lane;
  for (auto o : parked) by_lane[paratreet::parkedLane(o)].push_back(o);
  auto first_pe = this->isNodeGroup() ? CmiNodeFirst(CkMyNode()) : this->thisIndex;
  for (auto& lane : by_lane) {
    walkCreditInc(1); // one credit per resume message, added before the send
    r_proxy[first_pe + lane.first].process(node->key, lane.second);
  }
}

#endif //PARATREET_CACHEMANAGER_H_
