#ifndef EXAMPLE_MAIN_H
#define EXAMPLE_MAIN_H

#include "Main.decl.h"
#include "Paratreet.h"
#include "MinMaxData.h"

class ExMain: public paratreet::Main<MinMaxData> {
  virtual Real getTimestep(BoundingBox&, Real) override;
  virtual void preTraversalFn(ProxyPack<MinMaxData>&) override;
  virtual void traversalFn(BoundingBox&, ProxyPack<MinMaxData>&, int) override;
  virtual void postIterationFn(BoundingBox&, ProxyPack<MinMaxData>&, int) override;
  virtual void setDefaults(void) override;
  virtual void main(CkArgMsg*) override;
  virtual void run(void) override;
};

#endif
