#ifndef _SUBTREE_H_
#define _SUBTREE_H_

#include "paratreet.decl.h"
#include "common.h"
#include "templates.h"
#include "ParticleMsg.h"
#include "NodeWrapper.h"
#include "Node.h"
#include "Utility.h"
#include "Reader.h"
#include "CacheManager.h"
#include "Resumer.h"
#include "Driver.h"
#include "OrientedBox.h"

#include <cstring>
#include <queue>
#include <vector>
#include <fstream>

extern CProxy_TreeSpec treespec;
extern CProxy_Reader readers;

template <typename Data>
class TreePiece : public CBase_TreePiece<Data> {
public:
  std::vector<Particle> particles, incoming_particles;
  std::vector<Node<Data>*> leaves;
  std::vector<Node<Data>*> empty_leaves;
  Real load;

  long n_total_particles; // global count: 64-bit
  int n_treepieces;
  int n_partitions;

  bool matching_decomps;

  Key tp_key; // Should be a prefix of all particle keys underneath this node
  Node<Data>* local_root; // Root node of this TreePiece, TreeCanopies sit above this node
  MultiData<Data> flat_subtree;
  std::vector<int> copy_requesters; // CacheManager indices that took a flat_TreePiece copy

  CProxy_TreeCanopy<Data> tc_proxy;
  CProxy_CacheManager<Data> cm_proxy;
  CProxy_Resumer<Data> r_proxy;
  Resumer<Data>* r_local = nullptr;
  CacheManager<Data>* cm_local = nullptr;

  std::unique_ptr<Traverser<Data>> traverser;

  std::vector<Particle> flushed_particles; // For debugging

  TreePiece(const CkCallback&, long, int, int, TCHolder<Data>,
          CProxy_Resumer<Data>, CProxy_CacheManager<Data>, DPHolder<Data>, bool);
  TreePiece(CkMigrateMessage * msg){
    delete msg;
  };
  void receive(ParticleMsg*);
  void buildTree(CProxy_Partition<Data>, CkCallback);
  void recursiveBuild(Node<Data>*, Particle*, size_t, size_t);
  void populateTree();
  inline void initCache();
  typename Node<Data>::Type getType(size_t num_particles, size_t max_particles_per_leaf) const;
  void handlePossibleLeaf(Node<Data>* node);
  void sendLeaves(CProxy_Partition<Data>);
  template <typename Visitor> void startDual(Visitor v);
  template <typename Visitor> void startDown(Visitor v);
  void resumeAfterPause(size_t travIdx);
  void goDown(size_t travIdx);
  void requestNodes(Key, int);
  void requestCopy(int, PPHolder<Data>);
  void refreshCopies(const CkCallback&);
  void print(Node<Data>*);
  void destroy();
  void reset();
  void output(CProxy_Writer w, CkCallback cb);
  void pup(PUP::er& p);
  void collectMetaData(const CkCallback & cb);
  void callPerLeafFn(paratreet::PerLeafAble<Data>&, const CkCallback&);
  void callPerTreePieceFn(paratreet::PerTreePieceAble<Data>&, const CkCallback&);
  void upwardPass(const CkCallback&);
  // Single-distribution movement (design/single-distribution-mode.md
  // phase B): the Partition-resident kick/perturb/rebuild machinery on
  // the TreePiece's OWN particle storage, used when no Partition array
  // exists. (Particle deletion — Partition::deleteParticleOfOrder — has
  // no TreePiece-side equivalent yet; the collision-style apps that use it
  // stay dual-distribution.)
  void kick(Real, const CkCallback&);
  void perturb(Real, const CkCallback&);
  void rebucket(BoundingBox, bool);
  void recomputeData(Node<Data>*);
  void addNodeToFlatSubtree(Node<Data>* node);
  void pauseForLB(){
    //CkPrintf("[ST %d]  pause for LB on PE %d\n", this->thisIndex, CkMyPe());
    this->AtSync();
  }
  // ---- Targeted shedding, decide/reduce/migrate (design/piece-load-model.md).
  // The migration must NOT happen inside this broadcast: the source PE is
  // still walking its local element list for this very broadcast, and
  // destroying an element mid-walk segfaults in
  // CkArrayBroadcaster::attemptDelivery (measured, reproducible at
  // 2 processes x 1 PE, so not a race). Instead every element DECIDES here
  // and contributes; the reduction completing proves the broadcast reached
  // everyone and the walk is over, and only then does the Driver migrate
  // the chosen elements point-to-point.
  int shed_dest_ = -1;
  void shedDecide(const CkCallback& cb) {
    shed_dest_ = -1;
    const char* vn = std::getenv("FOF_SHED_NODE");
    const char* kc = std::getenv("FOF_SHED_COUNT");
    const int victim = vn ? std::atoi(vn) : -1;
    const int k = kc ? std::atoi(kc) : 0;
    if (victim >= 0 && k > 0 && CkMyNode() == victim &&
        !incoming_particles.empty() && CkNumNodes() > 1) {
      static std::atomic<int> taken{0};
      const int slot = taken.fetch_add(1);
      if (slot < k) {
        int dn = (victim + 1) % CkNumNodes();
        if (dn != victim)
          shed_dest_ = CkNodeFirst(dn) + (slot % CkNodeSize(dn));
      }
    }
    int pair[2] = {this->thisIndex, shed_dest_};
    this->contribute(sizeof(pair), pair, CkReduction::concat, cb);
  }
  void migrateTo(int destPe) { this->ckMigrate(destPe); }

