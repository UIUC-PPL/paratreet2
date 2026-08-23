#ifndef PARATREET_DRIVER_H_
#define PARATREET_DRIVER_H_

#include "paratreet.decl.h"

#include "CoreFunctions.h"

#include <algorithm>
#include <vector>

#include <numeric>
#include "Reader.h"
#include "Splitter.h"
#include "TreeCanopy.h"
#include "TreeSpec.h"
#include "BoundingBox.h"
#include "BufferedVec.h"
#include "Utility.h"
#include "CacheManager.h"
#include "Resumer.h"
#include "ThreadStateHolder.h"
#include "Modularization.h"
#include "Node.h"
#include "Writer.h"
#include "TreePiece.h"

extern CProxy_Reader readers;
extern CProxy_TreeSpec treespec;
extern CProxy_ThreadStateHolder thread_state_holder;

template <typename Data>
class Driver : public CBase_Driver<Data> {
public:
  CProxy_TreeCanopy<Data> calculator;
  CProxy_CacheManager<Data> cache_manager;
  CProxy_Resumer<Data> resumer;
  std::vector<std::pair<Key, SpatialNode<Data>>> storage;
  bool storage_sorted;
  BoundingBox universe;
  CProxy_TreePiece<Data> treepieces; // Cannot be a global readonly variable
  CProxy_Partition<Data> partitions;
  int n_treepieces;
  int n_partitions;
  double start_time;
  std::vector<int> partition_locations;

  Driver(CProxy_CacheManager<Data> cache_manager_, CProxy_Resumer<Data> resumer_, CProxy_TreeCanopy<Data> calculator_) :
    cache_manager(cache_manager_), resumer(resumer_), calculator(calculator_), storage_sorted(false) {}

  // Performs initial decomposition
  void init(const CkCallback& cb, const paratreet::Configuration& cfg) {
    // TreeCanopy has no initial elements and relies entirely on demand
    // creation later during tree traversal, so unlike TreePiece/Partition
    // (whose ckNew() below is synchronized via a completion callback), its
    // creation in paratreet::initialize() has nothing to wait on. CkWaitQD()
    // was tried here but hangs on this reconverse runtime even once the
    // system has genuinely gone idle, so instead force a plain reduction
    // (thread_state_holder is a readonly Group, broadcast from the same PE
    // and before this point, so same-source delivery order guarantees its
    // own creation -- and, before it, TreeCanopy's -- already landed
    // everywhere by the time every branch replies here).
    thread_state_holder.ping(CkCallbackResumeThread());

    // Ensure all treespecs have been created
    CkPrintf("* Validating tree specifications.\n");
    treespec.receiveConfiguration(
      CkCallbackResumeThread(),
      CkReference<paratreet::Configuration>(const_cast<paratreet::Configuration&>(cfg))
    );
    // Then, initialize the cache managers
    CkPrintf("* Initializing cache managers.\n");
    cache_manager.initialize(CkCallbackResumeThread());
    // Useful particle keys
    CkPrintf("* Initialization\n");
    decompose(0);
    cb.send();
  }

  void remakeUniverse() {
    Vector3D<Real> bsize = universe.box.size();
    Real max = (bsize.x > bsize.y) ? bsize.x : bsize.y;
    max = (max > bsize.z) ? max : bsize.z;
    Vector3D<Real> bcenter = universe.box.center();
    // The magic number below is approximately 2^(-19)
    const Real fEps = 1.0 + 1.91e-6;  // slop to ensure keys fall between 0 and 1.
    bsize = Vector3D<Real>(fEps*0.5*max);
    universe.box = OrientedBox<Real>(bcenter-bsize, bcenter+bsize);
    thread_state_holder.setUniverse(universe);

    std::cout << "Universal bounding box: " << universe << " with volume "
      << universe.box.volume() << std::endl;
  }

  void partitionLocation(int partition_idx, int home_pe) {
     partition_locations[partition_idx] = home_pe;
  }

