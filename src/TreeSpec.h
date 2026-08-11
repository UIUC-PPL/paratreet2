#ifndef PARATREET_TREESPEC_H_
#define PARATREET_TREESPEC_H_

#include "paratreet.decl.h"
#include "Node.h"
#include "Modularization.h"

class TreeSpec : public CBase_TreeSpec {
public:
    TreeSpec(void)
    : treepiece_decomp(nullptr),
      partition_decomp(nullptr),
      tree(nullptr) { }

    void receiveConfiguration(const CkCallback&, paratreet::Configuration&);
    void receiveDecomposition(const CkCallback&, Decomposition*, bool if_treepiece);
    Decomposition* getTreePieceDecomposition();
    Decomposition* getPartitionDecomposition();
    Tree* getTree();

    void reset() {
      tree.reset();
      treepiece_decomp.reset();
      partition_decomp.reset();
    }

protected:
    std::unique_ptr<Tree> tree;
    std::unique_ptr<Decomposition> treepiece_decomp;
    std::unique_ptr<Decomposition> partition_decomp;

private:
  void getDecomposition(std::unique_ptr<Decomposition>& decomp, paratreet::DecompType decomp_type, bool is_treepiece);
};

#endif
