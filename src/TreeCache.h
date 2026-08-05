#ifndef PARATREET_TREECACHE_H_
#define PARATREET_TREECACHE_H_

// TreeCache: the PASSIVE core of the software cache — phase 1 of
// design/smp-cache-extraction.md. One instance is shared by all worker
// threads ("lanes") of a process. It owns the cached global tree (root,
// registries, node pools, placeholder discipline, atomic install) and NO
// messaging: how nodes are REQUESTED — transport, entry methods, waking
// parked walkers — stays with the caller (in paratreet2, the CacheManager
// nodegroup, which delegates here). Callers identify their thread with a
// `lane` index (the CacheManager passes CkMyRank()).
//
// CONCURRENCY DESIGN — READ BEFORE ADDING ANY LOCK HERE.
// Lock-free on the hot path, deliberately:
//   * Concurrent fills grow the tree via ATOMIC child-pointer exchange
//     (Node::exchangeChild) plus the atomic per-lane `requested` bitmask.
//   * Traversals READ node data/particles with NO lock (cached Data is
//     immutable-after-fill; post-build mutation uses PHASE SEPARATION —
//     see refreshSubtreeCopy and design/cache-concurrency.md).
//   * maps_lock is the ONLY mutex and is intentionally NARROW: it guards
//     just the registry-map inserts (local_tps / leaf_lookup), nothing on
//     the data path. Do not widen it and do not add locks — a writer-side
//     mutex cannot serialize against the lockless readers anyway.
//
// Phase-3 goal (not yet): no Charm dependency in this header. For now the
// moved code keeps CkAbort/CkEnforce and the framework Configuration read
// in the collect path was lifted to a parameter (share_depth).

#include "common.h"
#include "Node.h"

#include <list>
#include <map>
#include <memory>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <mutex>

template <typename Data>
struct NodePool {
  virtual ~NodePool() = default;
  virtual Node<Data>* alloc(Key key, typename Node<Data>::Type type, int depth, int n_particles, Particle* particles, Node<Data>* parent, int tp_index, int cm_index) = 0;
  virtual Node<Data>* alloc(Key key, typename Node<Data>::Type type, SpatialNode<Data> spatial_node, Node<Data>* parent, Particle* particles, int tp_index, int cm_index) = 0;
  virtual void cleanup() = 0;
  // Memory accounting: bytes of pool blocks allocated (capacity, not just
  // in-use — blocks are never returned until destruction) and node slots
  // currently in use.
  virtual size_t allocatedBytes() const = 0;
  virtual size_t usedNodes() const = 0;
};

template <typename Data, size_t BranchFactor>
class FullNodePool : public NodePool<Data> {
public:
  FullNodePool(size_t pool_elem_sizei)
  : pool_elem_size(pool_elem_sizei)
  {
    append();
    curr = std::begin(list);
  }
  virtual ~FullNodePool() override {
    for (auto& elem : list) delete[] elem.ptr;
  }
  virtual Node<Data>* alloc(Key key, typename Node<Data>::Type type, int depth, int n_particles, Particle* particles, Node<Data>* parent, int tp_index, int cm_index) override {
    auto buf = getBuf();
    if (type == Node<Data>::Type::Leaf)  { // use this not (n_particles > 0)
      return new (buf) FullNode<Data, BranchFactor>(key, type, depth, n_particles, particles, parent, tp_index, cm_index);
    } else return new (buf) FullNode<Data, BranchFactor>(key, type, depth, parent, tp_index, cm_index);
  }
  virtual Node<Data>* alloc(Key key, typename Node<Data>::Type type, SpatialNode<Data> spatial_node, Node<Data>* parent, Particle* particles, int tp_index, int cm_index) override {
    auto buf = getBuf();
    return new (buf) FullNode<Data, BranchFactor>(key, type, spatial_node, particles, parent, tp_index, cm_index);
  }
private:
  char* getBuf() {
    if (curr->idx == pool_elem_size) {
      if (std::next(curr) == std::end(list)) append();
      curr = std::next(curr);
    }
    auto buf = (FullNode<Data, BranchFactor>*)(curr->ptr) + curr->idx;
    curr->idx++;
    return (char*) buf;
  }
  virtual void cleanup() override {
    for (auto& elem : list) elem.idx = 0;
    curr = std::begin(list);
  }
public:
  virtual size_t allocatedBytes() const override {
    return list.size() * pool_elem_size * sizeof(FullNode<Data, BranchFactor>);
  }
  virtual size_t usedNodes() const override {
    size_t n = 0;
    for (auto& elem : list) n += elem.idx;
    return n;
  }
private:
  void append() {
    list.emplace_back();
    list.back().ptr = new char [sizeof(FullNode<Data, BranchFactor>) * pool_elem_size];
  }
  struct PoolElem {
    char * ptr = nullptr;
    size_t idx = 0;
  };
  const size_t pool_elem_size;
  std::list<PoolElem> list;
  typename std::list<PoolElem>::iterator curr;
};

