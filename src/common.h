#ifndef PARATREET_COMMON_H_
#define PARATREET_COMMON_H_

#include <string>
#include <cinttypes> // For printing keys in hex
#include <vector>
#include <cstdlib>
#include "Vector3D.h"
#include "SFC.h"

// Standalone (non-Charm) builds of the passive tree/cache core — the
// TreeCache unit test, or a host application bringing its own runtime
// (design/smp-cache-extraction.md phase 3). charmc defines __CHARMC__;
// without it, the utility structures already compile their serialization
// methods out (the same guard), and the few runtime calls in the
// Node/Particle/TreeCache chain fall back to plain C here. Charm builds
// are untouched (the block is skipped).
#ifndef __CHARMC__
#include <cassert>
#include <cstdio>
#include <cstdlib>
#define CkAssert(expr) assert(expr)
#define CkEnforce(expr) do { if (!(expr)) { fprintf(stderr, "CkEnforce failed: %s\n", #expr); abort(); } } while (0)
#define CkAbort(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); abort(); } while (0)
#define CkPrintf printf
#endif

// Floating point type
#ifndef USE_DOUBLE_FP
typedef float Real;
typedef float SSEReal;
#define REAL_MAX FLT_MAX
#else
typedef double Real;
typedef double SSEReal;
#define REAL_MAX DBL_MAX
#endif

// Simulation domain dimensions
#define NDIM 3
#define CMK_SSE 0

// Particle key
typedef SFC::Key Key;
#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif
#define KEY_BITS (sizeof(Key)*CHAR_BIT)
#define BITS_PER_DIM (KEY_BITS/NDIM)
#define BOXES_PER_DIM (1<<(BITS_PER_DIM))

