// unbind_kernel.h -- the pair-term kernel of the unbinding stage
// (design/subhalo-checks.md, step 1).
//
// For every target particle, the softened gravitational potential of the
// source particles of the target's group, without G:
//
//   phi(t) = sum over sources s of  -m_s / r            r >= h = 2.8 eps
//                                    m_s / h * wp(r/h)   r <  h
//
// with wp the Gadget spline, the formula HBT-HERONS uses
// (src/gravity_tree.cpp, EvaluatePotential, commit c728ee0). One pair term is
// one iteration of the inner loop.
//
// Targets and sources are separate lists, so one kernel serves the three
// uses unbinding has:
//   all pairs       sources = the targets themselves   (self = true)
//   subtraction     sources = the particles just removed
//   capped sample   sources = a sample of the group
// Many groups go in one launch: target t belongs to group tgt_group(t), whose
// sources are src_off(g) .. src_off(g + 1).
//
// Coordinates are offsets from a per-group reference point, which keeps
// single precision accurate for groups far from the origin. If every group
// is narrower than half the periodic box, pass box = 0; otherwise pass the
// box side and every pair separation takes its nearest image.
//
// Template parameters
//   Real   type of the pair arithmetic (float or double)
//   Acc    type the pair terms are summed in (float or double)
//   Fast   false: -m / sqrt(r2), HBT-HERONS's arithmetic
//          true:  -m * rsqrt(r2), one operation fewer per pair
//
// potential(): one thread per target, sources visited in order: the sum
// order is fixed, so a backend that honours -ffp-contract=off gives the same
// bits as any other.
//
// potentialSplit(): the same sum split into chunks of at most `chunk`
// consecutive sources. One thread per (target, chunk) sums its chunk in
// source order; a second kernel adds each target's chunk sums in chunk
// order. The order is fixed again, but it is not potential()'s, so the
// result differs from it by rounding. It is for few targets with many
// sources each, such as the centre refinement's candidates
// (design/subhalo-checks.md, "Centre refinement"), where one thread per
// target leaves the device idle and the largest group sets the time. It
// needs the targets of a group to be consecutive in tgt; splitPlan() finds
// the runs of consecutive targets that share a group.
//
// Plain Kokkos: no Charm header, no HDF5.

#ifndef SUBHALO_UNBIND_KERNEL_H_
#define SUBHALO_UNBIND_KERNEL_H_

#include <Kokkos_Core.hpp>

#include <cstdint>
#include <vector>

namespace unbind {

using Exec = Kokkos::DefaultExecutionSpace;
using Mem = Exec::memory_space;

// x, y, z (offsets from the group's reference point) and mass.
template <class Real>
using ParticleView = Kokkos::View<Real* [4], Kokkos::LayoutRight, Mem>;
using GroupView = Kokkos::View<int*, Mem>;
using OffsetView = Kokkos::View<int64_t*, Mem>;
using PotentialView = Kokkos::View<double*, Mem>;

// The nearest image of a separation d in a box of side box (box_half =
// box / 2), for |d| < box.
template <class Real>
KOKKOS_INLINE_FUNCTION Real nearestDev(const Real d, const Real box, const Real box_half) {
  return d > box_half ? d - box : (d < -box_half ? d + box : d);
}

// Body of every kernel: the sum over sources s0 .. s1 - 1, in that order, of
// the pair terms of target t, in Acc, without G.
template <class Real, class Acc, bool Wrap, bool Fast>
KOKKOS_INLINE_FUNCTION Acc pairSum(const ParticleView<Real>& tgt, const ParticleView<Real>& src,
                                   const int64_t t, const int64_t s0, const int64_t s1,
                                   const Real h, const Real h_inv, const Real box,
                                   const Real box_half) {
  const Real x = tgt(t, 0), y = tgt(t, 1), z = tgt(t, 2);
  Acc acc = 0;
  for (int64_t s = s0; s < s1; s++) {
    Real dx = src(s, 0) - x, dy = src(s, 1) - y, dz = src(s, 2) - z;
    if (Wrap) {
      dx = nearestDev(dx, box, box_half);
      dy = nearestDev(dy, box, box_half);
      dz = nearestDev(dz, box, box_half);
    }
    const Real r2 = dx * dx + dy * dy + dz * dz;
    const Real m = src(s, 3);
    if (r2 >= h * h) {
      if (Fast)
        acc -= Acc(m * Kokkos::rsqrt(r2));
      else
        acc -= Acc(m / Kokkos::sqrt(r2));
    } else {
      const Real u = Kokkos::sqrt(r2) * h_inv;
      Real wp;
      if (u < Real(0.5))
        wp = Real(-2.8) +
             u * u * (Real(5.333333333333) + u * u * (Real(6.4) * u - Real(9.6)));
      else
        wp = Real(-3.2) + Real(0.066666666667) / u +
             u * u * (Real(10.666666666667) +
                      u * (Real(-16.0) + u * (Real(9.6) - Real(2.133333333333) * u)));
      acc += Acc(m * h_inv * wp);
    }
  }
  return acc;
}

// The target is one of its own sources (self): at r = 0 the loop added
// m * h_inv * (-2.8); this takes exactly that back.
template <class Real, class Acc>
KOKKOS_INLINE_FUNCTION Acc selfTerm(const ParticleView<Real>& tgt, const int64_t t,
                                    const Real h_inv) {
  return Acc(tgt(t, 3) * h_inv * Real(2.8));
}

template <class Real, class Acc, bool Wrap, bool Fast>
struct PotentialFunctor {
  ParticleView<Real> tgt, src;
  GroupView tgt_group;
  OffsetView src_off;
  PotentialView phi;
  Real h, h_inv, box, box_half;
  bool self;