template <typename Data>
class TreeCache {
public:
  using CachedP = typename CachedParticleOf<Data>::type;
  using NodeLookup = std::unordered_map<Key, Node<Data>*>;

  // ---- park / install (phase 2 of the extraction design) ----
  // A walker that must wait for a node's data PARKS an opaque value on the
  // placeholder; install() (the swapIn drain) returns every parked opaque
  // EXACTLY ONCE to the caller, which owns scheduling the resumptions. The
  // race between a late park and the install is closed here — against
  // install's atomic publication — by closing the list with a sentinel:
  // a park that finds the sentinel returns AlreadyInstalled and the caller
  // proceeds as if the data had been present (the lost-wakeup fix,
  // provided once here instead of re-solved by every client).
  struct ParkedEntry {
    uint64_t opaque;
    ParkedEntry* next;
  };
  static void* closedSentinel() {
    static char sentinel_storage;
    return (void*)&sentinel_storage;
  }
  enum class ParkResult { Parked, AlreadyInstalled };

  ParkResult park(Node<Data>* slot, uint64_t opaque) {
    // Consecutive-duplicate suppression, matching the old Resumer
    // waiting-list behavior: one walker sweeping many payloads against the
    // same placeholder parks once (a racing other-lane entry in between
    // just costs a duplicate resumption, which the traverser tolerates).
    void* head = slot->parked_head.load();
    if (head != closedSentinel() && head != nullptr &&
        ((ParkedEntry*)head)->opaque == opaque)
      return ParkResult::Parked;
    auto* entry = new ParkedEntry{opaque, nullptr};
    while (true) {
      head = slot->parked_head.load();
      if (head == closedSentinel()) {
        delete entry;
        return ParkResult::AlreadyInstalled;
      }
      entry->next = (ParkedEntry*)head;
      if (slot->parked_head.compare_exchange_weak(head, (void*)entry))
        return ParkResult::Parked;
    }
  }

private:
  // Close a displaced placeholder's parked list and collect the opaques.
  // Exactly-once: the exchange with the sentinel wins against concurrent
  // parks (they either landed before — collected here — or see the
  // sentinel and self-handle).
  static void drainParked(Node<Data>* displaced, std::vector<uint64_t>& out) {
    void* head = displaced->parked_head.exchange(closedSentinel());
    if (head == closedSentinel()) return; // already drained
    auto* entry = (ParkedEntry*)head;
    while (entry) {
      out.push_back(entry->opaque);
      auto* next = entry->next;
      delete entry;
      entry = next;
    }
  }

public:

  // Narrow structural lock for the registry maps only (local_tps /
  // leaf_lookup); NOT a data-path lock. See the header comment.
  std::mutex maps_lock;
  Node<Data>* root = nullptr;
  size_t branch_factor = 0;
  NodeLookup local_tps;
  NodeLookup leaf_lookup; // all the cached leaves from copied subtrees
  std::map<Key, std::vector<int>> subtree_copy_started;
  std::set<Key> prefetch_set;
  std::vector<std::vector<Node<Data>*>> cached_leaves; // cached leaves left over
  std::vector<std::vector<Node<Data>*>> displaced_leaves; // leaves split between >1 Partitions
  std::vector<std::unique_ptr<NodePool<Data>>> pools;

