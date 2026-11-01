#ifndef FOF_CATALOG_H_
#define FOF_CATALOG_H_

// Records the FoF module sends in bulk (included by fof.ci): the halo
// catalog's partial record (design/plugin-interface.md stage D), one
// group's sums over the particles one PE holds, which owners merge; and a
// hosted run's per-particle label.

#include <cstdint>
#include <pup.h>
#include <pup_stl.h>

namespace paratreet {

struct FoFCatPartial {
  long label = -1;      // the group's minimum particle ID
  long n = 0;           // particles
  double m = 0;         // mass
  double mx[3] = {0, 0, 0};  // sum of m * (x - ref), minimum image
  double mv[3] = {0, 0, 0};  // sum of m * v
  double lo[3] = {0, 0, 0};  // extent of x - ref, minimum image
  double hi[3] = {0, 0, 0};
  double ref[3] = {0, 0, 0}; // one member's position
};

// A hosted run's result for one staged particle (FoFPhase1::returnHostLabels).
struct HostLabel {
  uint64_t index;  // staging index on the receiving PE
  long label;
};

}  // namespace paratreet

// Plain data: marshalled as bytes, so a vector of them is one copy rather
// than a pup call per field of every element.
PUPbytes(paratreet::FoFCatPartial)
PUPbytes(paratreet::HostLabel)

#endif  // FOF_CATALOG_H_