  void ResumeFromSync(){
    //CkPrintf("[ST %d]  resume from sync for LB on PE %d\n", this->thisIndex, CkMyPe());
    return;
  };
  // Predicted FoF phase-1 cost of this piece, from particle count and
  // occupied volume alone (design/piece-load-model.md):
  //
  //   self / phaseA  ~ n^SELF_P     SELF_P ~ 1.2 measured, R^2 0.85-0.90
  //                                 (design/cost-model-probe.md). The
  //                                 exponent is sub-quadratic because the
  //                                 -G cell grid short-circuits dense self
  //                                 pairs into ~n log n.
  //   pair / phaseB  ~ n^2/V^(4/3)  the expected-pairs estimate m2 (R^2 0.87
  //                                 at 2B) summed over a piece's neighbours
  //                                 in a MEAN-FIELD approximation: neighbours
  //                                 of similar density contribute
  //                                 rho^2 * A * b * V_ball, and A ~ V^(2/3).
  //                                 The b^4 that falls out is the same for
  //                                 every piece, so it is absorbed into K.
  //
  // Only the RATIO of the two terms is a free parameter, hence one knob
  // (PARATREET_LB_K). Both inputs are available BEFORE the tree is built,
  // which matters: pup() ships incoming_particles and nothing else, so the
  // window between flush/rebucket and buildTree is the only one in which a
  // TreePiece may legally migrate.
  static double lbK() {
    static const double v = [] {
      const char* e = std::getenv("PARATREET_LB_K");
      return e ? std::atof(e) : 0.0;   // 0 = self term only (calibrate first)
    }();
    return v;
  }
  static double lbSelfExp() {
    static const double v = [] {
      const char* e = std::getenv("PARATREET_LB_SELF_EXP");
      return e ? std::atof(e) : 1.2;
    }();
    return v;
  }
  // n and the occupied volume, taken from whichever representation is live:
  // incoming_particles pre-build (the migration window), the built tree
  // after. Returns false when neither is available.
  bool lbExtent(double& n_out, double& vol_out) const {
    if (!incoming_particles.empty()) {
      OrientedBox<Real> box;
      for (const auto& p : incoming_particles) box.grow(p.position);
      n_out = (double)incoming_particles.size();
      vol_out = (double)box.volume();
      return true;
    }
    if (local_root) {
      n_out = (double)local_root->data.n_below;
      vol_out = (double)local_root->data.box.volume();
      return n_out > 0;
    }
    return false;
  }
  void UserSetLBLoad()
  {
    // PARATREET_LB_MODEL: 1 (default) = the cost model below; 0 = the
    // historical inverse-bounding-box-volume proxy; 2 = uniform load.
    //
    // Read 0 carefully as an A/B arm: that proxy reads local_root, which is
    // NULL in the pre-build migration window, so at iteration 0 it assigns
    // no load at all — arm "LB with an unset load vector", not "LB with the
    // old model". The old proxy simply has no inputs before the tree
    // exists. Mode 2 is the control that question actually wants: uniform
    // load makes the balancer equalise PIECE COUNT, so a win for mode 1
    // over mode 2 is attributable to the cost model rather than to the fact
    // that any LB ran at all.
    static const int model = [] {
      const char* e = std::getenv("PARATREET_LB_MODEL");
      return e ? std::atoi(e) : 1;
    }();
    double n = 0, vol = 0;
    if (model == 2) {
      this->load = 1.0;
    } else if (model == 0) {
      if (local_root) {
        Real volume = local_root->data.box.volume();
        this->load = volume > 0.0 ? 1.0 / volume : 0.0;
      }
    } else if (lbExtent(n, vol)) {
      double w = std::pow(n, lbSelfExp());
      if (lbK() > 0 && vol > 0) w += lbK() * n * n / std::pow(vol, 4.0 / 3.0);
      this->load = w;
    } else {
      this->load = 0.0;
    }
    //CkPrintf("[ST %d] LB load set to %f on PE %d\n", this->thisIndex, this->load, CkMyPe());
    this->setObjTime(this->load);
  }

