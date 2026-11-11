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
// One thread per target, sources visited in order: the sum order is fixed,
// so a backend that honours -ffp-contract=off gives the same bits as any
// other.
//
// Plain Kokkos: no Charm header, no HDF5.

#ifndef SUBHALO_UNBIND_KERNEL_H_
#define SUBHALO_UNBIND_KERNEL_H_

#include <Kokkos_Core.hpp>

#include <cstdint>

namespace unbind {

using Exec = Kokkos::DefaultExecutionSpace;
using Mem = Exec::memory_space;

// x, y, z (offsets from the group's reference point) and mass.
template <class Real>
using ParticleView = Kokkos::View<Real* [4], Kokkos::LayoutRight, Mem>;
using GroupView = Kokkos::View<int*, Mem>;
using OffsetView = Kokkos::View<int64_t*, Mem>;
using PotentialView = Kokkos::View<double*, Mem>;

template <class Real, class Acc, bool Wrap, bool Fast>
struct PotentialFunctor {
  ParticleView<Real> tgt, src;
  GroupView tgt_group;
  OffsetView src_off;
  PotentialView phi;
  Real h, h_inv, box, box_half;
  bool self;

  KOKKOS_INLINE_FUNCTION Real nearest(Real d) const {
    return d > box_half ? d - box : (d < -box_half ? d + box : d);
  }

  KOKKOS_INLINE_FUNCTION void operator()(const int64_t t) const {
    const Real x = tgt(t, 0), y = tgt(t, 1), z = tgt(t, 2);
    const int g = tgt_group(t);
    const int64_t end = src_off(g + 1);
    Acc acc = 0;
    for (int64_t s = src_off(g); s < end; s++) {
      Real dx = src(s, 0) - x, dy = src(s, 1) - y, dz = src(s, 2) - z;
      if (Wrap) {
        dx = nearest(dx);
        dy = nearest(dy);
        dz = nearest(dz);
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
    // The target is one of its own sources: at r = 0 the loop added
    // m * h_inv * (-2.8); take exactly that back.
    if (self) acc += Acc(tgt(t, 3) * h_inv * Real(2.8));
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

}  // namespace unbind

#endif  // SUBHALO_UNBIND_KERNEL_H_
