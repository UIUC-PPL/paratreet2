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
