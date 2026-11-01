#ifndef PARATREET_THREADSTATEHOLDER_H_
#define PARATREET_THREADSTATEHOLDER_H_

#include "paratreet.decl.h"
#include "common.h"
#include "Particle.h"

class ThreadStateHolder : public CBase_ThreadStateHolder {
public: // these need to be seen by other local chares
  unsigned long long n_part_ints = 0ull;
  unsigned long long n_node_ints = 0ull;
  unsigned long long n_opens     = 0ull;
  unsigned long long n_closes    = 0ull;
  // 64-bit: process-summed totals reach N (> 2^31 at 2B+ particles).
  unsigned long n_partition_particles = 0u;
  unsigned long n_treepiece_particles   = 0u;
  unsigned long n_ps_copies           = 0u;
  unsigned long n_ps_shares           = 0u;

  // Dual-walk timing on this PE (DualTraverser::doTrav; issue #5). busy =
  // time inside doTrav; first/last = ABSOLUTE wall times (CkWallTimer +
  // CmiInitTime, comparable across processes) of the first entry and last
  // exit; fetches = remote-node requests this PE issued. Cleared by
  // resetWalk() before each walk the caller wants to measure.
  double walk_busy = 0.0;
  double walk_first = -1.0;
  double walk_last = 0.0;
  unsigned long walk_fetches = 0u;
  // Scheduler idle time on this PE, accumulated from the Converse
  // BEGIN/END_IDLE conditions once hookIdle() has run, and its value at the
  // walk's first entry / last exit: span - busy - (idle_at_last -
  // idle_at_first) is the time this PE spent on OTHER work during its walk
  // span (cache service, resumes, union handling).
  double idle_cum = 0.0;
  double idle_since = -1.0;
  double walk_idle_at_first = 0.0;
  double walk_idle_at_last = 0.0;
  void resetWalk() {
    walk_busy = 0.0;
    walk_first = -1.0;
    walk_last = 0.0;
    walk_fetches = 0u;
    walk_idle_at_first = walk_idle_at_last = idle_cum;
  }
  void hookIdle() {
    if (idle_hooked_) return;
    idle_hooked_ = true;
    CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE, beginIdle, this);
    CcdCallOnConditionKeep(CcdPROCESSOR_END_IDLE, endIdle, this);
  }
  static void beginIdle(void* self) {
    static_cast<ThreadStateHolder*>(self)->idle_since = CkWallTimer();
  }
  static void endIdle(void* self) {
    auto* t = static_cast<ThreadStateHolder*>(self);
    if (t->idle_since >= 0) t->idle_cum += CkWallTimer() - t->idle_since;
    t->idle_since = -1.0;
  }

  BoundingBox universe;

private:
  bool idle_hooked_ = false;
  std::map<int, std::map<Key, Particle::Effect>> opposing_effects; // (partition, pKey, effect)

public:
  void collectAndResetStats(CkCallback cb);
  void collectMetaData (const CkCallback & cb);

  void setUniverse(BoundingBox universe_) {
    universe = universe_;
  }

  // Unconditional broadcast/reduce no-op, used as a barrier: every group
  // branch must exist and run this before the reduction completes. Prefer
  // this over CkWaitQD()/CkStartQD() for startup synchronization -- on this
  // reconverse runtime, quiescence detection can fail to fire even once the
  // whole system has genuinely gone idle, hanging forever, whereas a plain
  // reduction (proven reliable elsewhere in this codebase) always completes.
  void ping(CkCallback cb) {
    contribute(cb);
  }

  void reset() {
    n_part_ints = n_node_ints = n_opens = n_closes = 0ull;
    n_partition_particles = n_treepiece_particles = 0u;
    n_ps_copies = n_ps_shares = 0;
    if (!opposing_effects.empty()) CkAbort("user added opposing effects but did not flush them");
  }

  void applyOpposingEffect(const Particle& part, Vector3D<Real> accel, Real pressure) {
    auto& value = opposing_effects[part.partition_idx][part.key];
    value.first += accel;
    value.second += pressure;
  }

  template <typename Data>
  void applyAccumulatedOpposingEffects(PPHolder<Data> pp_holder) {
    for (auto && oe : opposing_effects) {
      std::vector<std::pair<Key, Particle::Effect>> effects (oe.second.begin(), oe.second.end());
      pp_holder.proxy[oe.first].applyOpposingEffects(effects);
    }
    opposing_effects.clear();
  }

  void countLeafInts(int n_ints) {
    n_part_ints += n_ints;
  }

  void countNodeInts(int n_ints) {
    n_node_ints += n_ints;
  }

  void countOpen(bool should_open) {
    should_open ? n_opens++ : n_closes++;
  }

  void countPartitionParticles(int n_parts) {
    n_partition_particles += n_parts;
  }

  void countTreePieceParticles(int n_parts) {
    n_treepiece_particles += n_parts;
  }

  void countCopiesAndShares(int n_c, int n_s) {
    n_ps_copies += n_c;
    n_ps_shares += n_s;
  }
};

#endif // PARATREET_THREADSTATEHOLDER_H_