  // Walk-completion credit counter (Kale's scheme, 2026-08-05): a
  // process-wide count of outstanding walk continuations. Every message
  // that carries walk continuation holds a credit -- added strictly
  // BEFORE the message that will consume it is sent, removed only AFTER
  // the synchronous work it triggered has completed -- so the count can
  // never read zero while work is pending. Zero (once armed and the
  // seeding open credit is released) means this process's walk is done;
  // a reduction over processes then replaces quiescence detection.
  // walk_state: 0 idle, 1 armed, 2 fired. Consumed by the serial-mode
  // FoF walk (runFoFPhase3); the distributed path keeps the combined
  // walk+union quiescence instead.
  std::atomic<long> walk_credits{0};
  std::atomic<int> walk_state{0};
  Data nodewide_data;

  TreeCache() = default;
  ~TreeCache() { destroy(); }

  // shared = more than one lane uses this instance (the nodegroup case);
  // when false the maps lock is skipped, matching the old per-PE-group
  // build.
  void init(size_t n_lanes, bool shared_, size_t branch_factor_,
            size_t pool_elem_size) {
    shared = shared_;
    cached_leaves.resize(n_lanes);
    displaced_leaves.resize(n_lanes);
    branch_factor = branch_factor_;
    for (size_t i = 0; i < n_lanes; i++) {
      if (branch_factor == 2) pools.emplace_back(new FullNodePool<Data, 2>(pool_elem_size));
      else if (branch_factor == 8) pools.emplace_back(new FullNodePool<Data, 8>(pool_elem_size));
      else CkAbort("Config branch factor is not 2 or 8. Update list in TreeCache::init to handle this.");
    }
  }

  void lockMaps() {
    if (shared) maps_lock.lock();
  }
  void unlockMaps() {
    if (shared) maps_lock.unlock();
  }

  // Memory accounting numbers (the caller wraps them in its reduction).
  // Call at a phase boundary (quiescence-separated from fills) — the same
  // phase-separation contract as every post-build cache read.
  struct Stats {
    long pool_bytes = 0;
    long used_nodes = 0;        // pool slots incl. placeholders
    long cached_nodes = 0;      // fetched-content count
    long cached_leaves = 0;
    long cached_particles = 0;
    long total_bytes = 0;
  };
  Stats stats() {
    Stats s;
    for (auto& p : pools) {
      s.pool_bytes += (long)p->allocatedBytes();
      s.used_nodes += (long)p->usedNodes();
    }
    // Tally by traversing the cache tree itself (leaf_lookup is not
    // populated on every fetch path). Post-quiescence single reader — safe.
    tallyCache(root, s.cached_nodes, s.cached_leaves, s.cached_particles);
    // Cached-particle bytes use the ACTUAL stored type (CachedParticle —
    // see design/cached-particle-slimming.md), so this directly shows the
    // slimming saving.
    s.total_bytes = s.pool_bytes + s.cached_particles * (long)sizeof(CachedP);
    return s;
  }

  // Recursive tally over the cache tree: fetched (Cached*) nodes, cached
  // remote leaves, and their copied particles. Descends everywhere — local
  // subtrees can hang below cached boundary nodes and vice versa.
  void tallyCache(Node<Data>* n, long& cached_nodes, long& cached_leaves_n,
                  long& cached_particles) {
    if (n == nullptr) return;
    switch (n->type) {
      case Node<Data>::Type::CachedRemoteLeaf:
        cached_nodes++;
        cached_leaves_n++;
        if (n->n_particles > 0) cached_particles += n->n_particles;
        return;
      case Node<Data>::Type::CachedRemote:
      case Node<Data>::Type::CachedBoundary:
        cached_nodes++;
        break;
      default:
        break;
    }
    for (int i = 0; i < n->n_children; i++)
      tallyCache(n->getChild(i), cached_nodes, cached_leaves_n,
                 cached_particles);
  }

