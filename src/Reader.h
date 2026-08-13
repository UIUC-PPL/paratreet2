#ifndef PARATREET_READER_H_
#define PARATREET_READER_H_

#include "paratreet.decl.h"
#include "common.h"
#include "ParticleMsg.h"
#include "BoundingBox.h"
#include "Splitter.h"
#include "Utility.h"
#include "TreeSpec.h"
#include "Modularization.h"

extern int n_readers;
extern CProxy_TreeSpec treespec;

class Reader : public CBase_Reader {
  std::vector<Particle> particles;
  std::vector<ParticleMsg*> particle_messages;
  int particle_index;

  // ---- Flush windowing -----------------------------------------------------
  // Reader::flush used to post every ParticleMsg in one unbroken loop and
  // return, with no window, no acks and no backpressure. At 2B particles that
  // is ~1M particles per Reader branch handed to the runtime as fast as it will
  // take them. When the network is draining slowly (Anvil's degraded messaging
  // mode costs ~100x for minutes at a time) the sender outruns the wire, the
  // untransmitted copies accumulate, and one rank is eventually OOM-killed --
  // which is what produced the "IBV poll_comp assert" cluster on 2026-08-11:
  // the assert fires on the OOM victim's PEERS when its queue pairs flush.
  //
  // The window caps unacknowledged particles per Reader branch. TreePiece::
  // receive acks; when the outstanding count hits the window the (threaded)
  // flush suspends until acks bring it back under the low-water mark.
  //
  // PARATREET_FLUSH_WINDOW = particles in flight per Reader (0 disables
  // windowing entirely and restores the original unbounded behaviour).
  //
  // The send list is built once (recording destinations, not copying
  // particles), then drained by pumpFlush: post while under the window, stop,
  // and let acks pull the rest through. `particles` stays alive until the last
  // message is constructed, exactly as in the original code.
  struct FlushChunk { int dest; int offset; int count; };
  std::vector<FlushChunk> flush_plan;
  size_t flush_next = 0;
  long flush_outstanding = 0;

  public:
  static long flushWindow() {
    static long w = [] {
      const char* e = std::getenv("PARATREET_FLUSH_WINDOW");
      // 128K particles ~ 24 MB in flight per branch at the current Particle
      // size; x15 PEs that bounds a process at a few hundred MB, against the
      // ~1M particles (~200 MB) a branch would otherwise post in one go.
      return e ? std::atol(e) : 131072L;
    }();
    return w;
  }

  private:
  // Post queued chunks while the window allows. Posting is checked BEFORE each
  // message, so a single chunk larger than the window still goes out (the
  // window is then briefly exceeded by that one chunk) and the drain cannot
  // deadlock. Frees `particles` as soon as the last message is constructed.
  template <typename Data>
  void pumpFlush(CProxy_TreePiece<Data> treepieces) {
    const long window = flushWindow();
    while (flush_next < flush_plan.size() && flush_outstanding < window) {
      const FlushChunk& c = flush_plan[flush_next++];
      ParticleMsg* msg = new (c.count) ParticleMsg(&particles[c.offset], c.count);
      msg->sender = thisIndex;
      flush_outstanding += c.count;
      treepieces[c.dest].receive(msg);
    }
    if (flush_next == flush_plan.size() && !flush_plan.empty()) {
      // Every message has been constructed; the particle data is copied into
      // them, so the source can go now rather than at the end of the flush.
      flush_plan.clear();
      flush_plan.shrink_to_fit();
      particles.clear();
      particles.shrink_to_fit();
      particle_index = 0;
    }
  }

  static constexpr const Real gasConstant = 1.0;
  static constexpr const Real gammam1 = 5.0/3.0 - 1;
  static constexpr const Real meanMolWeight = 1.0;

  public:
    Real start_time = 0;
    BoundingBox universe;
    // std::vector<Splitter> splitters;
    // std::vector<Key> SFCsplitters;
    Reader();

    // Loading particles and assigning keys
    void load(std::string, const CkCallback&);
    void setSoft(const double dSoft, const CkCallback&);
    void computeUniverseBoundingBox(const CkCallback& cb);
    void assignKeys(BoundingBox, const CkCallback&);

    void countAssignments(const std::vector<GenericSplitter>&, bool is_treepiece, const CkCallback& cb, bool weight_by_partition);
    void doSplit(const std::vector<GenericSplitter>&, bool is_treepiece, const CkCallback&);

    // SFC decomposition
    void getAllSfcKeys(const CkCallback& cb);
    void getAllPositions(const CkCallback& cb);
    //void countSfc(const std::vector<Key>&, const CkCallback&);
    void pickSamples(const int, const CkCallback&);
    void prepMessages(const std::vector<Key>&, const CkCallback&);
    void redistribute();
    void receive(ParticleMsg*);
    template <typename Data>
    void flushAck(int, CProxy_TreePiece<Data>);
    void localSort(const CkCallback&);
    void checkSort(const Key, const CkCallback&);
    template <typename Data>
    void request(CProxy_TreePiece<Data>, int, int);