  // Performs decomposition by distributing particles among TreePieces,
  // by either loading particle information from input file or re-computing
  // the universal bounding box
  void decompose(int iter) {
    auto& config = paratreet::getConfiguration();
    double decomp_time = CkWallTimer();
    if (iter == 0) {
      // Build universe
      start_time = CkWallTimer();
      CkReductionMsg* result;
      readers.load(config.input_file, CkCallbackResumeThread((void*&)result));
      CkPrintf("Loading input data and building universe: %.3lf ms\n",
          (CkWallTimer() - start_time) * 1000);
      
      if(config.origin_of("dSoft") != paratreet::FieldOrigin::Unknown) {
      	  CkPrintf("Setting softening to %f \n", config.dSoft);
          // Softening is specified: set it for all particles.
          readers.setSoft(config.dSoft, CkCallbackResumeThread());
      }
      universe = *((BoundingBox*)result->getData());
      delete result;
      remakeUniverse();
      if (config.min_n_treepieces < CkNumPes() || config.min_n_partitions < CkNumPes()) {
        CkPrintf("WARNING: Consider increasing min_n_treepieces and min_n_partitions to at least #pes\n");
      }
      // Assign keys and sort particles locally
      start_time = CkWallTimer();
      readers.assignKeys(universe, CkCallbackResumeThread());
      CkPrintf("Assigning keys and sorting particles: %.3lf ms\n",
        (CkWallTimer() - start_time) * 1000);
    } else CkWaitQD();

    bool matching_decomps = config.decomp_type == paratreet::treepieceDecompForTree(config.tree_type);
    // Single-distribution mode (design/single-distribution-mode.md): no
    // Partition array at all. One splitter computation (below, shared with
    // the dual-distribution matching path), no partition assignment pass,
    // TreePieces placed by their own decomposition map instead of bindTo.
    // Restrictions enforced here; the movement machinery (kick/perturb/
    // rebuild) is Partition-resident, so moving apps stay dual for now.
    bool single = config.single_distribution;
    if (single && !matching_decomps)
      CkAbort("single_distribution requires matching decomposition and tree "
              "types (e.g. -d oct with the oct tree)");
    // Set up splitters for decomposition
    start_time = CkWallTimer();
    n_partitions = treespec.ckLocalBranch()->getPartitionDecomposition()->findSplitters(universe, readers, config.min_n_partitions);
    partition_locations.resize(n_partitions);
    treespec.receiveDecomposition(CkCallbackResumeThread(),
        CkPointer<Decomposition>(treespec.ckLocalBranch()->getPartitionDecomposition()), false);
    CkPrintf("Setting up splitters for particle decompositions: %.3lf ms\n",
        (CkWallTimer() - start_time) * 1000);

    if (single) {
      // No Partition array: reuse the splitters just computed as the
      // TreePiece decomposition (the matching-decomps identity), create
      // TreePieces on their own decomposition map, and flush particles
      // straight to them. partitions stays a null proxy; ProxyPack
      // carries it as such and single-mode apps must not use it.
      n_treepieces = n_partitions;
      treespec.receiveDecomposition(CkCallbackResumeThread(),
        CkPointer<Decomposition>(treespec.ckLocalBranch()->getPartitionDecomposition()), true);

      start_time = CkWallTimer();
      CkArrayOptions treepiece_opts(n_treepieces);
      treespec.ckLocalBranch()->getTreePieceDecomposition()->setArrayOpts(treepiece_opts, {}, false);
      // Same map-creation ordering hazard as the dual-distribution path
      // below (see that comment): declare the fresh map as a user group
      // dependency of the array creation.
      CkEntryOptions sub_dep_opts;
      if (!treepiece_opts.getMap().isZero())
        sub_dep_opts.setGroupDepID(treepiece_opts.getMap());
      treepieces = CProxy_TreePiece<Data>::ckNew(
        CkCallbackResumeThread(),
        universe.n_particles, n_treepieces, n_partitions,
        calculator, resumer,
        cache_manager, this->thisProxy, matching_decomps, treepiece_opts,
        &sub_dep_opts
        );
      CkPrintf("Created %d TreePieces (single distribution): %.3lf ms\n",
          n_treepieces, (CkWallTimer() - start_time) * 1000);

      start_time = CkWallTimer();
      readers.flush(n_treepieces, treepieces);
      CkStartQD(CkCallbackResumeThread());
      CkPrintf("Flushing particles to TreePieces: %.3lf ms\n",
          (CkWallTimer() - start_time) * 1000);
      // Peak RSS of THIS process (PE 0's) since start. The flush is the largest
      // memory event in decomposition, so this is the number the flush window is
      // meant to hold down; compare across PARATREET_FLUSH_WINDOW settings.
      // One process only -- it does not capture the skew that decides which
      // rank actually OOMs, so read it as an A/B signal, not an absolute bound.
      CkPrintf("PARATREET vmhwm_mb after decomposition: %ld (flush window %ld)\n",
          paratreet::procHighWaterMB(), Reader::flushWindow());
      CkPrintf("**Total Decomposition time: %.3lf ms\n",
          (CkWallTimer() - decomp_time) * 1000);
      return;
    }

    // Create Partitions
    CkArrayOptions partition_opts(n_partitions);
    treespec.ckLocalBranch()->getPartitionDecomposition()->setArrayOpts(partition_opts, {}, false);
    // setArrayOpts may have ckNew'd a fresh array-map GROUP, and this
    // Driver creates maps and arrays POST-INIT from a threaded entry (the
    // tree rebuilds per iteration). Context (Kale): creations made in
    // MAINCHARE CONSTRUCTORS are batched from PE 0 with sequence
    // information and installed everywhere in creation order before the
    // scheduler runs anything else, so group-then-array is unconditionally
    // safe there. Post-init, ordering rests only on per-message group
    // DEPENDENCIES. Those cover the plain setMap path (CkCreateArray
    // chains locCache -> map) but NOT bindTo + fresh map: bindTo reuses
    // the bound-to array's location manager, skips that block, and
    // declares no dependency on the new map, so the CkArray constructor's
    // map lookup (ckarray.C:912) races the map-creation broadcast:
    // CkAbort "Local branch of array map is NULL!" on a remote process.
    // Both classic Converse and reconverse (shared Ck layer); probability
    // grows with process count (seen at 32 processes on Anvil,
    // 2026-07-24; never lost on loopback).
    //
    // Fix: declare the map as a USER group dependency of the array
    // creation message (CkEntryOptions::setGroupDepID; CkCreateArray
    // copies user dependencies onto the CkArray group creation). Each PE
    // then buffers the array creation until its map branch exists — no
    // barrier, no quiescence, no waiting thread; the message itself
    // waits. (An earlier version used CkStartQD here: correct but far too
    // broad — quiescence requires that NO other computation is in flight,
    // which a per-iteration framework path must not assume. A reduction
    // over the map branches is also unavailable: CkArrayMap derives from
    // IrrGroup, not Group, so map constructors cannot contribute().)
    // Upstream charm candidate: bindTo + fresh setMap should declare this
    // dependency itself.
    CkEntryOptions part_dep_opts;
    if (!partition_opts.getMap().isZero())
      part_dep_opts.setGroupDepID(partition_opts.getMap());
    partitions = CProxy_Partition<Data>::ckNew(
      n_partitions, cache_manager, resumer, calculator,
      this->thisProxy, matching_decomps, partition_opts, &part_dep_opts
      );
    CkPrintf("Created %d Partitions: %.3lf ms\n", n_partitions,
        (CkWallTimer() - start_time) * 1000);
    // Storing partition proxy for global access for FoF
    // partitionProxy<Data> = partitions;

    start_time = CkWallTimer();
    readers.assignPartitions(n_partitions, partitions);
    CkStartQD(CkCallbackResumeThread());
    CkPrintf("Assigning particles to Partitions: %.3lf ms\n",
        (CkWallTimer() - start_time) * 1000);

    start_time = CkWallTimer();
    if (matching_decomps) {
      n_treepieces = n_partitions;
      CkPrintf("Using same decomposition for TreePieces and Partitions\n");
      treespec.receiveDecomposition(CkCallbackResumeThread(),
        CkPointer<Decomposition>(treespec.ckLocalBranch()->getPartitionDecomposition()), true);
    }
    else {
      n_treepieces = treespec.ckLocalBranch()->getTreePieceDecomposition()->findSplitters(universe, readers, config.min_n_treepieces);
      treespec.receiveDecomposition(CkCallbackResumeThread(),
        CkPointer<Decomposition>(treespec.ckLocalBranch()->getTreePieceDecomposition()), true);
      CkPrintf("Setting up splitters for TreePiece decompositions: %.3lf ms\n",
          (CkWallTimer() - start_time) * 1000);
    }

    // Create TreePieces
    start_time = CkWallTimer();
    CkArrayOptions treepiece_opts(n_treepieces);
    if (matching_decomps) treepiece_opts.bindTo(partitions);
    treespec.ckLocalBranch()->getTreePieceDecomposition()->setArrayOpts(treepiece_opts, partition_locations, !matching_decomps);
    // Same map dependency as for Partitions above — and THIS is the
    // exposed site: bindTo(partitions) + the fresh DecompArrayMap from
    // setArrayOpts is exactly the unprotected combination (see the
    // comment at the Partition creation).
    CkEntryOptions sub_dep_opts;
    if (!treepiece_opts.getMap().isZero())
      sub_dep_opts.setGroupDepID(treepiece_opts.getMap());
    treepieces = CProxy_TreePiece<Data>::ckNew(
      CkCallbackResumeThread(),
      universe.n_particles, n_treepieces, n_partitions,
      calculator, resumer,
      cache_manager, this->thisProxy, matching_decomps, treepiece_opts,
      &sub_dep_opts
      );
    CkPrintf("Created %d TreePieces: %.3lf ms\n", n_treepieces,
        (CkWallTimer() - start_time) * 1000);

    start_time = CkWallTimer();
    readers.flush(n_treepieces, treepieces);
    CkStartQD(CkCallbackResumeThread());
    CkPrintf("Flushing particles to TreePieces: %.3lf ms\n",
        (CkWallTimer() - start_time) * 1000);
    CkPrintf("**Total Decomposition time: %.3lf ms\n",
        (CkWallTimer() - decomp_time) * 1000);
    
  }

