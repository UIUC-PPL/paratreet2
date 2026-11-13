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
  long sublabel;   // the label if the particle is in its group's sub-halo, else -1
};

// Unbinding inside FoF3 (design/subhalo-checks.md, "Step 2"): one
// particle of a kept group on its way to the process that unbinds the group.
struct FoFUnbindRec {
  long order;       // the particle's order (its id is order + 1)
  long label;       // the group's minimum particle order
  double x[3];      // comoving position, code units
  double v[3];      // velocity as read (Tipsy); times vel_factor = peculiar
  double m;
  // Part 2d: where the particle lives, (PE << kOriginIndexBits) | its index
  // in that PE's treepieces in order (FoFPhase1::unbindGather), and whether
  // its group spans processes (which decides its catalogue owner).
  unsigned long home;
  int touched;
  int pad;
};

// Part 2d: a particle's result on its way back to its PE.
struct FoFUnbindFlag {
  unsigned long index;  // in that PE's treepieces in order
  long flags;           // 1 = bound, 2 = the most bound particle
};

// Part 2d: a sub-halo's fields on their way to its catalogue row.
struct FoFUnbindRow {
  long label;
  long nbound;
  long most_bound;      // order of the most bound particle
  double bound_mass;
  double x[3], v[3];    // frame of the bound set; v in the input's velocity units
  long refined;         // 1 = the most bound particle came from the centre refinement
};

// FoF3 -U settings, in code units (G = 1); filled by parseUnbindSpec.
struct FoFUnbindSpec {
  double G = 1, a = 1, H = 0, eps = 0, box = 0;
  double bound_fraction = 0.995;
  double vel_factor = 1;    // Tipsy velocity -> peculiar velocity (= a)
  long cap = 10000, refine = 1000, chunk = 1024;
  // Part 2c: a group of more than `gather` particles is not gathered but
  // unbound where its particles are (unbind_spread.h); 0 = gather every
  // group. Only with a cap: without one every group is gathered.
  long gather = 1000000;
  int min_bound = 20;
  int kernel = 0;           // 0 f64, 1 f32 (summed in f64), 2 f32f32
  int rsqrt = 0;
};

// One record of FoF3 -U's per-process output <prefix>.<process>.ubd: a
// particle of a group that process unbound. 40 bytes, native byte order.
struct FoFUnbindOut {
  long order;
  long label;
  double energy;     // as last evaluated, code units
  double refined;    // the centre refinement's energy; NaN if not refined
  unsigned char bound, most_bound, refined_group, pad[5];
};

}  // namespace paratreet

// Plain data: marshalled as bytes, so a vector of them is one copy rather
// than a pup call per field of every element.
PUPbytes(paratreet::FoFCatPartial)
PUPbytes(paratreet::HostLabel)
PUPbytes(paratreet::FoFUnbindRec)
PUPbytes(paratreet::FoFUnbindSpec)
PUPbytes(paratreet::FoFUnbindFlag)
PUPbytes(paratreet::FoFUnbindRow)

#endif  // FOF_CATALOG_H_
