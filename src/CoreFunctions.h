#ifndef PARATREET_CORE_FUNCTIONS_H_
#define PARATREET_CORE_FUNCTIONS_H_

#include "common.h"

class BoundingBox;
struct Particle;

template<typename T>
class Node;

template<typename T>
class Partition;

template<typename T>
class ProxyPack;

template<typename T>
class SpatialNode;

namespace paratreet {
    // Parked-opaque encoding for the TreeCache park/install contract.
    // CALLER-side: the cache stores and returns these values without ever
    // interpreting them. Layout: lane (worker thread / processor rank
    // within the process) in the top byte for install-side routing, then
    // the traversal index, then the walking chare element's array index.
    inline uint64_t makeParkedOpaque(int lane, size_t trav_idx, int elem_idx) {
      return ((uint64_t)(unsigned)lane << 56) |
             (((uint64_t)trav_idx & 0xffffffull) << 32) |
             (uint64_t)(unsigned)elem_idx;
    }
    inline int parkedLane(uint64_t o) { return (int)(o >> 56); }
    inline int parkedTravIdx(uint64_t o) { return (int)((o >> 32) & 0xffffffull); }
    inline int parkedElemIdx(uint64_t o) { return (int)(o & 0xffffffffull); }

    inline Real getTimestep(BoundingBox& box, Real real);

    template<typename T>
    inline void preTraversalFn(ProxyPack<T>& pack);

    template<typename T>
    inline void traversalFn(BoundingBox& box, ProxyPack<T>& pack, int iter);

    template<typename T>
    inline void postIterationFn(BoundingBox& box, ProxyPack<T>& pack, int iter);

    template<typename T>
    class PerLeafAble: public PUP::able {
      public:  
        PerLeafAble(void) = default;
        PerLeafAble(CkMigrateMessage *m): PUP::able(m) {}

        virtual void pup(PUP::er &p) override { PUP::able::pup(p); }

        virtual void operator()(SpatialNode<T>& node, Partition<T>* partition) = 0;
    };

    // Subtree-level analogue of PerLeafAble, for consumers that need each
    // Subtree element's local tree root and contiguous particle block rather
    // than individual leaves (delivered by Subtree::callPerSubtreeFn). The
    // block pointer is stable from the end of tree build until the next
    // rebuild/reset; consumers that retain it must finish inside that window.
    template<typename T>
    class PerSubtreeAble: public PUP::able {
      public:
        PerSubtreeAble(void) = default;
        PerSubtreeAble(CkMigrateMessage *m): PUP::able(m) {}

        virtual void pup(PUP::er &p) override { PUP::able::pup(p); }

        virtual void operator()(Node<T>* local_root, Particle* particles, int n_particles) = 0;
    };
}

#endif