  // For debugging
  void checkParticlesChanged(const CkCallback& cb) {
    bool result = true;
    if (particles.size() != flushed_particles.size()) {
      result = false;
      this->contribute(sizeof(bool), &result, CkReduction::logical_and_bool, cb);
      return;
    }
    for (int i = 0; i < particles.size(); i++) {
      if (!(particles[i] == flushed_particles[i])) {
        result = false;
        break;
      }
    }
    this->contribute(sizeof(bool), &result, CkReduction::logical_and_bool, cb);
  }
};

template <typename Data>
TreePiece<Data>::TreePiece(const CkCallback& cb, long n_total_particles_,
                       int n_treepieces_, int n_partitions_, TCHolder<Data> tc_holder,
                       CProxy_Resumer<Data> r_proxy_,
                       CProxy_CacheManager<Data> cm_proxy_, DPHolder<Data> dp_holder,
                       bool matching_decomps_){
  n_total_particles = n_total_particles_;
  n_treepieces = n_treepieces_;
  n_partitions = n_partitions_;

  tc_proxy = tc_holder.proxy;
  cm_proxy = cm_proxy_;
  cm_local = cm_proxy.ckLocalBranch();
  r_proxy  = r_proxy_;

  //this->load = 0.0;
  //this->usesAutoMeasure = false;
  // Load-balancing participation: in single-distribution mode the
  // TreePieces ARE the balanced array (Driver's LB block calls
  // treepieces.pauseForLB). In dual mode they must NOT register for
  // AtSync — the Partitions drive it and never-synching registered
  // TreePieces would stall the balancer's barrier.
  this->usesAtSync = paratreet::getConfiguration().single_distribution;

  matching_decomps = matching_decomps_;

  tp_key = treespec.ckLocalBranch()->getTreePieceDecomposition()->
    getTpKey(this->thisIndex);

  // Create TreeCanopies and send proxies
  auto sendProxy =
    [&](Key dest, int tp_index) {
      tc_proxy[dest].recvProxies(TPHolder<Data>(this->thisProxy),
                                 tp_index, cm_proxy, dp_holder);
    };

  treespec.ckLocalBranch()->getTree()->buildCanopy(this->thisIndex, sendProxy);

  local_root = nullptr;

  this->contribute(cb);
}

template <typename Data>
void TreePiece<Data>::pup(PUP::er& p) {
  p | n_total_particles;
  p | n_treepieces;
  p | n_partitions;
  p | tp_key;
  p | tc_proxy;
  p | cm_proxy;
  p | r_proxy;
  p | incoming_particles;
  p | matching_decomps;
  p | load;
  if(p.isUnpacking()) {
    cm_local = cm_proxy.ckLocalBranch();
  }
}

template <typename Data>
void TreePiece<Data>::receive(ParticleMsg* msg) {
  // Copy particles to local vector
  // TODO: Remove memcpy by just storing the pointer to msg->particles
  // and using it in tree build
  int initial_size = incoming_particles.size();
  incoming_particles.resize(initial_size + msg->n_particles);
  std::memcpy(&incoming_particles[initial_size], msg->particles,
              msg->n_particles * sizeof(Particle));
  // Release the sender's flush window. Sent only when the producer asked for it
  // (Reader::flush with windowing on); -1 means no ack expected, which is every
  // other ParticleMsg producer. Acked after the copy, so the window reflects
  // particles actually landed rather than merely delivered.
  if (msg->sender >= 0)
    readers[msg->sender].flushAck(msg->n_particles, this->thisProxy);
  delete msg;
}

