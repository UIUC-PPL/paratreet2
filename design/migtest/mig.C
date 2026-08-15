#include "mig.decl.h"
CProxy_Main mainProxy; int nElements; int gMode;
struct Main : CBase_Main {
  CProxy_Mover arr; int expect;
  Main(CkArgMsg* m) {
    nElements = (m->argc > 1) ? atoi(m->argv[1]) : 8;
    gMode     = (m->argc > 2) ? atoi(m->argv[2]) : 0;
    delete m;
    mainProxy = thisProxy;
    arr = CProxy_Mover::ckNew(nElements);
    CkPrintf("MIGTEST mode %d, %d elements, %d PEs\n", gMode, nElements, CkNumPes());
    switch (gMode) {
      case 0: for (int i=0;i<nElements;i++) arr[i].p2pMigrate((CkMyPe()+1)%CkNumPes()); break;
      case 1: arr.bcastMigrate(); break;
      case 2: arr.bcastDefer(); break;
      case 3: arr.bcastPlan(); break;
    }
    if (gMode != 3) { CkStartQD(CkCallback(CkIndex_Main::phaseDone(), thisProxy)); }
  }
  void planReady(CkReductionMsg* msg) {           // mode 3: plan lands here
    int n = msg->getSize()/(int)(2*sizeof(int)); int* p = (int*)msg->getData();
    int moved=0;
    for (int i=0;i<n;i++) if (p[2*i+1]>=0) { arr[p[2*i]].doMigrate(p[2*i+1]); moved++; }
    CkPrintf("MIGTEST plan: %d elements to migrate\n", moved);
    delete msg;
    CkStartQD(CkCallback(CkIndex_Main::phaseDone(), thisProxy));
  }
  void phaseDone() {                            // QD after migrations
    CkPrintf("MIGTEST migrations quiesced; verifying every element answers\n");
    arr.checkAlive();
  }
  void allAnswered(CkReductionMsg* m) {
    int n = *(int*)m->getData(); delete m;
    CkPrintf("MIGTEST mode %d: %d of %d elements answered -> %s\n",
             gMode, n, nElements, n==nElements ? "PASS" : "FAIL");
    CkExit();
  }
};
struct Mover : CBase_Mover {
  Mover() {} 
  Mover(CkMigrateMessage* m) { delete m; }
  int dest_ = -1;
  void p2pMigrate(int d) { if (CkNumPes()>1) migrateMe((CkMyPe()+1)%CkNumPes()); }
  void bcastMigrate()    { if (CkNumPes()>1) migrateMe((CkMyPe()+1)%CkNumPes()); }
  void bcastDefer()      { if (CkNumPes()>1) thisProxy[thisIndex].doMigrate((CkMyPe()+1)%CkNumPes()); }
  void bcastPlan() {                       // decide, then reduce; migrate later
    dest_ = (CkNumPes()>1) ? (CkMyPe()+1)%CkNumPes() : -1;
    int pair[2] = {thisIndex, dest_};
    contribute(sizeof(pair), pair, CkReduction::concat,
               CkCallback(CkIndex_Main::planReady(NULL), mainProxy));
  }
  void doMigrate(int d) { migrateMe(d); }
  void checkAlive() { int one=1; contribute(sizeof(int),&one,CkReduction::sum_int,CkCallback(CkIndex_Main::allAnswered(NULL), mainProxy)); }
  void pup(PUP::er& p) { CBase_Mover::pup(p); p|dest_; }
};
#include "mig.def.h"
