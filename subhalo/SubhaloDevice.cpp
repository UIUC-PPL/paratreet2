// SubhaloDevice.cpp -- the device side of unbinding inside FoF3
// (SubhaloDevice.h; design/subhalo-checks.md, "Step 2"). Built by hipcc
// into libsubhalodevice.a (subhalo/Makefile); no Charm header.
//
// The packing is unbind::makeGroups and the unbinding unbind::unbindGroups,
// the same calls the standalone `unbind` makes, so a group packed here from
// the same particles in the same order gives the same bits.

#include "SubhaloDevice.h"

#include "unbind_groups.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>

namespace subhalodev {

bool available() {
  return !std::is_same<unbind::Exec, Kokkos::DefaultHostExecutionSpace>::value;
}

const char* backendName() { return unbind::Exec::name(); }

namespace {

template <class Real, class Acc>
unbind::Result run(const unbind::Groups& G, const unbind::Params& P, bool rsqrt) {
  return rsqrt ? unbind::unbindGroups<Real, Acc, true>(G, P)
               : unbind::unbindGroups<Real, Acc, false>(G, P);
}

[[noreturn]] void fail(const char* what) {
  fprintf(stderr, "subhalodev::unbind: %s\n", what);
  std::abort();
}

}  // namespace

void unbind(const Input& in, const Params& p, Output& o) {
  if (!Kokkos::is_initialized()) fail("Kokkos is not initialized (bind the device first)");
  if (!(p.eps > 0)) fail("the softening must be positive");
  if (p.kernel < 0 || p.kernel > 2) fail("kernel must be 0 (f64), 1 (f32) or 2 (f32f32)");
  Kokkos::Timer wall;
  const int64_t n = in.n;
  for (int64_t i = 1; i < n; i++)
    if (in.label[i] < in.label[i - 1] ||
        (in.label[i] == in.label[i - 1] && in.id[i] <= in.id[i - 1]))
      fail("the input is not sorted by (label, id)");

  o = Output();
  o.bound.assign((size_t)n, 0);
  o.energy.assign((size_t)n, 0.0);
  o.refined.assign((size_t)n, std::numeric_limits<double>::quiet_NaN());
  for (int64_t b = 0; b < n;) {
    int64_t e = b + 1;
    while (e < n && in.label[e] == in.label[b]) e++;
    o.label.push_back(in.label[b]);
    o.first.push_back(b);
    o.count.push_back(e - b);
    b = e;
  }
  const size_t ng_in = o.label.size();
  o.nbound.assign(ng_in, 0);
  o.centre.assign(ng_in, -1);
  o.passes.assign(ng_in, 0);
  o.refined_group.assign(ng_in, 0);
  o.stale_frame.assign(ng_in, 0);
  if (n == 0) return;

  // makeGroups keeps each group's particles in input order, which is id
  // order, relative to its first (lowest-id) particle; null id -1 never
  // matches a label.
  unbind::Groups G = unbind::makeGroups(n, in.pos, in.vel, in.mass, in.label, (int64_t)-1,
                                        p.box, (int64_t)p.min_bound, in.id);
  unbind::Params P;
  P.G = p.G;
  P.a = p.a;
  P.H = p.H;
  P.eps = p.eps;
  P.box = p.box;
  P.bound_fraction = p.bound_fraction;
  P.min_bound = p.min_bound;
  P.cap = p.cap;
  P.refine = p.cap > 0 ? p.refine : 0;
  P.refine_chunk = p.refine_chunk;
  unbind::Result R;
  if (G.ng() > 0) {
    switch (p.kernel) {
      case 0: R = run<double, double>(G, P, p.rsqrt); break;
      case 1: R = run<float, double>(G, P, p.rsqrt); break;
      default: R = run<float, float>(G, P, p.rsqrt); break;
    }
  }

  // Back to input order. G's groups are the input's groups of at least
  // min_bound particles, in the same (label) order.
  size_t gi = 0;
  for (int g = 0; g < G.ng(); g++) {
    while (gi < ng_in && o.label[gi] != G.id[g]) gi++;
    if (gi == ng_in) fail("a packed group is not in the input");
    o.nbound[gi] = R.nbound[g];
    o.passes[gi] = R.passes[g];
    o.stale_frame[gi] = R.stale_frame[g];
    int64_t best = -1;
    for (int64_t k = G.off[g]; k < G.off[g + 1]; k++) {
      const int64_t i = G.particle[k];
      o.bound[i] = R.bound[k];
      o.energy[i] = R.energy[k];
      if (!R.refined.empty()) o.refined[i] = R.refined[k];
      // Without the refinement: the bound particle of lowest energy, ties
      // by id (what unbindGroups reports for a group it does not refine).
      if (R.bound[k] && (best < 0 || R.energy[k] < R.energy[best] ||
                         (R.energy[k] == R.energy[best] && G.pid[k] < G.pid[best])))
        best = k;
    }
    if (!R.centre.empty()) {
      o.centre[gi] = R.centre[g] >= 0 ? G.particle[R.centre[g]] : -1;
      o.refined_group[gi] = R.refined_group[g];
    } else {
      o.centre[gi] = best >= 0 && R.nbound[g] > 0 ? G.particle[best] : -1;
    }
    gi++;
  }
  o.wide_groups = G.wide_groups;
  o.sampled_groups = R.stats.sampled_groups;
  o.refined_groups = R.stats.refined_groups;
  o.pairs_first = (double)R.stats.pairs_first;
  o.pairs_later = (double)(R.stats.pairs_full_later + R.stats.pairs_subtract +
                           R.stats.pairs_sample_update);
  o.pairs_refine = (double)R.stats.pairs_refine;
  o.kernel_s_first = R.stats.kernel_s_first;
  o.kernel_s_later = R.stats.kernel_s_later;
  o.kernel_s_refine = R.stats.kernel_s_refine;
  o.host_s = R.stats.host_s;
  o.rounds = R.stats.passes;
  o.wall_s = wall.seconds();
}

}  // namespace subhalodev