template <typename Data>
void TreePiece<Data>::collectMetaData (const CkCallback & cb) {
  Real maxVelocity = 0.0;
  for (auto& particle : particles){
    if (particle.velocity.lengthSquared() > maxVelocity)
      maxVelocity = particle.velocity.lengthSquared();
  }
  maxVelocity = std::sqrt(maxVelocity);

  const size_t numTuples = 1;
  CkReduction::tupleElement tupleRedn[] = {
    CkReduction::tupleElement(sizeof(Real), &maxVelocity, CkReduction::max_float)
  };

  CkReductionMsg * msg = CkReductionMsg::buildFromTuple(tupleRedn, numTuples);
  msg->setCallback(cb);
  this->contribute(msg);
};

template <typename Data>
void TreePiece<Data>::sendLeaves(CProxy_Partition<Data> part)
{
  // When TreePiece and Partition have the same decomp type
  // there is a consistant 1-on-1 mapping
  // partical.partition_idx is ignored
  if (matching_decomps) {
    auto it = cm_proxy.ckLocalBranch()->partition_lookup.find(this->thisIndex);
    it->second->addLeaves(leaves, this->thisIndex);
    return;
  }

  // When TreePiece and Partition has different decomp types
  std::map<int, std::set<Node<Data>*>> part_idx_to_leaf;
  std::set<Node<Data>*> displaced_leaves, shared_leaves;
  for (auto && leaf : leaves) {
    int prev_partition_idx = -1;
    for (int pi = 0; pi < leaf->n_particles; pi++) {
      auto partition_idx = leaf->particles()[pi].partition_idx;
      if (pi > 0 && partition_idx != prev_partition_idx) displaced_leaves.insert(leaf);
      prev_partition_idx = partition_idx;
      part_idx_to_leaf[partition_idx].insert(leaf);
    }
  }

  size_t num_shares = 0u, num_copies = 0;
  for (auto && part_receiver : part_idx_to_leaf) {
    auto it = cm_proxy.ckLocalBranch()->partition_lookup.find(part_receiver.first);
    if (it != cm_proxy.ckLocalBranch()->partition_lookup.end()) {
      std::vector<Node<Data>*> leaf_ptrs (part_receiver.second.begin(), part_receiver.second.end());
      it->second->addLeaves(leaf_ptrs, this->thisIndex);
    }
    else {
      std::vector<Key> lookup_leaf_keys;
      for (auto && leaf : part_receiver.second) {
        lookup_leaf_keys.push_back(leaf->key);
        displaced_leaves.insert(leaf);
        if (shared_leaves.insert(leaf).second) num_shares += leaf->n_particles;
      }
      part[part_receiver.first].receiveLeaves(lookup_leaf_keys, tp_key, this->thisIndex, this->thisProxy);
    }
  }
  for (auto leaf : displaced_leaves) {
    cm_local->addDisplacedLeaf(leaf);
    num_copies += leaf->n_particles;
  }
  thread_state_holder.ckLocalBranch()->countCopiesAndShares(num_copies, num_shares);
}

// TreePiece-driven TRANSPOSED walk (single-distribution mode): the same
// TransposedDownTraverser the Partitions run in dual-distribution mode,
// with this TreePiece's own leaves as the target buckets — the walk
// semantics (and the acceptance decisions an app like gravity makes) are
// identical, only the driving chare differs.
template <typename Data>
template <typename Visitor>
void TreePiece<Data>::startDown(Visitor v) {
  r_local = r_proxy.ckLocalBranch();
  r_local->treepiece_proxy = this->thisProxy;
  r_local->use_treepiece = true;
  // Same resumer/cache cross-wiring as startDual below (no Partition
  // elements exist to have done it).
  auto* cml = cm_proxy.ckLocalBranch();
  r_local->cm_local = cml;
  cml->r_proxy = r_proxy;
  traverser.reset(new TransposedDownTraverser<Data, Visitor, TreePiece<Data>>(
      v, 0, leaves, *this));
  traverser->start();
  // Pause protocol, mirrored from Partition::startNewTraverser: the
  // transposed traverser defers work (paused_curr_nodes) once its
  // outstanding-request/handle counters trip, and relies on its OWNER to
  // reschedule through the self-perpetuating resumeAfterPause entry.
  // Without this the paused work is silently abandoned (multi-process
  // single-mode gravity lost most remote contributions, 2026-08-04).
  if (traverser->wantsPause()) {
    this->thisProxy[this->thisIndex].resumeAfterPause(0);
  }
}

