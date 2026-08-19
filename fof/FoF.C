// Module translation unit for the fof module: compiles the non-template part
// of fof.def.h (module registration, called from the application mainmodule's
// registration chain via its `extern module fof`) into libfof.a. Everything
// else in this module is templated headers instantiated by the application
// (see fof-templates.h and examples/fof3).
#include "Paratreet.h"
#include "FoFPhase3.h"

#include <cstdlib>
#include <mutex>

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
// FABRIC-SPECIFIC: this is an InfiniBand-path (libfabric/IBV provider)
// behaviour, measured on Anvil (jobs 19608513-19661625). It does NOT
// reproduce on Frontier's Slingshot/CXI as of 2026-08-18: 220k round
// trips, 0 over 1 ms, at up to 64 peers and 60 s silences, ring on or
// off (jobs 5303887/5303902), and FoF's timings there are unchanged
// with the ring off (job 5302846). The ring stays on because it is
// load-bearing on InfiniBand and costs nothing measurable elsewhere.
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
static long fof_keepalive_ticks = 0;
static double fof_keepalive_t0 = 0;
static bool fof_keepalive_verbose = false;
// Ring period, ms (FOF_KEEPALIVE_MS, default 100). Three useful settings:
//   100   the validated workaround (suppression measured total at every
//         period from 200 us to 200 ms, job 19661625) — and, with the gap
//         monitor below, a tripwire that it KEEPS working.
//   10    finer monitor sampling (per-process episode counts undercount
//         ~3-10x at 100 ms; machine-wide detection is fine either way).
//   1000+ PROBE mode: the ring's own traffic suppresses the Anvil stall,
//         so a sparse ring deliberately leaves quiet windows longer than
//         the ~1 s onset and each message then samples delivery exactly
//         at the trigger. Use to MEASURE the raw bug without projections;
//         it stops being a workaround at these settings.
static double fof_keepalive_period_ms = 100.0;

// ---- Ring-gap monitor (Kale, 2026-08-19): the ring doubles as a
// delivery-delay instrument that needs no tracing. Each process receives
// one ring message per 100 ms from its predecessor; the deviation of the
// inter-ARRIVAL gap from that cadence is (predecessor's send stretch) +
// (delivery delay) + (handler scheduling). The inter-SEND gaps are
// recorded too, so a stretched sender (a busy rank slows its periodic
// ladder to ~4/s legitimately — the rank-probe finding above) is
// distinguishable from a delivery stall: arrival jitter WITHOUT matching
// send jitter anywhere is the LCI idle-stall signature (10-25 ms tails,
// WORKAROUND-lci-idle-stall.md). Counters only, one print per process at
// phase3Stats (fofKeepAliveGapsPrint) — watch over10/over25 on machines
// where the stall is live (Anvil/InfiniBand; absent on Frontier CXI as
// of 2026-08-18, jobs 5303887/5303902).
static std::mutex fof_ka_gap_mtx;
struct FofKaGaps {
  long n = 0, over5 = 0, over10 = 0, over25 = 0, over100 = 0;
  double last = -1, max_ms = 0, max_at = 0;
  void record(double now) {
    if (last >= 0) {
      // deviation from the configured cadence, ms
      double d = (now - last) * 1e3 - fof_keepalive_period_ms;
      n++;
      if (d > 5) over5++;
      if (d > 10) over10++;
      if (d > 25) over25++;
      if (d > 100) over100++;
      if (d > max_ms) { max_ms = d; max_at = now; }
    }
    last = now;
  }
};
static FofKaGaps fof_ka_arrival, fof_ka_send;

static void fofKeepAliveHandler(void* msg) {
  {
    std::lock_guard<std::mutex> g(fof_ka_gap_mtx);
    fof_ka_arrival.record(CmiWallTimer());
  }
  CmiFree(msg);
}

