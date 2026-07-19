#include "Main.h"

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
    // run with -d oct.
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

    CkPrintf("\n[PARATREET FOF PHASE 1]\n");
    if (conf.input_file.empty()) CkAbort("Input file unspecified");
    CkPrintf("Input file: %s\n", conf.input_file.c_str());
    CkPrintf("Decomposition type: %s\n", paratreet::asString(conf.decomp_type).c_str());
    CkPrintf("Tree type: %s\n", paratreet::asString(conf.tree_type).c_str());
    CkPrintf("Minimum number of subtrees: %d\n", conf.min_n_subtrees);
    CkPrintf("Minimum number of partitions: %d\n", conf.min_n_partitions);
    CkPrintf("Maximum number of particles per leaf: %d\n", conf.max_particles_per_leaf);

    fof_node = CProxy_FoFPhase1Node<FragData>::ckNew();
    fof = CProxy_FoFPhase1<FragData>::ckNew(fof_node);
  }

  void ExMain::run() {
    driver.run(CkCallbackResumeThread());

    CkExit();
  }

#include "templates.h"

#include "Main.def.h"