template <typename Data>
template <typename Visitor>
void TreePiece<Data>::startDual(Visitor v) {
  r_local = r_proxy.ckLocalBranch();
  r_local->treepiece_proxy = this->thisProxy;
  r_local->use_treepiece = true;
  // Wire the per-PE resumer <-> cache cross-pointers the fetch/resume path
  // needs (CacheManager::process -> Resumer::process -> goDown). Dual-
  // distribution runs inherit these from Partition::initLocalBranches on
  // every PE (TreePieces are bound to Partitions); in single-distribution
  // mode there are no Partition elements, so the TreePiece-driven walk sets
  // them itself. Idempotent — same values either way. Without cm r_proxy,
  // installs notify a NULL group proxy: no walker ever resumes and the
  // undeliverable sends stall quiescence (2-process hang, 2026-08-04).
  auto* cml = cm_proxy.ckLocalBranch();
  r_local->cm_local = cml;
  cml->r_proxy = r_proxy;
  traverser.reset(new DualTraverser<Data, Visitor>(v, 0, *this));
  traverser->start();
  // Walk-completion counting (armed only in the serial-mode FoF walk):
  // every element contributes here after its seeding descent; the array
  // reduction broadcasts releaseOpenCredit, which removes the per-process
  // open credit -- so the counter cannot reach zero before every element
  // has seeded. New requests issued during start() took their own credits.
  if (cml->walkArmed()) {
    this->contribute(CkCallback(
        CkIndex_CacheManager<Data>::releaseOpenCredit(), cm_proxy));
  }
}

template <typename Data>
void TreePiece<Data>::goDown(size_t travIdx) {
  CkAssert(travIdx == 0); // cannot handle multiple dual tree traversals yet
  traverser->resumeTrav();
  // This goDown message's walk credit (added by Resumer::process before
  // the send); requests issued during the drained segments took their own.
  cm_proxy.ckLocalBranch()->walkCreditDec(1);
}

// Mirror of Partition::resumeAfterPause (see the note in startDown).
template <typename Data>
void TreePiece<Data>::resumeAfterPause(size_t travIdx) {
  traverser->resumeAfterPause();
  if (traverser->wantsPause()) {
    this->thisProxy[this->thisIndex].resumeAfterPause(travIdx);
  }
}

template <typename Data>
void TreePiece<Data>::addNodeToFlatSubtree(Node<Data>* node) {
  SpatialNode<Data> sn (*node);
  flat_subtree.nodes.emplace_back(node->key, sn);
  for (int i = 0; i < node->n_children; i++) {
    addNodeToFlatSubtree(node->getChild(i));
  }
}

template <typename Data>
void TreePiece<Data>::requestCopy(int cm_index, PPHolder<Data> pp_holder) {
  if (flat_subtree.nodes.empty()) addNodeToFlatSubtree(local_root);
  // Record which remote CacheManagers hold a copy of this TreePiece, so a later
  // post-build mutation (TreePiece::callPerLeafFn) can be pushed to them via
  // refreshCopies(). flat_subtree is a build-time snapshot shipped here BEFORE
  // any mutation, so without that push the remote copies stay stale.
  copy_requesters.push_back(cm_index);
  cm_proxy[cm_index].receiveTreePiece(flat_subtree, pp_holder);
}