namespace paratreet {
// Peak resident set size of the calling PROCESS in MB, from VmHWM in
// /proc/self/status (Linux high-water mark, so it survives the transient it
// is measuring). Returns -1 where unavailable. Cheap enough to call at phase
// boundaries; do not put it in an inner loop.
inline long procHighWaterMB() {
  FILE* f = fopen("/proc/self/status", "r");
  if (!f) return -1;
  char line[256];
  long kb = -1;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "VmHWM:", 6) == 0) { sscanf(line + 6, "%ld", &kb); break; }
  }
  fclose(f);
  return kb < 0 ? -1 : kb / 1024;
}
// ---- PE-SET SPLIT (Kale, 2026-08-16; reports/pe-set-split-results.md).
//
// Treat a process's PEs as s independent sets for phase 1: piece pairs that
// cross a set boundary are left out of the phaseB pool and phase 3 merges them
// instead. Lives here rather than in FoF because BOTH ends need it — the pool
// build (FoFPhase1.h::buildPoolSlice) decides what to omit, and the phase-3
// ownership prune (Traverser.h::dualSkipLocalSource) must let exactly those
// pairs through. Getting only one end right returns the wrong component count:
// measured, 3642 against a gold of 3549 (job 5287404).
//
//   FOF_PE_SETS=s        sets per process (1 = off, the default)
//   FOF_PE_SETS_NODE=n   apply only on process n (unset = every process)
//   FOF_PE_SETS_MODE=0|1 0 = blocked, 1 = round robin (measured better)
inline int peSets() {
  static const int v = [] {
    const char* e = std::getenv("FOF_PE_SETS");
    int x = e ? std::atoi(e) : 1;
    return x >= 1 ? x : 1;
  }();
  return v;
}
inline int peSetsMode() {
  static const int v = [] {
    const char* e = std::getenv("FOF_PE_SETS_MODE");
    return e ? std::atoi(e) : 0;
  }();
  return v;
}
// Sets active on THIS process (1 = no split here).
// FOF_PE_SETS_NODES is a comma-separated victim LIST — the whole point of a
// mechanism with no per-process cost is that it can treat the top N processes
// at once, which shedding never could (phase1-idle-structure.md: the phaseB
// tail is broad; top-1 holds 5.4% of the work, top-25 holds 48.8%).
// FOF_PE_SETS_NODE (singular) is kept for the earlier single-victim runs.
inline int peSetsHere() {
  const int s = peSets();
  if (s <= 1) return 1;
  static const bool scoped = [] {
    return std::getenv("FOF_PE_SETS_NODE") || std::getenv("FOF_PE_SETS_NODES");
  }();
  if (scoped) {
    static const std::vector<int> victims = [] {
      std::vector<int> v;
      const char* e = std::getenv("FOF_PE_SETS_NODES");
      if (!e) e = std::getenv("FOF_PE_SETS_NODE");
      for (const char* p = e; p && *p;) {
        char* end = nullptr;
        long x = std::strtol(p, &end, 10);
        if (end == p) break;
        v.push_back((int)x);
        p = (*end == ',') ? end + 1 : end;
      }
      return v;
    }();
    bool mine = false;
    for (int v : victims) if (v == CkMyNode()) { mine = true; break; }
    if (!mine) return 1;
  }
  const int n = CkNodeSize(CkMyNode());
  return s > n ? n : s;
}
inline int peSetOfPe(int pe) {
  const int sets = peSetsHere();
  if (sets <= 1) return 0;
  const int n = CkNodeSize(CkMyNode());
  const int lr = pe - CkNodeFirst(CkMyNode());
  if (lr < 0 || lr >= n) return -1;
  return peSetsMode() == 1 ? (lr % sets) : (lr * sets / n);
}
// TreePiece index -> set id on THIS process; -1 = not ours / unknown.
// Written ONCE per iteration by the pool build, on the single PE that
// broadcasts buildPoolSlice, and read-only thereafter — including by the
// phase-3 traversal on every PE. One instance per PROCESS, which is exactly
// the scope wanted: in SMP all PEs of a process share this address space.
inline std::vector<signed char>& pieceSetTable() {
  static std::vector<signed char> t;
  return t;
}
// Should the phase-3 ownership prune be VETOED for this pair?
// True = walk it anyway, because the two sides are in DIFFERENT sets and so
// phase 1 did not merge them.
//
// BOTH sides are looked up in the table, which is keyed by phase-1 set
// membership, i.e. by the CLAIMED PE. An earlier version took the target's set
// from CkMyPe() instead; that is wrong under FOF_STEALA, where a piece is
// claimed by a PE that need not be the one it resides on, and it lost a
// handful of merges (10k 3555/3562/3557 against a gold of 3549, job 5287618).
// Source and target indices are the same space here — the mirrored-pairs prune
// a few lines below already compares node->tp_index with tp.thisIndex directly.
//
// Every "don't know" answers TRUE (walk it). Wrongly walking a pair costs
// time; wrongly pruning one loses a merge and changes the answer.
inline bool peSetKeepLocalPair(int src_piece, int tgt_piece) {
  if (peSetsHere() <= 1) return false;          // split off here: prune as before
  const auto& t = pieceSetTable();
  const int a = (src_piece >= 0 && (size_t)src_piece < t.size()) ? t[src_piece] : -1;
  const int b = (tgt_piece >= 0 && (size_t)tgt_piece < t.size()) ? t[tgt_piece] : -1;
  if (a < 0 || b < 0) return true;
  return a != b;
}

// Targeted shedding (design/piece-load-model.md): one candidate piece on the
// victim process, as gathered by the shedDecide reduction. POD and fixed size,
// because it travels as raw bytes through CkReduction::concat.
//
// Why the DRIVER and not the element decides: the element cannot rank itself
// against its siblings without a gather, and a gather to the Driver already
// has to happen — the migration must be issued from OUTSIDE the array
// traversal (issuing it from inside the decision broadcast is what crashed,
// fb43f06), so the Driver needs the plan regardless. Scoring in the element
// and choosing in the Driver therefore costs no extra communication.
struct ShedCand {
  int index;      // TreePiece array index
  int n;          // particles, i.e. what the migration will cost to move
  double score;   // rank key, higher = shed first (FOF_SHED_RANK)
};

}  // namespace paratreet

#endif // PARATREET_COMMON_H_
