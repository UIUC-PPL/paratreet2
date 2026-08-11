#ifndef EXAMPLE_MAIN_H
#define EXAMPLE_MAIN_H

#include "Main.decl.h"
#include "Paratreet.h"
#include "FoFData.h"
#include "FoFPhase1.h"

class ExMain: public paratreet::Main<FragData> {
  // The fof module's chares are registered by the app (the core registers
  // only its own): extend the base registration with the fof module's.
  virtual void __register(void) override {
    paratreet::Main<FragData>::__register();
    fof::registerChares<FragData>();
  }

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

  // App-specific flags (parsed by getopt in main(); see README):
  //   -b <factor>  linking-length factor: b = factor * (V/N)^(1/3)
  //   -c <mode>    correctness-check mode: full | stats | auto
  //   -u <mode>    UF_2 implementation: dist (default) | serial (see
  //                design/step4.md; serial kept for A/B against the
  //                gather-to-one v1/3a path)
  //   -m <int>     minimum component size (particles) for REPORTING: a
  //                component "survives" if its size >= m. Default 0 = report
  //                everything (byte-identical to pre-step-5 output). When m>0
  //                an extra FOF3STAT surviving line is printed alongside the
  //                unchanged (unpruned) components line (design/step5-pruning.md).
  //                This is a reporting FILTER only; it never relabels particles
  //                or changes the validated partition/equality checks.
  double fof_b_factor = 0.2;
  enum class CheckMode { Auto, Full, Stats };
  CheckMode check_mode = CheckMode::Auto;
  enum class UF2Mode { Dist, Serial };
  // Serial is the production default (quiescence-free bracket, immune to
  // the LCI idle-stall; flat 0.01-0.02 s at every measured scale). Dist
  // remains a fully supported research mode — distributed union-find is a
  // research focus of this project and is expected to improve as process
  // counts grow.
  UF2Mode uf2_mode = UF2Mode::Serial;
  // Phase-3 walk mechanism (-w; design/dual-tree.md): dual (DEFAULT since
  // the Anvil 80M A/B — 20x/15x/7.6x/2.9x/1.6x faster than transposed at
  // P = 1/2/4/8/16, identical outputs; -u dist only) or transposed (the
  // original startDown source-tree-vs-flat-target-leaves walk, kept
  // permanently as the independent A/B oracle).
  enum class WalkMode { Transposed, Dual };
  WalkMode walk_mode = WalkMode::Dual;
  // Min component size for reporting (-m); 0 = report everything (default).
  int fof_min_component_size = 0;
  // -G <t>: occupancy threshold (expected particles per b/sqrt(6) cell at
  // a chare's root) above which phaseA solves that chare's self pair with
  // the cell grid instead of the tree walk. 0 disables the grid (the
  // walk-only oracle for A/B). DEFAULT 4 since 2026-08-04 (Kale's go on
  // the standing recommendation): the 80M/2B A/B on current main showed
  // -G 4 does not hurt at 80M (mild win at ~21k particles/chare) and
  // cuts 2B phaseA ~19-29% (design/phase1-scaling.md final round +
  // ledger); counts are bit-identical by construction and re-verified at
  // every scale. The old default 1.0 was behaviorally off at real
  // dataset occupancies.
  double fof_grid_threshold = 4.0;
  // -E <n>: stream phase-3 edge batches of n edges into UF_2 DURING the
  // walk (design/walk-uf2-overlap.md step 1), overlapping the union
  // cascades with the walk under one QD. 0 = classic post-walk injection
  // (the A/B oracle). Ignored under AGGREGATION (tram buffers are
  // invisible to the walk's QD). Default 4096.
  long fof_uf2_stream_batch = 4096;
  // -g: compute and print the phase-1 fragments histogram (FOF3STAT
  // fragments line). Off by default since sparse-uf2: the histogram was the
  // only surviving consumer of countFragments, which is otherwise off the
  // dist critical path (design/sparse-uf2-encoding.md). Serial mode (-u
  // serial) always prints it, as before.
  bool fof_frag_histogram = false;
  // -C: skip the cache memory accounting (CacheManager::cacheStats and its
  // FOF3STAT cache line). The accounting walks the process's entire cached
  // tree on one processor per process — measured 314 ms on the hot process
  // at 80M/480 PEs — AFTER all timed brackets, so it never affects
  // measurements, but it clutters projections timelines. Use -C for traced
  // runs unless the cache accounting itself is under investigation (Kale,
  // 2026-08-04). The cheap process-RSS memory line is unaffected.
  bool fof_skip_cache_stats = false;
  // Single-distribution mode (design/single-distribution-mode.md) — no
  // Partition array. DEFAULT since 2026-08-04 (80M gate job 19661057:
  // components bit-identical across 4 arms; decomposition ~25% faster —
  // the partition creation + assignment passes vanish). -S is kept as a
  // no-op for compatibility. -w transposed (the standing walk oracle)
  // needs the Partition array and automatically selects dual
  // distribution with a printed note; the FragCheckVisitor sweep of
  // -c full runs only in that dual mode (the grid/O(n^2) component
  // checks and the in-walk annotation CkEnforce always run).
  bool single_distribution = true;
  // Pre-created UF_2 placement-map group (see preTraversalFn): created early
  // so its branches exist everywhere before phase 3's array creation
  // consults it (reconverse has no group-dependency buffering).
  CkGroupID uf_node_map_gid;
  // Periodic boundary conditions (-P <L>; design/pbc.md): cubic box period
  // applied on all three axes. Default 0 = open boundaries (PBC off, exact
  // current behavior; the periodic branch is a no-op). Threaded into phase 1
  // (runFoFPhase1) and phase 3 (the FoFEdgeVisitor) as Vector3D<Real>(L,L,L).
  Real pbc_period = 0.0;
  // Auto-mode gate: full verification gathers ~24 B/particle to PE 0 and
  // runs the serial grid reference there; above this N, auto falls back to
  // stats mode (force with -c full, memory permitting).
  static constexpr int kAutoFullMaxN = 20000000;
};

#endif