  // Core iterative loop of the simulation
  void run(CkCallback cb) {
    auto& config = paratreet::getConfiguration();
    double total_time = 0;
    for (int iter = 0; iter < config.num_iterations; iter++) {
      CkPrintf("\n* Iteration %d\n", iter);
      double iter_start_time = CkWallTimer();
      // Start tree build in TreePieces
      start_time = CkWallTimer();
      CkCallback timeCb (CkReductionTarget(Driver<Data>, reportTime), this->thisProxy);
      treepieces.buildTree(partitions, timeCb);
      CkWaitQD();
      CkPrintf("Tree build and sending leaves: %.3lf ms\n", (CkWallTimer() - start_time) * 1000);

      // Meta data collection (max velocity -> timestep). Its only
      // consumers are the kick/perturb path and the [Meta] print, so
      // non-perturbing apps (fof3: perturb_particles = false) skip the
      // whole pass — it streams every particle's velocity (the first
      // touch of that memory after the exchange, ~all cache misses) and
      // serializes at a reduction on the slowest PE (observed: one PE
      // busy ~30 ms while every other PE idled; Kale, 2026-08-11).
      CkReductionMsg * msg = nullptr, *msg2;
      Real max_velocity = 0.0;
      Real timestep_size = 0.0;
      int numRedn = 0, numRedn2 = 0;
      CkReduction::tupleElement* res = nullptr, *res2 = nullptr;
      if (config.perturb_particles) {
        treepieces.collectMetaData(CkCallbackResumeThread((void *&) msg));
        msg->toTuple(&res, &numRedn);
        max_velocity = *(Real*)(res[0].data); // avoid max_velocity = 0.0
        timestep_size = paratreet::getTimestep(universe, max_velocity);
      }

      ProxyPack<Data> proxy_pack (this->thisProxy, treepieces, partitions, cache_manager);

      // Prefetch into cache
      start_time = CkWallTimer();
      // use exactly one of these three commands to load the software cache
      paratreet::preTraversalFn(proxy_pack);
      CkWaitQD();
      // This bracket spans the WHOLE app preTraversalFn — for fof3 that is
      // all of phase 1 + tip encoding + upwardPass + the cache load, so the
      // old label "TreeCanopy cache loading" mis-attributed seconds of
      // phase-1 work to the cache (caught by Kale, 2026-08-11; the true
      // cache-load time is the app-side FOF3STAT loadCache field).
      CkPrintf("Pre-traversal (app preTraversalFn, includes cache load): %.3lf ms\n",
          (CkWallTimer() - start_time) * 1000);


      // Perform traversals
      start_time = CkWallTimer();

      //print traversal start time
      CkPrintf("Starting tree traversal at time %.3f\n", start_time);

      paratreet::traversalFn(universe, proxy_pack, iter);
      CkWaitQD();
      CkPrintf("Tree traversal: %.3lf ms\n", (CkWallTimer() - start_time) * 1000);


      start_time = CkWallTimer();

      // The end-of-iteration movement machinery (kick, perturb, rebuild)
      // exists for apps that move particles. A static-analysis app
      // (config.perturb_particles == false, e.g. FoF) skips it: particles
      // did not move, so the kick/perturb sweeps and the rebuild's particle
      // exchange are pure overhead, and the universe box is unchanged.
      // postIterationFn always runs — it is an app hook, not movement.
      bool complete_rebuild = false;
      bool single_iter = config.single_distribution;
      if (config.perturb_particles) {
        // Move the particles: first leapfrog half-kick, on whichever array
        // owns the traversal targets in this mode.
        if (single_iter) treepieces.kick(timestep_size, CkCallbackResumeThread());
        else partitions.kick(timestep_size, CkCallbackResumeThread());

        // Now track PE imbalance for memory reasons
        thread_state_holder.collectMetaData(CkCallbackResumeThread((void *&) msg2));

        msg2->toTuple(&res2, &numRedn2);

        long numParticleCopies = *(long*)(res2[2].data);
        long numParticleShares = *(long*)(res2[3].data);
        long maxPESize = *(long*)(res2[0].data);
        long sumPESize = *(long*)(res2[1].data);
        float avgPESize = (float) universe.n_particles / (float) CkNumPes();
        float ratio = (float) maxPESize / avgPESize;
        complete_rebuild = (config.flush_period == 0) ?
            (ratio > config.flush_max_avg_ratio || numParticleShares * 10 > universe.n_particles) :
            (iter % config.flush_period == config.flush_period - 1);

        if (iter + 1 == config.num_iterations) complete_rebuild = false;
        CkPrintf("[Meta] n_treepiece = %d; timestep_size = %f; numPSParticleCopies = %ld; numPSParticleShares = %ld; sumPESize = %ld; maxPESize = %ld, avgPESize = %f; ratio = %f; maxVelocity = %f; rebuild = %s\n", n_treepieces, timestep_size, numParticleCopies, numParticleShares, sumPESize, maxPESize, avgPESize, ratio, max_velocity, (complete_rebuild? "yes" : "no"));
      }
      //End TreePiece reduction message parsing

      paratreet::postIterationFn(universe, proxy_pack, iter);

      if (config.perturb_particles) {
        CkReductionMsg* result;
        if (single_iter) treepieces.perturb(timestep_size, CkCallbackResumeThread((void *&)result));
        else partitions.perturb(timestep_size, CkCallbackResumeThread((void *&)result));

        universe = *((BoundingBox*)result->getData());
        delete result;
        remakeUniverse();
        if (single_iter) treepieces.rebucket(universe, complete_rebuild);
        else partitions.rebuild(universe, treepieces, complete_rebuild); // 0.1s for example

        CkWaitQD();
        CkPrintf("Perturbations: %.3lf ms\n", (CkWallTimer() - start_time) * 1000);
      }
      // Load balancing: Partitions drive it in dual mode, TreePieces in
      // single mode (phase B — TreePiece registers for AtSync only in
      // single mode, and its pup ships incoming_particles, which the
      // rebucket above just filled: exactly the state the next build
      // needs).
      bool single = config.single_distribution;
      if (!complete_rebuild && config.lb_period > 0 && iter % config.lb_period == config.lb_period - 1 && iter != config.num_iterations - 1) {
        start_time = CkWallTimer();
        CkPrintf("Starting load balancing...\n");
        if (single) treepieces.pauseForLB();
        else partitions.pauseForLB();
        CkWaitQD();
        CkPrintf("Load balancing: %.3lf ms\n", (CkWallTimer() - start_time) * 1000);
      }
      // Destroy TreePieces and perform decomposition from scratch
      resumer.reset();
      if (complete_rebuild) {
        treespec.reset();
        treepieces.destroy();
        if (!single) partitions.destroy();
        decompose(iter+1);
      } else {
        if (!single) partitions.reset();
        treepieces.reset();
      }

      // Clear cache and other storages used in this iteration
      cache_manager.destroy(true);
      CkCallback statsCb (CkReductionTarget(Driver<Data>, countInts), this->thisProxy);
      thread_state_holder.collectAndResetStats(statsCb);
      storage.clear();
      storage_sorted = false;
      CkWaitQD();
      double iter_time = CkWallTimer() - iter_start_time;
      total_time += iter_time;
      CkPrintf("Iteration %d time: %.3lf ms\n", iter, iter_time * 1000);
      if (iter == config.num_iterations-1) {
        CkPrintf("Average iteration time: %.3lf ms\n", total_time / config.num_iterations * 1000);
      }
    }

    cb.send();
  }

