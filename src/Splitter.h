#ifndef PARATREET_SPLITTER_H_
#define PARATREET_SPLITTER_H_

#include "common.h"

struct Splitter {
  Key from;
  Key to;
  Key tp_key;
  // 64-bit: this is a PARTICLE COUNT, and on the first pass of
  // OctDecomposition::findSplitters the candidate range is the whole
  // snapshot. int truncated 24461180928 to -1308622848 and the range was
  // then never subdivided (job 5332118, 2026-08-23).
  long n_particles;

  Splitter() {}
  Splitter(Key from_, Key to_, Key tp_key_, long n_particles_) :
    from(from_), to(to_), tp_key(tp_key_), n_particles(n_particles_) {}

  void pup(PUP::er &p) {
    p | from;
    p | to;
    p | tp_key;
    p | n_particles;
  }

  bool operator<=(const Splitter& other) const {
    return from <= other.from;
  }

  bool operator>(const Splitter& other) const {
    return !(*this <= other);
  }

  bool operator>=(const Splitter& other) const {
    return from >= other.from;
  }

  bool operator<(const Splitter& other) const {
    return !(*this >= other);
  }
};

struct GenericSplitter {
  Key 	start_key = Key(0);
  Key 	end_key = (~Key(0));
  Key 	midKey() const {return start_key + (end_key - start_key) / 2;}
  long 	goal_rank; // rank in [0, N): 64-bit
  bool 	pending = true;
  int 	dim = -1;
  Real  start_float = 0;
  Real  end_float   = 0;
  Real  midFloat() const {return start_float + (end_float - start_float) / 2;}
  void pup(PUP::er &p) {
    p | start_key;
    p | end_key;
    p | goal_rank;
    p | pending;
    p | dim;
    p | start_float;
    p | end_float;
  }
};

#endif // PARATREET_SPLITTER_H_
