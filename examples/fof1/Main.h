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

  // FoF phase-1 chares, created in main() (mainchare and driver.run both
  // execute on PE 0, so plain members are visible to traversalFn).
  CProxy_FoFPhase1<FragData> fof;
  CProxy_FoFPhase1Node<FragData> fof_node;

  // Serial O(n^2) reference results, stashed by the phase-1 validation and
  // reused by the fragment-histogram checks (exact on single-process runs).
  long serial_n_components = 0;
  long serial_max_size = 0;
};

#endif