// Push this TreePiece's refreshed state to every remote CacheManager that
// requested a copy of it. Those copies were shipped as a build-time
// flat_subtree snapshot (see requestCopy) and are promoted into the remote
// CacheManager's local_tps/leaf_lookup, so a down-traversal reads them as
// SOURCE leaves. Work applied after tree build -- a per-leaf mutation
// (annotate's DensityStampFn, FoF phase-1 relabeling) plus the upwardPass node
// recompute -- touches only the live nodes/particles here, leaving those
// remote snapshots stale in BOTH particle data and node annotations. This
// re-snapshots flat_subtree (mutated particles + post-upwardPass annotations)
// and ships it so the remote copy is updated in place, by key, structure
// unchanged. No-op when no remote copies exist (e.g. matching decompositions,
// where remote source leaves are fetched live via
// CacheManager::serviceRequest).
//
// CONCURRENCY: refreshTreePieceCopy mutates cached node payload that traversals
// read locklessly (see its comment in CacheManager.h). The caller MUST run
// this in a mutation phase quiescence-separated from the down-traversal, and
// MUST CkWaitQD after it (this method's callback fires when the push messages
// are SENT, not processed) so the in-place updates land before any traversal
// read. See examples/annotate preTraversalFn.
template <typename Data>
void TreePiece<Data>::refreshCopies(const CkCallback& cb) {
  if (!copy_requesters.empty()) {
    flat_subtree.setParticles(particles); // mutated particle data (converted to CachedParticle)
    flat_subtree.nodes.clear();
    addNodeToFlatSubtree(local_root);     // post-upwardPass node annotations
    for (int cm_index : copy_requesters) {
      cm_proxy[cm_index].refreshTreePieceCopy(flat_subtree);
    }
  }
  this->contribute(cb);
}

template <typename Data>
typename Node<Data>::Type TreePiece<Data>::getType(size_t num_particles, size_t max_particles_per_leaf) const {
  if (num_particles == 0) return Node<Data>::Type::EmptyLeaf;
  else if (num_particles <= max_particles_per_leaf) return Node<Data>::Type::Leaf;
  return Node<Data>::Type::Internal;
}

template <typename Data>
void TreePiece<Data>::handlePossibleLeaf(Node<Data>* node) {
  if (node->type == Node<Data>::Type::EmptyLeaf) empty_leaves.push_back(node);
  else if (node->type == Node<Data>::Type::Leaf) leaves.push_back(node);
}

template <typename Data>
void TreePiece<Data>::buildTree(CProxy_Partition<Data> part, CkCallback cb) {
  // Copy over received particles
  std::swap(particles, incoming_particles);

  // Sort particles
  std::sort(particles.begin(), particles.end());

  // Clear existing data
  leaves.clear();
  empty_leaves.clear();

  // Create global root and build local tree recursively
#if DEBUG
  CkPrintf("[TP %d] key: 0x%" PRIx64 " particles: %d\n", this->thisIndex, tp_key, particles.size());
#endif
  auto& config = paratreet::getConfiguration();
  Key lbf = log2(config.branchFactor());
  auto local_root_type = getType(particles.size(), config.max_particles_per_leaf);
  local_root = cm_local->makeNode(tp_key, local_root_type,
         Utility::getDepthFromKey(tp_key, lbf), particles.size(),
         particles.data(), nullptr, this->thisIndex, cm_local->thisIndex);
  if (local_root->isLeaf()) handlePossibleLeaf(local_root);
  else recursiveBuild(local_root, &particles[0], particles.size(), lbf);

  flat_subtree.tp_index  = this->thisIndex;
  flat_subtree.cm_index  = cm_proxy.ckLocalBranch()->thisIndex;
  flat_subtree.setParticles(particles);

  // Populate the tree structure (including TreeCanopy)
  populateTree();
  thread_state_holder.ckLocalBranch()->countTreePieceParticles(particles.size());
  initCache();

  this->contribute(cb);
  // Single-distribution mode has no Partition array to feed (part is a
  // null proxy); traversal targets are this TreePiece's own leaves.
  if (!paratreet::getConfiguration().single_distribution) sendLeaves(part);
}

