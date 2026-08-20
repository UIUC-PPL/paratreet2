#ifndef PARATREET_CORE_FUNCTIONS_H_
#define PARATREET_CORE_FUNCTIONS_H_

#include "common.h"

class BoundingBox;
struct Particle;

template<typename T>
class Node;

namespace paratreet { struct DNode; }  // src/DeviceTree.h; pointer only here

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

    // TreePiece-level analogue of PerLeafAble, for consumers that need each
    // TreePiece element's local tree root and contiguous particle block rather
    // than individual leaves (delivered by TreePiece::callPerTreePieceFn). The
    // block pointer is stable from the end of tree build until the next
    // rebuild/reset; consumers that retain it must finish inside that window.
    template<typename T>
    class PerTreePieceAble: public PUP::able {
      public:
        PerTreePieceAble(void) = default;
        PerTreePieceAble(CkMigrateMessage *m): PUP::able(m) {}

        virtual void pup(PUP::er &p) override { PUP::able::pup(p); }

        virtual void operator()(Node<T>* local_root, Particle* particles, int n_particles) = 0;

        // Flat-tree-aware form (src/DeviceTree.h). Additive: the default
        // forwards to the 3-arg operator above, so every existing
        // PerTreePieceAble keeps compiling and behaving identically and
        // only clients that want the flat tree override this one.
        virtual void operator()(Node<T>* local_root, Particle* particles,
                                int n_particles, const paratreet::DNode* dnodes,
                                int n_dnodes) {
          (void)dnodes; (void)n_dnodes;
          (*this)(local_root, particles, n_particles);
        }
    };
}

#endif
