#include "Main.h"

#include "FoFPhase3.h"
#include "FragCheckVisitor.h"

PARATREET_REGISTER_MAIN(ExMain);

/* readonly */ int peanoKey;

  static void initialize() {
    BoundingBox::registerReducer();
  }

  // Defaults are applied before the framework parses the command line
  // (framework-registered options: -x -f -v -n -p -l -d -t -i -s ...;
  // see paratreet::Configuration::register_fields).
  void ExMain::setDefaults(void) {
    conf.min_n_subtrees = CkNumPes() * 8;
    conf.min_n_partitions = CkNumPes() * 8;
    conf.max_particles_per_leaf = 12;
    // Oct decomposition/tree is the FoF configuration (design/phase1.md);
    // run with -d oct. Phase 3 requires the matching decompositions
    // (runFoFPhase3 enforces it).
    conf.decomp_type = paratreet::DecompType::eOct;
    conf.tree_type = paratreet::TreeType::eOct;
    conf.num_iterations = 1;
    conf.num_share_nodes = 0;
    conf.cache_share_depth = 3;
    conf.request_pause_interval = 20;
    conf.iter_pause_interval = 1000;
  }

  void ExMain::main(CkArgMsg* m) {
    peanoKey = 3;
    delete m;

    CkPrintf("\n[PARATREET FOF PHASE 3]\n");
    if (conf.input_file.empty()) CkAbort("Input file unspecified");
    CkPrintf("Input file: %s\n", conf.input_file.c_str());
    CkPrintf("Decomposition type: %s\n", paratreet::asString(conf.decomp_type).c_str());
    CkPrintf("Tree type: %s\n", paratreet::asString(conf.tree_type).c_str());
    CkPrintf("Minimum number of subtrees: %d\n", conf.min_n_subtrees);
    CkPrintf("Minimum number of partitions: %d\n", conf.min_n_partitions);
    CkPrintf("Maximum number of particles per leaf: %d\n", conf.max_particles_per_leaf);

    // Unit checks for paratreet::maxdist2 (the phase-3a positive
    // certificate). The certificate rarely fires in vivo — DFS leaf
    // witnesses usually mark (g,f) SEEN before any internal pair is fully
    // within b — so verify the closed form directly on the hand cases from
    // its comment (one axis exercised at a time, plus a mixed 3-D case).
    {
      auto box = [](double lx, double ly, double lz,
                    double gx, double gy, double gz) {
        OrientedBox<Real> b;
        b.lesser_corner = Vector3D<Real>(lx, ly, lz);
        b.greater_corner = Vector3D<Real>(gx, gy, gz);
        return b;
      };
      auto near = [](Real v, double want) { return std::fabs(v - want) < 1e-9; };
      // Disjoint on x: A=[0,1], B=[2,3] -> 3^2 (farthest points 0 and 3).
      CkEnforce(near(paratreet::maxdist2(box(0,0,0, 1,0,0), box(2,0,0, 3,0,0)), 9.0));
      // Identical [0,1]^3: opposite corners -> 1+1+1.
      CkEnforce(near(paratreet::maxdist2(box(0,0,0, 1,1,1), box(0,0,0, 1,1,1)), 3.0));
      // Nested on y: A=[0,4], B=[1,2] -> 3^2 (farthest points 4 and 1).
      CkEnforce(near(paratreet::maxdist2(box(0,0,0, 0,4,0), box(0,1,0, 0,2,0)), 9.0));
      // Mixed 3-D: x disjoint (3), y identical (1), z nested (3) -> 9+1+9.
      CkEnforce(near(paratreet::maxdist2(box(0,0,0, 1,1,4), box(2,0,1, 3,1,2)), 19.0));
      // Symmetry on the disjoint case.
      CkEnforce(near(paratreet::maxdist2(box(2,0,0, 3,0,0), box(0,0,0, 1,0,0)), 9.0));
    }

    fof_node = CProxy_FoFPhase1Node<FragData>::ckNew();
    fof = CProxy_FoFPhase1<FragData>::ckNew(fof_node);
  }

  void ExMain::run() {
    driver.run(CkCallbackResumeThread());

    CkExit();
  }

#include "templates.h"

#include "Main.def.h"