    // Sending particles to home Partitions and TreePieces
    template <typename Data>
    void flush(int, CProxy_TreePiece<Data>);
    template <typename Data>
    void assignPartitions(int, CProxy_Partition<Data>);
};

template <typename Data>
void Reader::request(CProxy_TreePiece<Data> tp_proxy, int index, int num_to_give) {
  int n_particles = universe.n_particles;
  ParticleMsg* msg = new (num_to_give) ParticleMsg(&particles[0], num_to_give);
  for (int i = 0; i < n_particles - num_to_give; i++) {
    particles[i] = particles[num_to_give + i];
  } // maybe a function can just do this for me?
  particles.resize(n_particles - num_to_give);
  n_particles -= num_to_give;
  // if (n_particles) SFCsplitters.push_back(particles[0].key);
  tp_proxy[index].receive(msg);
}

template <typename Data>
void Reader::flush(int n_treepieces,
                   CProxy_TreePiece<Data> treepieces) {
  const long window = flushWindow();

  if (window <= 0) {
    // Legacy path: post everything in one unbounded burst, no acks.
    auto sendParticles = [&](int dest, int n_particles, Particle* parts) {
      ParticleMsg* msg = new (n_particles) ParticleMsg(parts, n_particles);
      treepieces[dest].receive(msg);
    };
    int flush_count = treespec.ckLocalBranch()->getTreePieceDecomposition()->flush(particles, sendParticles);
    if (flush_count != particles.size()) {
      CkPrintf("Reader %d failure: flushed %d out of %zu particles\n", thisIndex,
          flush_count, particles.size());
      CkAbort("Reader failure -- see stdout");
    }
    particles.clear();
    particle_index = 0;
    return;
  }

  // Windowed path. Record the plan without copying anything: the decomposition
  // hands us (dest, ptr, count) and we keep the OFFSET into `particles`, so the
  // message can be built later, when the window allows.
  //
  // Deferring is only sound when that pointer aims into `particles` itself.
  // Sfc and Oct sort in place and do (std::sort does not reallocate, so `base`
  // stays valid across the call). BinaryDecomposition instead stages into a
  // local vector and clears `particles` as it goes, so its buffers would be
  // gone by the time we posted; those chunks are sent immediately and simply
  // are not windowed. The range test below is what distinguishes them.
  flush_plan.clear();
  flush_next = 0;
  flush_outstanding = 0;
  Particle* const base = particles.data();
  const size_t base_n = particles.size();
  auto planParticles = [&](int dest, int n_particles, Particle* parts) {
    if (parts >= base && parts + n_particles <= base + base_n) {
      flush_plan.push_back(FlushChunk{dest, (int)(parts - base), n_particles});
    } else {
      ParticleMsg* msg = new (n_particles) ParticleMsg(parts, n_particles);
      treepieces[dest].receive(msg);   // sender stays -1: unwindowed, no ack
    }
  };

  int flush_count = treespec.ckLocalBranch()->getTreePieceDecomposition()->flush(particles, planParticles);
  if (flush_count != particles.size()) {
    CkPrintf("Reader %d failure: flushed %d out of %zu particles\n", thisIndex,
        flush_count, particles.size());
    CkAbort("Reader failure -- see stdout");
  }
  if (flush_plan.empty()) {            // nothing deferred; release immediately
    particles.clear();
    particle_index = 0;
    return;
  }

  pumpFlush(treepieces);
}

// Ack from a TreePiece that has copied `n` particles out of one of our
// messages. Releases that much window and posts more. Runs on the Reader
// branch's own PE; entry methods on a PE do not preempt each other, so the
// counter needs no synchronisation.
//
// Quiescence safety: while any chunk is unposted there is always either a
// message in flight or an ack pending, so the CkStartQD that follows
// readers.flush() in Driver cannot fire mid-flush.
template <typename Data>
void Reader::flushAck(int n, CProxy_TreePiece<Data> treepieces) {
  flush_outstanding -= n;
  pumpFlush(treepieces);
}

template <typename Data>
void Reader::assignPartitions(int n_partitions, CProxy_Partition<Data> partitions)
{
  auto sendParticles =
    [&](int dest, int n_particles, Particle* particles) {
      for (int i = 0; i < n_particles; ++i)
        particles[i].partition_idx = dest;
    };
  int flush_count = treespec.ckLocalBranch()->getPartitionDecomposition()->flush(particles, sendParticles);
  if (flush_count != particles.size()) {
    CkPrintf("Reader %d failure: flushed %d out of %zu particles\n", thisIndex,
        flush_count, particles.size());
    CkAbort("Reader failure -- see stdout");
  }
}


#endif // PARATREET_READER_H_