  void addDisplacedLeaf(int lane, Node<Data>* leaf) {
    displaced_leaves[lane].push_back(leaf);
  }

  // Bulk teardown between phases (the "auxiliaries" of the design note).
  void destroy() {
    for (auto& clv : cached_leaves) {
      for (auto l : clv) l->freeCachedParticles();
      clv.clear();
    }
    for (auto& l : leaf_lookup) l.second->freeCachedParticles();
    for (auto& dlv : displaced_leaves) {
      dlv.clear();
    }
    for (auto& pool : pools) pool->cleanup();
    local_tps.clear();
    leaf_lookup.clear();
    walk_credits.store(0);
    walk_state.store(0);
    subtree_copy_started.clear();
    prefetch_set.clear();

    root = nullptr;
  }

  // Only PLACEHOLDERS (Remote / RemoteAboveTPKey) carry an open parked
  // list; every other node is born with the list CLOSED, so a park on an
  // already-installed (or local) node returns AlreadyInstalled instead of
  // accepting a waiter nobody would ever drain. Found by the standalone
  // unit test (tests/treecache); the framework's own walkers only park on
  // placeholder types, but the library contract should not rely on that.
  static bool isPlaceholderType(typename Node<Data>::Type type) {
    return type == Node<Data>::Type::Remote ||
           type == Node<Data>::Type::RemoteAboveTPKey;
  }

  Node<Data>* makeNode(int lane, Key key, typename Node<Data>::Type type, int depth, int n_particles, Particle* particles, Node<Data>* parent, int tp_index, int cm_index) {
    auto* node = pools[lane]->alloc(key, type, depth, n_particles, particles, parent, tp_index, cm_index);
    if (!isPlaceholderType(type)) node->parked_head.store(closedSentinel());
    return node;
  }

  Node<Data>* makeCachedNode(int lane, Key key, typename Node<Data>::Type type, SpatialNode<Data> spatial_node, Node<Data>* parent, const CachedP* particlesToCopy, int tp_index, int cm_index) {
    // Cached copies are stored in the application's CachedParticle type
    // (design/cached-particle-slimming.md); the node's legacy Particle
    // pointer aliases the same array when CachedParticle == Particle.
    CachedP* cached = nullptr;
    if (spatial_node.n_particles > 0) {
      cached = new CachedP [spatial_node.n_particles];
      std::copy(particlesToCopy, particlesToCopy + spatial_node.n_particles, cached);
    }
    auto* node = pools[lane]->alloc(key, type, spatial_node, parent, nullptr, tp_index, cm_index);
    if (cached) node->setCachedParticles(cached);
    if (!isPlaceholderType(type)) node->parked_head.store(closedSentinel());
    return node;
  }

  // Register a Subtree's local root (and optionally its leaves) with the
  // registries; the prefetch bookkeeping rides along under the same lock.
  void connectRoot(Node<Data>* node) {
    lockMaps();
    local_tps.insert(std::make_pair(node->key, node));
    prepPrefetch(node);
    unlockMaps();
  }
  void connectRootWithLeaves(Node<Data>* node, const std::vector<Node<Data>*>& leaves) {
    lockMaps();
    local_tps.insert(std::make_pair(node->key, node));
    for (auto && leaf : leaves) leaf_lookup.emplace(leaf->key, leaf);
    unlockMaps();
  }

  void prepPrefetch(Node<Data>* node) {
    // THIS IS IN LOCK. make faster? TODO
    nodewide_data += node->data;
    Key curr_key = node->key;
    while (curr_key > 1) {
      curr_key /= branch_factor;
      prefetch_set.insert(curr_key);
      for (int i = 0; i < branch_factor; i++) {
        prefetch_set.insert(curr_key * branch_factor + i);
      }
    }
  }

