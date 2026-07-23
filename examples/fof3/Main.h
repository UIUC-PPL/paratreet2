#ifndef EXAMPLE_MAIN_H
#define EXAMPLE_MAIN_H

#include "Main.decl.h"
#include "Paratreet.h"
#include "FoFData.h"

class ExMain: public paratreet::Main<FragData> {
  virtual Real getTimestep(BoundingBox&, Real) override;
  virtual void preTraversalFn(ProxyPack<FragData>&) override;
  virtual void traversalFn(BoundingBox&, ProxyPack<FragData>&, int) override;
  virtual void postIterationFn(BoundingBox&, ProxyPack<FragData>&, int) override;
  virtual void setDefaults(void) override;
  virtual void main(CkArgMsg*) override;
  virtual void run(void) override;

  // FoF phase-1/phase-3 chares, created in main() (mainchare and driver.run
  // both execute on PE 0, so plain members are visible to preTraversalFn
  // and traversalFn).
  CProxy_FoFPhase1<FragData> fof;
  CProxy_FoFPhase1Node<FragData> fof_node;
  // Linking length, computed in preTraversalFn (phase 1 now runs there,
  // before loadCache) and reused by phase 3 in traversalFn.
  double fof_b = 0.0;

  // App-specific flags (parsed by getopt in main(); see README):
  //   -b <factor>  linking-length factor: b = factor * (V/N)^(1/3)
  //   -c <mode>    correctness-check mode: full | stats | auto
  //   -u <mode>    UF_2 implementation: dist (default) | serial (see
  //                design/step4.md; serial kept for A/B against the
  //                gather-to-one v1/3a path)
  //   -m <int>     minimum component size (particles) for REPORTING: a
  //                component "survives" if its size >= m. Default 0 = report
  //                everything (byte-identical to pre-step-5 output). When m>0
  //                an extra FOF3STAT surviving line is printed alongside the
  //                unchanged (unpruned) components line (design/step5-pruning.md).
  //                This is a reporting FILTER only; it never relabels particles
  //                or changes the validated partition/equality checks.
  double fof_b_factor = 0.2;
  enum class CheckMode { Auto, Full, Stats };
  CheckMode check_mode = CheckMode::Auto;
  enum class UF2Mode { Dist, Serial };
  UF2Mode uf2_mode = UF2Mode::Dist;
  // Phase-3 walk mechanism (-w; design/dual-tree.md): transposed (default,
  // the validated startDown source-tree-vs-flat-target-leaves walk) or dual
  // (Subtree::startDual symmetric dual-tree walk -- the design/step3.md 6f
  // lever; prototype, -u dist only).
  enum class WalkMode { Transposed, Dual };
  WalkMode walk_mode = WalkMode::Transposed;
  // Min component size for reporting (-m); 0 = report everything (default).
  int fof_min_component_size = 0;
  // Periodic boundary conditions (-P <L>; design/pbc.md): cubic box period
  // applied on all three axes. Default 0 = open boundaries (PBC off, exact
  // current behavior; the periodic branch is a no-op). Threaded into phase 1
  // (runFoFPhase1) and phase 3 (the FoFEdgeVisitor) as Vector3D<Real>(L,L,L).
  Real pbc_period = 0.0;
  // Auto-mode gate: full verification gathers ~24 B/particle to PE 0 and
  // runs the serial grid reference there; above this N, auto falls back to
  // stats mode (force with -c full, memory permitting).
  static constexpr int kAutoFullMaxN = 20000000;
};

#endif