template <typename Data>
void TreePiece<Data>::recursiveBuild(Node<Data>* node, Particle* node_particles, size_t node_n_particles, size_t log_branch_factor) {
#if DEBUG
  CkPrintf("[Level %d] created node 0x%" PRIx64 " with %d particles\n",
      node->depth, node->key, node_n_particles);
#endif
  // store reference to splitters
  //static std::vector<Splitter>& splitters = readers.ckLocalBranch()->splitters;
  auto& config = paratreet::getConfiguration();
  auto tree   = treespec.ckLocalBranch()->getTree();

  // Create children
  Key child_key = (node->key << log_branch_factor);
  int start = 0;
  int finish = start + node_n_particles;

  tree->prepParticles(node_particles, node_n_particles, node->depth);
  for (int i = 0; i < node->n_children; i++) {
    int first_ge_idx = finish;
    if (i < node->n_children - 1) {
      first_ge_idx = tree->findChildsLastParticle(node_particles, start, finish, child_key, log_branch_factor);
    }
    int n_particles = first_ge_idx - start;

    // Create child and store in vector
    Node<Data>* child = cm_local->makeNode(child_key, getType(n_particles, config.max_particles_per_leaf),
        node->depth + 1, n_particles, node_particles + start, node, this->thisIndex, cm_local->thisIndex);
    node->exchangeChild(i, child);

    // Recursive tree build
    if (child->isLeaf()) handlePossibleLeaf(child);
    else recursiveBuild(child, node_particles + start, n_particles, log_branch_factor);

    start = first_ge_idx;
    child_key++;
  }
}

template <typename Data>
void TreePiece<Data>::populateTree() {
  // Populates the global tree structure by going up the tree
  std::queue<Node<Data>*> going_up;

  for (auto leaf : leaves) {
    going_up.push(leaf);
  }
  for (auto empty_leaf : empty_leaves) {
    going_up.push(empty_leaf);
  }
  CkAssert(!going_up.empty());

  auto branch_factor = paratreet::getConfiguration().branchFactor();
  while (going_up.size()) {
    Node<Data>* node = going_up.front();
    going_up.pop();
    CkAssert(node);
    if (node->key == tp_key) {
      // We are at the root of the TreePiece, send accumulated data to
      // parent TreeCanopy
      Key tc_key = tp_key / branch_factor;
      if (tc_key > 0) tc_proxy[tc_key].recvData(*node, branch_factor);
    } else {
      // Add this node's data to the parent, and add parent to the queue
      // if all children have contributed
      Node<Data>* parent = node->parent;
      CkAssert(parent);
      parent->data += node->data;
      parent->wait_count--;
      if (parent->wait_count == 0) going_up.push(parent);
    }
  }
}

// Recompute node Data bottom-up over the already-built local tree, then
// re-propagate the TreePiece root's Data to the TreeCanopy above. For use
// after a phase that modifies per-particle state (e.g. FoF fragment
// assignment): the tree structure is unchanged, only annotations refresh.
// Contract: run this before any traversal that reads the new annotations
// issues remote fetches; cached copies shipped in earlier traversals are
// not invalidated by this pass.
template <typename Data>
void TreePiece<Data>::upwardPass(const CkCallback& cb) {
  recomputeData(local_root);
  auto branch_factor = paratreet::getConfiguration().branchFactor();
  Key tc_key = tp_key / branch_factor;
  if (tc_key > 0) tc_proxy[tc_key].recvData(*local_root, branch_factor);
  this->contribute(cb);
}

template <typename Data>
void TreePiece<Data>::recomputeData(Node<Data>* node) {
  if (node->isLeaf()) {
    node->data = Data(node->particles(), node->n_particles, node->depth);
    return;
  }
  node->data = Data();
  for (int i = 0; i < node->n_children; i++) {
    Node<Data>* child = node->getChild(i);
    recomputeData(child);
    node->data += child->data;
  }
}

// First leapfrog half-kick on this TreePiece's leaves (the single-
// distribution twin of Partition::kick; under matching decompositions the
// leaves are the same objects either way).
template <typename Data>
void TreePiece<Data>::kick(Real timestep, const CkCallback& cb) {
  for (auto && leaf : leaves) {
    leaf->kick(timestep);
  }
  this->contribute(cb);
}