  // Find the live local node for a request key: walk up to the owning
  // subtree root, then down to the descendant. Returns null if no local
  // subtree covers the key (caller aborts — a request routed here is
  // supposed to be answerable).
  Node<Data>* findLocalNode(Key key) {
    Key temp = key;
    while (!local_tps.count(temp)) temp /= branch_factor;
    return local_tps[temp]->getDescendant(key);
  }

  // Collect the reply for a node request: the node, its descendants to
  // share_depth levels, and leaf particle payloads (the "partial subtree"
  // of the design note).
  void collectSubtree(int share_depth, int start_depth,
                      std::vector<Node<Data>*>& sending_nodes,
                      std::vector<Particle>& sending_particles,
                      Node<Data>* to_process) {
    sending_nodes.push_back(to_process);
    if (to_process->type == Node<Data>::Type::Leaf) {
      std::copy(to_process->particles(), to_process->particles() + to_process->n_particles, std::back_inserter(sending_particles));
    }
    if (to_process->depth + 1 < start_depth + share_depth) {
      for (int i = 0; i < to_process->n_children; i++) {
        Node<Data>* child = to_process->getChild(i);
        collectSubtree(share_depth, start_depth, sending_nodes, sending_particles, child);
      }
    }
  }

  // Install a fetched partial subtree (the reply to a node request, or a
  // shipped subtree copy). Builds Cached* nodes off the pool, wires
  // children/placeholders, and atomically publishes the top node in place
  // of its placeholder (swapIn) — concurrent lookups see the placeholder
  // or the complete subtree, never a partial state. Returns the installed
  // top node and appends the displaced placeholder's parked waiters to
  // `parked`; the CALLER owns waking them.
  Node<Data>* installSubtree(int lane, const CachedP* particles, int n_particles,
                             std::pair<Key, SpatialNode<Data>>* nodes, int n_nodes,
                             int cm_index, int tp_index, bool add_to_tps,
                             std::vector<uint64_t>& parked) {
    Node<Data>* first_node_placeholder_parent = nullptr;
    if (!add_to_tps) {
      auto first_node_placeholder = root->getDescendant(nodes[0].first);
      if (first_node_placeholder->type == Node<Data>::Type::CachedRemote
        || first_node_placeholder->type == Node<Data>::Type::CachedRemoteLeaf)
      {
        CkAbort("Invalid node placeholder type in TreeCache::installSubtree");
      }
      first_node_placeholder_parent = first_node_placeholder->parent;
    }

    bool start_is_leaf = nodes[0].second.n_particles > -1;
    auto top_type = start_is_leaf ? Node<Data>::Type::CachedRemoteLeaf : Node<Data>::Type::CachedRemote;
    auto first_node = makeCachedNode(lane, nodes[0].first, top_type, nodes[0].second, first_node_placeholder_parent, particles, tp_index, cm_index);
    std::vector<Node<Data>*> leaves;
    if (start_is_leaf) leaves.push_back(first_node);
    insertNode(lane, first_node, false, false);
    int p_index = start_is_leaf ? nodes[0].second.n_particles : 0;
    for (int j = 1; j < n_nodes; j++) {
      auto && new_key = nodes[j].first;
      auto && spatial_node = nodes[j].second;
      auto curr_parent = first_node->getDescendant(new_key / branch_factor);
      auto type = spatial_node.n_particles > -1 ? Node<Data>::Type::CachedRemoteLeaf : Node<Data>::Type::CachedRemote;
      auto node = makeCachedNode(lane, new_key, type, spatial_node, curr_parent, &particles[p_index], tp_index, cm_index);
      if (node->isLeaf()) {
        p_index += spatial_node.n_particles;
        leaves.push_back(node);
      }
      insertNode(lane, node, false, true);
    }
    if (add_to_tps) connectRootWithLeaves(first_node, leaves);
    else {
      auto && clv = cached_leaves[lane];
      clv.insert(clv.end(), leaves.begin(), leaves.end());
      swapIn(first_node, parked);
    }
    return first_node;
  }