namespace paratreet {
void fofKeepAliveGapsPrint(void) {
  std::lock_guard<std::mutex> g(fof_ka_gap_mtx);
  if (fof_ka_arrival.n == 0 && fof_ka_send.n == 0) return;
  CmiPrintf("FOF3STAT keepalive_gaps: node %d arr_n %ld over5 %ld "
            "over10 %ld over25 %ld over100 %ld max_ms %.1f at_s %.2f | "
            "send_n %ld over5 %ld over10 %ld over25 %ld over100 %ld "
            "max_ms %.1f\n",
            CmiMyNode(), fof_ka_arrival.n, fof_ka_arrival.over5,
            fof_ka_arrival.over10, fof_ka_arrival.over25,
            fof_ka_arrival.over100, fof_ka_arrival.max_ms,
            fof_ka_arrival.max_at, fof_ka_send.n, fof_ka_send.over5,
            fof_ka_send.over10, fof_ka_send.over25, fof_ka_send.over100,
            fof_ka_send.max_ms);
}
}  // namespace paratreet

static void fofKeepAliveTick(void*) {
  {
    std::lock_guard<std::mutex> g(fof_ka_gap_mtx);
    fof_ka_send.record(CmiWallTimer());
  }
  void* msg = CmiAlloc(CmiMsgHeaderSizeBytes);
  CmiSetHandler(msg, fof_keepalive_handler_id);
  CmiSyncNodeSendAndFree((CmiMyNode() + 1) % CmiNumNodes(),
                         CmiMsgHeaderSizeBytes, (char*)msg);
  // Rate instrument (FOF_KEEPALIVE_VERBOSE=1): the workaround's measured
  // basis is ~10 ticks/s/process; if the condition rung fires much faster
  // or much slower than that, the implementation does not match the
  // measurement and the verification numbers mean something else.
  fof_keepalive_ticks++;
  if (fof_keepalive_verbose) {
    if (fof_keepalive_ticks == 1) fof_keepalive_t0 = CmiWallTimer();
    if (fof_keepalive_ticks <= 3 || fof_keepalive_ticks == 10 ||
        fof_keepalive_ticks == 30) {
      CmiPrintf("keep-alive tick %ld at t=%.2f s (process %d PE %d)\n",
                fof_keepalive_ticks, CmiWallTimer() - fof_keepalive_t0,
                CmiMyNode(), CmiMyPe());
    }
    if (fof_keepalive_ticks % 100 == 0) {
      double dt = CmiWallTimer() - fof_keepalive_t0;
      CmiPrintf("keep-alive rate: %ld ticks in %.1f s (%.1f/s, process %d)\n",
                fof_keepalive_ticks, dt,
                dt > 0 ? (fof_keepalive_ticks - 1) / dt : 0.0, CmiMyNode());
    }
  }
}

// Non-default periods (FOF_KEEPALIVE_MS != 100) run on a self-rearming
// CcdCallFnAfter chain instead of the fixed 100 ms condition rung — the
// same idiom the S3 poll loop used on both runtimes.
static void fofKeepAliveChain(void* a, double) {
  fofKeepAliveTick(a);
  CcdCallFnAfter(fofKeepAliveChain, a, fof_keepalive_period_ms);
}

// Diagnosis probe (temporary): counts the 100 ms periodic condition on
// EVERY rank, independent of the keep-alive sender. Finding (16M local
// run, 2026-08-04): no rank's ladder ever stops; the busiest rank's tick
// interval stretches to ~250 ms during long entry-method executions
// (the sends inside them also contend on the network library's endpoint
// lock against other ranks' progress polling, per stack samples). So
// tick rates between about 4 and 10 per second are all healthy.
static thread_local long fof_rankprobe_ticks = 0;
static thread_local double fof_rankprobe_t0 = 0;
static void fofRankTickProbe(void*) {
  fof_rankprobe_ticks++;
  if (fof_rankprobe_ticks == 1) fof_rankprobe_t0 = CmiWallTimer();
  if (fof_rankprobe_ticks <= 3 || fof_rankprobe_ticks % 50 == 0) {
    CmiPrintf("rank-probe: process %d rank %d pe %d tick %ld at t=%.2f s\n",
              CmiMyNode(), CmiMyRank(), CmiMyPe(), fof_rankprobe_ticks,
              CmiWallTimer() - fof_rankprobe_t0);
  }
}