// Second leapfrog half-kick + drift on this TreePiece's owned particles,
// contributing the moved bounding box (the single-distribution twin of
// Partition::perturb, minus the deletion filter and the per-element load
// user data — see the declaration comment).
template <typename Data>
void TreePiece<Data>::perturb(Real timestep, const CkCallback& cb) {
  BoundingBox box;
  for (auto && p : particles) {
    p.perturb(timestep);
    box.grow(p.position);
    box.mass += p.mass;
    box.ke += 0.5 * p.mass * p.velocity.lengthSquared();
    if (p.isGas()) box.n_sph++;
    if (p.isDark()) box.n_dark++;
    if (p.isStar()) box.n_star++;
  }
  box.n_particles = particles.size();
  this->contribute(sizeof(BoundingBox), &box, BoundingBox::reducer(), cb);
}

// Re-home the moved particles (the single-distribution twin of
// Partition::rebuild): adjustNewUniverse re-generates each particle's key
// against the driver's re-made universe, then either every particle goes
// back to the Readers for a full re-sorting decomposition (if_flush) or
// straight to the TreePiece whose key range now contains it, through the
// EXISTING splitters. Receivers append to incoming_particles (receive),
// which the next buildTree swaps in — and which pup ships if load
// balancing migrates this element afterwards (same safety argument as the
// Partition path: the live tree is rebuilt from scratch every iteration).
template <typename Data>
void TreePiece<Data>::rebucket(BoundingBox universe, bool if_flush) {
  thread_state_holder.ckLocalBranch()->countPartitionParticles(particles.size());
  for (auto && p : particles) {
    p.adjustNewUniverse(universe.box);
  }
  if (if_flush) {
    ParticleMsg* msg = new (particles.size()) ParticleMsg(
      particles.data(), particles.size()
      );
    readers[CkMyPe()].receive(msg);
  } else {
    auto sendParticles = [&](int dest, int n_particles, Particle* parts) {
      ParticleMsg* msg = new (n_particles) ParticleMsg(parts, n_particles);
      this->thisProxy[dest].receive(msg);
    };
    treespec.ckLocalBranch()->getTreePieceDecomposition()->flush(particles, sendParticles);
  }
  particles.clear();
}

// Generic TreePiece-level hook (see paratreet::PerTreePieceAble): applies fn once
// per TreePiece element, handing it the element's local tree root and its
// contiguous particle block. The block (particles.data()) is stable from the
// end of tree build until the next rebuild/reset; a consumer that retains the
// pointers must finish inside that window. Elements with no local tree or no
// particles contribute without calling fn.
template <typename Data>
void TreePiece<Data>::callPerTreePieceFn(paratreet::PerTreePieceAble<Data>& fn, const CkCallback& cb) {
  if (local_root != nullptr && !particles.empty()) {
    fn(local_root, particles.data(), (int)particles.size());
  }
  this->contribute(cb);
}

// TreePiece-side analogue of Partition::callPerLeafFn: applies fn to the
// TreePiece's own particle copies (the ones traversals fetch through the
// cache), not the partition copies. The Partition* argument is null.
template <typename Data>
void TreePiece<Data>::callPerLeafFn(paratreet::PerLeafAble<Data>& fn, const CkCallback& cb) {
  for (auto && leaf : leaves) {
    fn(*leaf, nullptr);
  }
  this->contribute(cb);
}

template <typename Data>
void TreePiece<Data>::initCache() {
  cm_proxy.ckLocalBranch()->connect(local_root);
}

template <typename Data>
void TreePiece<Data>::requestNodes(Key key, int cm_index) {
  if (cm_index == cm_proxy.ckLocalBranch()->thisIndex) return;
  Node<Data>* node = local_root->getDescendant(key);
  if (!node) CkPrintf("null found for key %lu on tp %d\n", key, this->thisIndex);
  cm_proxy.ckLocalBranch()->serviceRequest(node, cm_index);
}

template <typename Data>
void TreePiece<Data>::reset() {
  particles.clear();
  flat_subtree.clear();
  copy_requesters.clear();
}

template <typename Data>
void TreePiece<Data>::destroy() {
  reset();
  this->thisProxy[this->thisIndex].ckDestroy();
}

template <typename Data>
void TreePiece<Data>::print(Node<Data>* node) {
  ostringstream oss;
  oss << "tree." << this->thisIndex << ".dot";
  ofstream out(oss.str().c_str());
  CkAssert(out.is_open());
  out << "digraph tree" << this->thisIndex << "{" << endl;
  node->dot(out);
  out << "}" << endl;
  out.close();
}

#endif /* _SUBTREE_H_ */
