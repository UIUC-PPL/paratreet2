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
  double fof_b_factor = 0.2;
  enum class CheckMode { Auto, Full, Stats };
  CheckMode check_mode = CheckMode::Auto;
  enum class UF2Mode { Dist, Serial };
  UF2Mode uf2_mode = UF2Mode::Dist;
  // Auto-mode gate: full verification gathers ~24 B/particle to PE 0 and
  // runs the serial grid reference there; above this N, auto falls back to
  // stats mode (force with -c full, memory permitting).
  static constexpr int kAutoFullMaxN = 20000000;
};

#endif