// Diagnosis probe (temporary): counts CcdPROCESSOR_STILL_IDLE firings —
// raised directly from the scheduler's idle branch with no timer
// arithmetic — to discriminate "scheduler stopped servicing conditions"
// from "the periodic ladder's time bookkeeping broke".
static long fof_stillidle_count = 0;
static void fofStillIdleProbe(void*) {
  fof_stillidle_count++;
  if (fof_stillidle_count == 1 || fof_stillidle_count == 1000000 ||
      fof_stillidle_count == 10000000 || fof_stillidle_count == 100000000) {
    CmiPrintf("still-idle probe: count %ld at t=%.2f s, keepalive ticks %ld "
              "(process %d)\n",
              fof_stillidle_count, CmiWallTimer(), fof_keepalive_ticks,
              CmiMyNode());
  }
}

// initproc (fof.ci): runs on every PE, so the handler id is registered —
// and identical — everywhere; the periodic sender is armed on exactly one
// PE per process, on the runtime's existing 100 ms condition rung.
void fofKeepAliveInit(void) {
  fof_keepalive_handler_id =
      CmiRegisterHandler((CmiHandler)fofKeepAliveHandler);
  const char* env = std::getenv("FOF_KEEPALIVE");
  bool enabled = !(env && std::atoi(env) == 0);
  const char* venv = std::getenv("FOF_KEEPALIVE_VERBOSE");
  fof_keepalive_verbose = (venv && std::atoi(venv) != 0);
  const char* penv = std::getenv("FOF_KEEPALIVE_MS");
  if (penv && std::atof(penv) > 0) fof_keepalive_period_ms = std::atof(penv);
  if (!enabled || CmiNumNodes() < 2) return;
  // Diagnosis (temporary): with verbose on, count the periodic condition
  // on every rank so starved ranks identify themselves.
  if (fof_keepalive_verbose) {
    CcdCallOnConditionKeep(CcdPERIODIC_100ms, fofRankTickProbe, nullptr);
  }
  // Arm on the LAST rank of each process. Any rank is CORRECT: stack
  // sampling plus per-rank tick counts (16M local run, 2026-08-04) show
  // every rank's scheduler loop services the periodic conditions for the
  // whole run — a bare-runtime probe program (ccdprobe, sibling of
  // paratreet2) confirms threaded entry methods do not starve them
  // either. But the tick INTERVAL stretches while a rank executes long
  // entry methods: rank 0, which hosts the mainchare, the driver thread,
  // and reduction roots, degraded from 10 to about 4 ticks per second
  // during the heaviest phase, while the last rank held near 10. The
  // last rank therefore keeps the ring rate closest to the measured
  // basis of the workaround. (An earlier reading that rank 0's
  // conditions "go silent" was wrong — an artifact of sparse print
  // points and block-buffered multi-process output.)
  if (CmiMyRank() == CmiMyNodeSize() - 1) {
    if (fof_keepalive_period_ms == 100.0)
      CcdCallOnConditionKeep(CcdPERIODIC_100ms, fofKeepAliveTick, nullptr);
    else
      CcdCallFnAfter(fofKeepAliveChain, nullptr, fof_keepalive_period_ms);
    if (fof_keepalive_verbose)
      CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE, fofStillIdleProbe, nullptr);
    if (CmiMyNode() == 0) {
      CmiPrintf("FoF keep-alive: %.0f ms raw-Converse ring across %d "
                "processes (FOF_KEEPALIVE=0 disables; FOF_KEEPALIVE_MS "
                "sets the period — 1000+ is probe mode, not a workaround)\n",
                fof_keepalive_period_ms, CmiNumNodes());
    }
  }
}

// Template definitions of both modules arrive via the headers above
// (templates.h through TreePiece.h; fof-templates.h through FoFPhase1.h).
// The plain include below compiles the non-template module-registration
// part of fof.def.h. FragData instantiations triggered here duplicate the
// app TU's; the linker deduplicates them.
#include "fof.def.h"