  // Install one boundary/canopy node (starter pack and canopy restore).
  // Returns the installed node and appends the displaced placeholder's
  // parked waiters to `parked`; caller owns any wake-up.
  Node<Data>* installBoundary(int lane, std::pair<Key, SpatialNode<Data>>& param,
                              std::vector<uint64_t>& parked) {
    Key key = param.first;
    Node<Data>* parent = (key == Key(1)) ? nullptr : root->getDescendant(key / branch_factor);
    auto node = makeCachedNode(lane, key, Node<Data>::Type::CachedBoundary, param.second, parent, nullptr, -1, -1);
    insertNode(lane, node, true, false);
    CkAssert(node->type == Node<Data>::Type::CachedBoundary);
    swapIn(node, parked);
    return node;
  }

  // Update a previously copied subtree in place from a refreshed
  // flat_subtree snapshot: overwrite each cached node's Data annotation and
  // each cached leaf's particle copies, matching by key. Structure is
  // unchanged, so cached pointers held elsewhere stay valid. No-op when no
  // copy of the subtree lives here.
  //
  // CONCURRENCY CONTRACT: traversals read node->data/particles WITHOUT a
  // lock; this MUTATES that payload, so it is only safe in a mutation phase
  // quiescence-separated from any traversal reading the affected subtree
  // (the Subtree::upwardPass contract; see design/cache-concurrency.md).
  // maps_lock below guards only the local_tps lookup. Concurrent refreshes
  // touch disjoint subtrees, so no node is written twice.
  void refreshSubtreeCopy(const CachedP* particles,
                          std::pair<Key, SpatialNode<Data>>* nodes, int n_nodes) {
    if (n_nodes == 0) return;
    lockMaps();
    auto it = local_tps.find(nodes[0].first);
    Node<Data>* top = (it != local_tps.end()) ? it->second : nullptr;
    unlockMaps();
    if (!top) return; // no copy of this subtree lives here
    int p_index = 0;
    for (int j = 0; j < n_nodes; j++) {
      auto&& key = nodes[j].first;
      auto&& spatial_node = nodes[j].second;
      Node<Data>* node = top->getDescendant(key);
      if (node) {
        node->data = spatial_node.data; // refreshed annotation
        // Leaves carry n_particles > -1; refresh their particle copies in
        // place. Only valid when cached storage is full Particle (the
        // overload dispatch aborts for slim-CachedParticle apps).
        for (int i = 0; i < spatial_node.n_particles; i++) {
          refreshCachedParticle(node, i, particles[p_index + i]);
        }
      }
      if (spatial_node.n_particles > -1) p_index += spatial_node.n_particles;
    }
  }

  // refreshSubtreeCopy particle-refresh dispatch: real update when the
  // cached type is the full Particle; abort otherwise (C++11 overload
  // selection — exact match wins when CachedP == Particle).
  static void refreshCachedParticle(Node<Data>* node, int i, const Particle& p) {
    node->changeParticle(i, p);
  }
  template <typename T>
  static void refreshCachedParticle(Node<Data>*, int, const T&) {
    CkAbort("refreshSubtreeCopy's particle refresh requires full cached "
            "particles; this application declares a slim Data::CachedParticle");
  }

  // Reset cached leaves back to placeholders and enumerate, per partition
  // index, the particle keys whose fresh copies must be re-requested. The
  // CALLER performs the requests (messaging stays outside).
  // Requires FULL particles on cached leaves (reads key/partition_idx):
  // invalid for applications that opt into a slim CachedParticle.
  void resetCachedParticles(int lane, std::map<int, std::vector<Key>>& partitions_to_request) {
    if (!std::is_same<CachedP, Particle>::value)
      CkAbort("resetCachedParticles requires full cached particles; this "
              "application declares a slim Data::CachedParticle");
    for (auto && clv : cached_leaves) {
      for (auto && cl : clv) {
        SpatialNode<Data> empty_sn (cl->depth, 0);
        auto new_leaf = makeCachedNode(lane, cl->key, Node<Data>::Type::Remote, empty_sn, cl->parent, nullptr, cl->tp_index, cl->cm_index); // placeholder
        auto which_child = cl->key % branch_factor;
        cl->parent->exchangeChild(which_child, new_leaf);
        cl->freeCachedParticles();
      }
      clv.clear();
    }
    auto handleLeaf = [&] (Node<Data>* leaf) {
      for (int i = 0; i < leaf->n_particles; i++) {
        partitions_to_request[leaf->particles()[i].partition_idx].push_back(leaf->particles()[i].key);
      }
    };
    for (auto && dlv : displaced_leaves) {
      for (auto && dl : dlv) handleLeaf(dl);
    }
    for (auto& l : leaf_lookup) handleLeaf(l.second);
  }

