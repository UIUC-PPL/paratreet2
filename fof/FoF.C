// Module translation unit for the fof module: compiles the non-template part
// of fof.def.h (module registration, called from the application mainmodule's
// registration chain via its `extern module fof`) into libfof.a. Everything
// else in this module is templated headers instantiated by the application
// (see fof-templates.h and examples/fof3).
#include "Paratreet.h"
#include "FoFPhase3.h"

#include <cstdlib>

// ---- Keep-alive ring for the LCI idle-stall (WORKAROUND-lci-idle-stall.md,
// 2026-08-04) ----
// On reconverse/LCI over InfiniBand, about one second of quiet traffic
// degrades a rotation of two-way exchanges from ~25 microseconds to
// 10-25 millisecond tails. Collectives and quiescence detection ARE that
// access pattern, which is the measured cause of FoF's uf2-bracket stalls
// (0.05-0.7 s against ~15 ms of real union-find work). Any background
// traffic suppresses the effect completely — measured down to 160
// messages per second job-wide — so this sends ONE tiny message per
// process every 100 ms to the next process in a ring.
//
// It MUST be a raw Converse message: Charm-level sends are counted by
// quiescence detection, so a continuous Charm-level heartbeat would keep
// CkStartQD/CkWaitQD (which the FoF uf2 bracket depends on) from ever
// firing. Raw Converse messages are invisible to those counters on both
// runtimes. The acceptance test for this code is therefore that
// quiescence still fires (every FoF run exercises it), not the latency
// numbers.
//
// Off-switch, to reproduce the underlying bug for the LCI developers:
// FOF_KEEPALIVE=0. Single-process jobs skip it (a same-process send never
// reaches LCI). The workaround masks the symptom; the bug report lives in
// lci-handover/.
static int fof_keepalive_handler_id;

static void fofKeepAliveHandler(void* msg) { CmiFree(msg); }

static void fofKeepAliveTick(void*) {
  void* msg = CmiAlloc(CmiMsgHeaderSizeBytes);
  CmiSetHandler(msg, fof_keepalive_handler_id);
  CmiSyncNodeSendAndFree((CmiMyNode() + 1) % CmiNumNodes(),
                         CmiMsgHeaderSizeBytes, (char*)msg);
}

// initproc (fof.ci): runs on every PE, so the handler id is registered —
// and identical — everywhere; the periodic sender is armed on exactly one
// PE per process, on the runtime's existing 100 ms condition rung.
void fofKeepAliveInit(void) {
  fof_keepalive_handler_id =
      CmiRegisterHandler((CmiHandler)fofKeepAliveHandler);
  const char* env = std::getenv("FOF_KEEPALIVE");
  bool enabled = !(env && std::atoi(env) == 0);
  if (!enabled || CmiNumNodes() < 2) return;
  if (CmiMyRank() == 0) {
    CcdCallOnConditionKeep(CcdPERIODIC_100ms, fofKeepAliveTick, nullptr);
    if (CmiMyNode() == 0) {
      CmiPrintf("FoF keep-alive: 100 ms raw-Converse ring across %d "
                "processes (FOF_KEEPALIVE=0 disables)\n",
                CmiNumNodes());
    }
  }
}

// Template definitions of both modules arrive via the headers above
// (templates.h through Subtree.h; fof-templates.h through FoFPhase1.h).
// The plain include below compiles the non-template module-registration
// part of fof.def.h. FragData instantiations triggered here duplicate the
// app TU's; the linker deduplicates them.
#include "fof.def.h"
