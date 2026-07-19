#include "prefixBalance.decl.h"

class Prefix : public CBase_Prefix {
  Prefix_SDAG_CODE

  private:
    int index, targetIndex, value; //parallel prefix members
    int nInts; //number of integers per chare array element
    int numElements;//number of chares in chare array
  public:
    Prefix(int nElements);
    Prefix(CkMigrateMessage* msg);
    int getValue(){return value;};

    void pup(PUP::er &p) {
      p|index;
      p|targetIndex;
      p|value;
      p|nInts;
      p|numElements;
    }

};