  // Replace stale particle copies (matched by particle key) on displaced
  // and copied leaves. Returns how many were replaced; the caller checks
  // it against what it requested.
  size_t receiveParticleUpdates(const std::vector<Particle>& particles_received) {
    std::map<Key, const Particle*> key_mappings;
    for (auto& p : particles_received) key_mappings.emplace(p.key, &p);
    size_t replaced = 0;
    auto handleLeaf = [&] (Node<Data>* leaf) {
      for (int i = 0; i < leaf->n_particles; i++) {
        auto it = key_mappings.find(leaf->particles()[i].key);
        if (it != key_mappings.end()) {
          replaced++;
          leaf->changeParticle(i, *(it->second));
        }
      }
    };
    for (auto && dlv : displaced_leaves) {
      for (auto && dl : dlv) handleLeaf(dl);
    }
    for (auto& l : leaf_lookup) handleLeaf(l.second);
    return replaced;
  }

  // Atomically publish an installed node in place of its placeholder, and
  // collect the placeholder's parked waiters into `parked` (each opaque
  // handed back exactly once — the caller owns waking them). Waiters that
  // parked before the exchange are drained here; one that parks after sees
  // the closed list and gets AlreadyInstalled from park(), so no wakeup is
  // lost (the same guarantee the old requested-bitmask handoff provided;
  // see the fanout-fix record, design/walk-uf2-overlap.md step 2).
  void swapIn(Node<Data>* to_swap, std::vector<uint64_t>& parked) {
    if (to_swap->key > 1) {
      auto which_child = to_swap->key % branch_factor;
      Node<Data>* displaced = to_swap->parent->exchangeChild(which_child, to_swap);
      if (displaced) drainParked(displaced, parked);
    }
    else {
      std::swap(root, to_swap);
      // to_swap now holds the displaced old root; NULL on the very first
      // swap (the starter pack installing the initial root).
      if (to_swap) drainParked(to_swap, parked);
    }
  }

  // Wire an installed node's children: adopt registered local subtree
  // roots (above the subtree keys) and create placeholders elsewhere.
  void insertNode(int lane, Node<Data>* node, bool above_tp, bool should_swap) {
    for (int i = 0; i < node->n_children; i++) {
      Node<Data>* new_child = nullptr;
      Key child_key = node->key * branch_factor + i;
      bool add_placeholder = false;
      if (above_tp) {
        auto it = local_tps.find(child_key);
        if (it != local_tps.end()) {
          new_child = it->second;
          new_child->parent = node;
        }
        else {
          add_placeholder = true;
        }
      }
      if (!above_tp || add_placeholder) {
        auto type = (above_tp) ? Node<Data>::Type::RemoteAboveTPKey : Node<Data>::Type::Remote;
        SpatialNode<Data> empty_sn (node->depth + 1, -1);
        int new_cm_index = above_tp ? -1 : node->cm_index;
        new_child = makeCachedNode(lane, child_key, type, empty_sn, node, nullptr, -1, new_cm_index); // placeholder
      }
      node->exchangeChild(i, new_child);
    }
    if (should_swap) {
      // Publication within a not-yet-published subtree (installSubtree's
      // interior nodes): the displaced slot is a fresh placeholder no
      // walker has ever seen, so nothing can be parked on it.
      std::vector<uint64_t> none;
      swapIn(node, none);
    }
  }

private:
  bool shared = false;
};

#endif // PARATREET_TREECACHE_H_