  // -------------------
  // Auxiliary functions
  // -------------------
  void reportTime(){
    CkPrintf("Tree build: %.3lf ms\n", (CkWallTimer() - start_time) * 1000);
  }

  void countInts(unsigned long long* intrn_counts) {
     CkPrintf("%llu node-particle interactions, %llu bucket-particle interactions %llu node opens, %llu node closes\n", intrn_counts[0], intrn_counts[1], intrn_counts[2], intrn_counts[3]);
  }

  void recvTC(std::pair<Key, SpatialNode<Data>> param) {
    storage.emplace_back(param);
    // A later annotation round (TreePiece::upwardPass) may re-send canopies
    // after a loadCache already sorted; force a re-sort (and keep-newest
    // dedup, see sortStorage) before the next ship.
    storage_sorted = false;
  }

  void loadCache(CkCallback cb) {
    auto& config = paratreet::getConfiguration();
    CkPrintf("Received data from %d TreeCanopies\n", (int) storage.size());
    // Sort data received from TreeCanopies (by their indices)
    const double lc_sort_t0 = CkWallTimer();   // relay92: pack build
    const long lc_raw = (long)storage.size();
    if (!storage_sorted) sortStorage();
    const double lc_sort_s = CkWallTimer() - lc_sort_t0;

    // Find how many should be sent to the caches
    int send_size = storage.size();
    if (config.num_share_nodes > 0 && config.num_share_nodes < send_size) {
      send_size = config.num_share_nodes;
    }
    else {
      CkPrintf("Broadcasting every tree canopy because num_share_nodes is unset\n");
    }

    CkPrintf("FOF3STAT loadcache_pack: raw_canopies %ld deduped %ld shipped %d "
             "sort_s %.4f entry_B %d pack_MB %.2f\n",
             lc_raw, (long)storage.size(), send_size, lc_sort_s,
             (int)sizeof(std::pair<Key, SpatialNode<Data>>),
             send_size * (double)sizeof(std::pair<Key, SpatialNode<Data>>) / 1e6);
    // Send data to caches
    cache_manager.recvStarterPack(storage.data(), send_size, cb);
  }