  KOKKOS_INLINE_FUNCTION void operator()(const int64_t t) const {
    const int g = tgt_group(t);
    Acc acc = pairSum<Real, Acc, Wrap, Fast>(tgt, src, t, src_off(g), src_off(g + 1), h,
                                             h_inv, box, box_half);
    if (self) acc += selfTerm<Real, Acc>(tgt, t, h_inv);
    phi(t) = double(acc);
  }
};

// ---- the split form ----------------------------------------------------------
// A block is a run of consecutive targets t0 .. t0 + nt - 1 of one group g.
// Its work items are (chunk c, target k), k fastest: items[b] + c * nt + k,
// so the threads of a wavefront read the same sources at the same time.
using BlockView = Kokkos::View<int64_t* [3], Kokkos::LayoutRight, Mem>;  // t0, nt, g

struct SplitPlan {
  BlockView block;
  OffsetView items;      // items[b] .. items[b + 1]: block b's work items
  OffsetView t0;         // block(b, 0), contiguous, for the search by target
  int64_t nitems = 0;
  int64_t chunk = 0;
};

// Built on the host from the targets' groups (grp, nt entries) and the
// groups' source offsets (off, one more entry than groups), as the kernels
// take them.
inline SplitPlan splitPlan(const int* grp, int64_t nt, const int64_t* off, int64_t chunk) {
  if (chunk <= 0) Kokkos::abort("splitPlan: chunk must be positive");
  std::vector<int64_t> blk, items{0}, first;
  for (int64_t t = 0; t < nt;) {
    int64_t e = t + 1;
    while (e < nt && grp[e] == grp[t]) e++;
    const int g = grp[t];
    const int64_t ns = off[g + 1] - off[g], nc = (ns + chunk - 1) / chunk;
    blk.insert(blk.end(), {t, e - t, (int64_t)g});
    first.push_back(t);
    items.push_back(items.back() + nc * (e - t));
    t = e;
  }
  SplitPlan S;
  const int64_t nb = (int64_t)items.size() - 1;
  S.block = BlockView(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_blocks"), nb);
  S.items = OffsetView(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_items"), nb + 1);
  S.t0 = OffsetView(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_t0"), nb);
  Kokkos::deep_copy(S.block, Kokkos::View<const int64_t* [3], Kokkos::LayoutRight,
                                          Kokkos::HostSpace,
                                          Kokkos::MemoryTraits<Kokkos::Unmanaged>>(blk.data(), nb));
  Kokkos::deep_copy(S.items, Kokkos::View<const int64_t*, Kokkos::HostSpace,
                                          Kokkos::MemoryTraits<Kokkos::Unmanaged>>(items.data(),
                                                                                   nb + 1));
  Kokkos::deep_copy(S.t0, Kokkos::View<const int64_t*, Kokkos::HostSpace,
                                       Kokkos::MemoryTraits<Kokkos::Unmanaged>>(first.data(), nb));
  S.nitems = items.back();
  S.chunk = chunk;
  return S;
}

// The last b with v(b) <= x, for v increasing and v(0) <= x.
KOKKOS_INLINE_FUNCTION int64_t lastAtMost(const OffsetView& v, const int64_t n, const int64_t x) {
  int64_t lo = 0, hi = n;   // v(lo) <= x; the answer is in lo .. hi - 1
  while (hi - lo > 1) {
    const int64_t mid = lo + (hi - lo) / 2;
    if (v(mid) <= x) lo = mid; else hi = mid;
  }
  return lo;
}

template <class Real, class Acc, bool Wrap, bool Fast>
struct ChunkFunctor {   // one thread per work item: one chunk of one target
  ParticleView<Real> tgt, src;
  OffsetView src_off;
  BlockView block;
  OffsetView items;
  Kokkos::View<Acc*, Mem> part;
  int64_t nblocks, chunk;
  Real h, h_inv, box, box_half;

  KOKKOS_INLINE_FUNCTION void operator()(const int64_t i) const {
    const int64_t b = lastAtMost(items, nblocks, i), nt = block(b, 1), j = i - items(b);
    const int64_t c = j / nt, t = block(b, 0) + j % nt, g = block(b, 2);
    const int64_t s0 = src_off(g) + c * chunk;
    const int64_t s1 = s0 + chunk < src_off(g + 1) ? s0 + chunk : src_off(g + 1);
    part(i) = pairSum<Real, Acc, Wrap, Fast>(tgt, src, t, s0, s1, h, h_inv, box, box_half);
  }
};

template <class Real, class Acc>
struct ChunkSumFunctor {   // one thread per target: its chunk sums in chunk order
  ParticleView<Real> tgt;
  BlockView block;
  OffsetView items, t0;
  Kokkos::View<Acc*, Mem> part;
  PotentialView phi;
  int64_t nblocks;
  Real h_inv;
  bool self;

  KOKKOS_INLINE_FUNCTION void operator()(const int64_t t) const {
    const int64_t b = lastAtMost(t0, nblocks, t), nt = block(b, 1), k = t - block(b, 0);
    const int64_t nc = (items(b + 1) - items(b)) / nt;
    Acc acc = 0;
    for (int64_t c = 0; c < nc; c++) acc += part(items(b) + c * nt + k);
    if (self) acc += selfTerm<Real, Acc>(tgt, t, h_inv);
    phi(t) = double(acc);
  }
};

// Runs the kernel and returns its wall time in seconds (launch to fence).
// phi must hold at least tgt.extent(0) entries. eps > 0.
template <class Real, class Acc, bool Fast = false>
double potential(const ParticleView<Real>& tgt, const GroupView& tgt_group,
                 const ParticleView<Real>& src, const OffsetView& src_off,
                 const PotentialView& phi, double eps, double box, bool self) {
  const int64_t nt = (int64_t)tgt.extent(0);
  const Real h = Real(2.8 * eps);
  const Real h_inv = Real(1) / h;
  const Kokkos::RangePolicy<Exec, Kokkos::IndexType<int64_t>> range(0, nt);
  Kokkos::fence();
  Kokkos::Timer timer;
  if (box > 0) {
    Kokkos::parallel_for(
        "unbind::potential_wrap", range,
        PotentialFunctor<Real, Acc, true, Fast>{tgt, src, tgt_group, src_off, phi, h,
                                               h_inv, Real(box), Real(0.5 * box),
                                               self});
  } else {
    Kokkos::parallel_for(
        "unbind::potential", range,
        PotentialFunctor<Real, Acc, false, Fast>{tgt, src, tgt_group, src_off, phi, h,
                                                h_inv, Real(0), Real(0), self});
  }
  Kokkos::fence();
  return timer.seconds();
}

// The split form of potential(): the same arguments, with the plan made by
// splitPlan() from the same tgt_group and src_off in place of tgt_group. Runs
// the two kernels and returns their wall time in seconds (launch to fence).
template <class Real, class Acc, bool Fast = false>
double potentialSplit(const ParticleView<Real>& tgt, const SplitPlan& plan,
                      const ParticleView<Real>& src, const OffsetView& src_off,
                      const PotentialView& phi, double eps, double box, bool self) {
  const int64_t nt = (int64_t)tgt.extent(0), nb = (int64_t)plan.block.extent(0);
  const Real h = Real(2.8 * eps);
  const Real h_inv = Real(1) / h;
  Kokkos::View<Acc*, Mem> part(Kokkos::view_alloc(Kokkos::WithoutInitializing, "unbind_part"),
                               plan.nitems);
  using Range = Kokkos::RangePolicy<Exec, Kokkos::IndexType<int64_t>>;
  Kokkos::fence();
  Kokkos::Timer timer;
  if (box > 0)
    Kokkos::parallel_for("unbind::chunks_wrap", Range(0, plan.nitems),
                         ChunkFunctor<Real, Acc, true, Fast>{
                             tgt, src, src_off, plan.block, plan.items, part, nb, plan.chunk,
                             h, h_inv, Real(box), Real(0.5 * box)});
  else
    Kokkos::parallel_for("unbind::chunks", Range(0, plan.nitems),
                         ChunkFunctor<Real, Acc, false, Fast>{
                             tgt, src, src_off, plan.block, plan.items, part, nb, plan.chunk,
                             h, h_inv, Real(0), Real(0)});
  Kokkos::parallel_for("unbind::chunk_sums", Range(0, nt),
                       ChunkSumFunctor<Real, Acc>{tgt, plan.block, plan.items, plan.t0, part, phi,
                                                  nb, h_inv, self});
  Kokkos::fence();
  return timer.seconds();
}

}  // namespace unbind

#endif  // SUBHALO_UNBIND_KERNEL_H_
