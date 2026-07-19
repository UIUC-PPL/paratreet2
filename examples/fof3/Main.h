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
};

#endif