  void sortStorage() {
    auto comp = [] (const std::pair<Key, SpatialNode<Data>>& a, const std::pair<Key, SpatialNode<Data>>& b) {return a.first < b.first;};
    // stable_sort + keep-newest dedup: TreeCanopy re-sends every canopy on
    // each accumulation round (initial build, then each TreePiece::upwardPass),
    // and recvTC appends, so storage can hold several generations per key.
    // Later arrivals supersede earlier ones (e.g. post-FoF-phase-1 FragData
    // vs build-time garbage); an unstable sort would ship an arbitrary
    // generation. Keep only the last occurrence per key.
    std::stable_sort(storage.begin(), storage.end(), comp);
    auto out = storage.begin();
    for (auto it = storage.begin(); it != storage.end(); ) {
      auto next = it + 1;
      while (next != storage.end() && next->first == it->first) it = next++;
      if (out != it) *out = std::move(*it);
      ++out;
      it = next;
    }
    storage.erase(out, storage.end());
    storage_sorted = true;
  }

  template <typename Visitor>
  void prefetch(Data nodewide_data, int cm_index, CkCallback cb) { // TODO
    CkAssert(false);
    // do traversal on the root, send everything
    /*if (!storage_sorted) sortStorage();
    std::queue<int> node_indices; // better for cache. plus no requirement here on order
    node_indices.push(0);
    std::vector<std::pair<Key, Data>> to_send;
    Visitor v;
    typename std::vector<std::pair<Key, Data> >::iterator it;

    while (node_indices.size()) {
      std::pair<Key, Data> node = storage[node_indices.front()];
      node_indices.pop();
      to_send.push_back(node);

      Node<Data> dummy_node1, dummy_node2;
      dummy_node1.data = node.second;
      dummy_node2.data = nodewide_data;
      if (v.cell(*dummy_node1, *dummy_node2)) {

        for (int i = 0; i < BRANCH_FACTOR; i++) {
          Key key = node.first * BRANCH_FACTOR + i;
          auto comp = [] (auto && a, auto && b) {return a.first < b.first;};
          it = std::lower_bound(storage.begin(), storage.end(), std::make_pair(key, Data()), comp);
          if (it != storage.end() && it->first == key) {
            node_indices.push(std::distance(storage.begin(), it));
          }
        }
      }
    }
    cache_manager[cm_index].recvStarterPack(to_send.data(), to_send.size(), cb);
    */
  }

  void request(Key* request_list, int list_size, int cm_index, CkCallback cb) {
    if (!storage_sorted) sortStorage();
    std::vector<std::pair<Key, SpatialNode<Data>>> to_send;
    for (int i = 0; i < list_size; i++) {
      Key key = request_list[i];
      auto comp = [] (const std::pair<Key, SpatialNode<Data>>& a, const Key & b) {return a.first < b;};
      auto it = std::lower_bound(storage.begin(), storage.end(), key, comp);
      if (it != storage.end() && it->first == key) {
        to_send.push_back(*it);
      }
    }
    cache_manager[cm_index].recvStarterPack(to_send.data(), to_send.size(), cb);
  }
};

#endif // PARATREET_DRIVER_H_
