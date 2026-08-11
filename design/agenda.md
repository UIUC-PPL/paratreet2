# Agenda: future work items

Standing list of agreed future work, in rough priority order. Each item
points at its design note where one exists. Started 2026-08-05.

1. **Extend paratreet2 to host FoF phase 1** (design/walk-unification.md;
   GPU-dimension deep dive 2026-08-11: design/walk-unification-gpu.md —
   staged order serving both the toolkit and Ritvik's HIP port, with
   stage 0 (device data contract + gridSelfUnion as kernel one)
   independent of everything and ready now;
   Kale's principle: the framework supports walks, and local versus
   across-process is a smaller variation than the commonality among
   walks). Expedient ordering while the report needs data: lift the
   process-level PAIR POOL and the deposit-chain termination out of
   FoFPhase1Node into framework services first (also the foundation for
   the phaseB offload); absorb the self-walk and PE-local drivers and
   shrink the phase-1 walk bodies to visitors afterward. Gate every
   step on bit-identical counts against existing baselines.
2. DONE 2026-08-05 (commit 4a3b496): serial-mode credit-counter
   termination for the dual walk. The serial pipeline now contains no
   quiescence detection at all. FOF_WALK_QD=1 keeps the quiescence
   path as the A/B oracle; Anvil-scale bracket measurement rides the
   next measurement round.
3. Steal-based phaseB cross-process offload. The implementation and its
   measurements live on the phaseb-steal branch, not here: correct at
   every round but the wall barely moved, and the branch records why
   (design/phaseb-offload.md there). Reconsider against item 4 and
   against simply running fewer, larger processes, which measured
   better than moving work between processes.
4. DONE 2026-08-07 (commit 553d722), premises superseded (analysis
   2026-08-11). The phaseB pool build is parallel: every thread
   enumerates a stride of the (thread-pair, TreePiece-pair) space into
   its own slice and sorts it; the last finisher assembles the pool
   with a comparison-free round-robin merge. The two follow-ons the
   item predicted are WITHDRAWN. Size-based splitting stays default
   off: measured at 2B WITH the parallel build in place it produced
   24x more units with the largest unit unchanged — a small box in a
   dense core holds enormous work (design/phaseb-handoff-2026-08-10.md,
   "Dead ends"); the FOF_POOL_SPLIT_SIZE remnants want deleting. And
   phaseB no longer has a volume problem to scale away: the uniformity
   annotation left ~145 core-seconds against a 1.3-1.6 s wall at 2B —
   the wall is CROSS-PROCESS imbalance, which a per-process pool cannot
   address at any build speed.
4b. phaseA/phaseB balancing program (one cost model, two halves;
   Kale's framing 2026-08-11: before stealing, rebalance phaseA).
   HALF 1 — phaseA light reassignment: a PE runs phaseA over pieces
   registered to sibling PEs of its process, no location-manager
   involvement. Feasibility AUDITED SOUND 2026-08-11
   (design/phasea-reassignment.md): no entry method targets a
   TreePiece in the phase-1 window; correctness rests on the grouping
   theorem (any partition of the process's pieces works, provided
   exclusive ownership per piece and the pool enumerates cross-GROUP
   pairs — the pool re-key is the one silent-under-merge trap);
   geometry-aware claiming is load-bearing; phaseA self pairs are 90%
   of phase-1 pair time, predictable at R2 0.90 from piece size alone
   (~600 items/process onto 15 PEs). GATED on a ~15-line measurement
   that does not exist: split the recorded 2.79 max/avg phaseA skew at
   2B into within-process x cross-process factors — if within ~1, the
   item becomes cross-process placement instead.
   HALF 2 — phaseB predicted-cost ranking (was 4b alone): the pool's
   LPT key is raw overlap volume, no densities; particle-count product
   explains 13% of pair cost, expected-pairs 38%/0.60 log-log, top 1%
   of pairs hold 60% of the time. Steps: (i) land FragData::n_below
   (de21b74) on main; (ii) densities into the LPT key, re-measure
   t_phaseB_maxpair; (iii) split only the predicted tail. Gate half 2
   PASSED 2026-08-11: at 2B the expected-pairs term ALONE explains
   0.87 of phaseB pair cost (particle-count product 0.04, descent
   size 0.01) and the top 1% of pairs hold 79.5% of the time —
   stronger than 80M on every reading (cost-model-probe.md).
   ORDER: skew-split measurement -> phaseA claim pool (dynamic,
   own-first, geometry-preferring, cost-ordered; FOF_PHASEA_STEAL=0 as
   the A/B) -> phaseB key -> tail splitting. Full analysis, prior art
   (three earlier proposals, the old paratreet parallel-help patch),
   constraints and gates: design/phasea-reassignment.md.
5. Decomposition anti-scaling at 16 nodes (80M: 0.78 s at 8 nodes ->
   1.65 s at 16; design/speedup-campaign-2026-08-05.md follow-up 2):
   profile splitter computation and particle flush at 1920 PEs.
6. DONE 2026-08-10 (modified scope, Kale): -u serial is now the fof3
   default (quiescence-free bracket, immune to the LCI idle-stall).
   The keep-alive ring is KEPT (it costs little and the stall issue is
   unresolved), and dist mode is KEPT fully supported with explicit
   -u dist runs in make test: distributed union-find is a research
   focus of this project and is expected to improve as process counts
   grow. Validated: 16-run matrix, 1M serial-vs-dist histograms
   identical (333889).
7. IMPLEMENTED 2026-08-10 (with item 12; design/relabel-representative.md,
   stages 1-3 = commits ec06543/fa0a383/bf9dc12): the phase-3 label map
   is owner-sharded at processor 0 and delivered as per-process slices
   to the node branch (size-dependent: maps under 1 MB keep the
   broadcast; FOF_SLICE_MIN_BYTES=0 forces slicing for tests). All
   identity gates green on both runtimes, both transport paths.
   MEASURED 2026-08-11 (jobs 19774169/19774171, results in the design
   note): relabel(p3) at 2B/16 nodes 2.84 s -> ~0.15 s (~18x; stages
   1-2 = ~2.6x, stage 3 = ~7x more); phase-1 relabel ~4-5x at 80M;
   all runs exact at both scales.
8. DONE 2026-08-10 (branch treepiece-rename, code commit 9ab9f04 +
   companion doc commit): renamed the Subtree chare to TreePiece across
   code, comments, and documentation ("subtree" collides with the
   generic word in prose). Literal tree-sense uses of "subtree"
   (flat_subtree, installSubtree/collectSubtree, partial/descent/
   sibling subtrees) are deliberately kept. User-visible: config key
   nSubtreesMin -> nTreePiecesMin (-n flag unchanged), [Meta] label
   n_subtree -> n_treepiece, startup prints now say TreePieces.
9. LCI items for the handover: packet-pool exhaustion at 2B on 4-8
   nodes (refill_recvs deadlock alerts then poll_comp_impl assert
   during the input flush); the idle-stall itself (dist uf2 3.7 s at
   80M/16 nodes with the keep-alive ring on). LCI needs a better
   example program (Kale, 2026-08-10). Also check +lci_ndevices for
   future Anvil runs — Kale recalls multiple devices per process in
   Ritvik's notes or scripts; our own 2026-08-01 Anvil A/B found no
   significant effect on InfiniBand (charm-notes). RESOLVED 2026-08-11
   (Kale): on FRONTIER Ritvik sets +lci_ndevices to about half the
   worker threads per process, capped at 8 (so 8 for ppn 15) — i.e.
   min(8, ppn/2). Slingshot, not InfiniBand, so it does not contradict
   the Anvil null result; adopt his setting for Frontier runs, keep
   Anvil scripts flag-free.
10. DONE 2026-08-06 (freeze-pass counting commit): eliminated the component counter's particle
   pass (Kale's design, 2026-08-06). Count per union-find root during
   the phaseA freeze pass (dense array increment beside the existing
   find()); carry the per-processor tip-count map through every label
   rewrite the particles undergo; depositLabelCounts then deposits the
   map instead of re-counting all particles. Removes the 60 ms band at
   80M (projected ~300 ms per processor at 2B); debug flag keeps the
   old particle loop as a cross-check.
11. MERGED UPSTREAM 2026-08-11 (Ritvik, merge 40d7ecc into
   fof_with_aggregation; originally unionfind branch batch-labeling, commit
   cd8a415, pushed — RITVIK REVIEW pending, he stewards unionfind;
   Anvil-scale measurement pending): batch the component-labeling
   requests per destination chare. Three fixes in one: the per-
   destination batch entry, plus making the parent-cache dedup LIVE
   (entries were never created — dead code), plus two latent defects
   that surfaced (uninitialized compNum; set_component never cleared
   drained requestors, an infinite work-queue loop once entries
   existed). Validated -u dist both runtimes incl. 32-process.
   Original text: The labeling scatter (boss_count_prefix_done ->
   insertDataNeedBoss, observed ~1400 sends from one chare at 2B) already
   deduplicates by parent through a cache; flushing per destination would
   collapse it to at most one message per peer chare. Dist mode remains
   important (Kale, 2026-08-05) and has been competitive — this and its
   quiescence-closed labeling phase are its main remaining fine-grained
   patterns.
12. IMPLEMENTED 2026-08-10 (stage 1 of design/relabel-representative.md,
    commit ec06543; measurement pending with item 7). Original text
    (Kale, 2026-08-06): keep the
    compressed per-processor union-find array from the phaseA freeze
    (uf_parent[i] = flat index of i's representative). Apply every
    label map — phase-1 merge, tip encoding, the phase-3 map in either
    union-find mode — at representative granularity only (thousands of
    hash lookups per processor), then materialize per-particle labels
    as one indexed load through the representative: no hash lookup in
    any per-particle relabel loop. The sign convention (or an untouched
    positive tip) is the "local fragment" marker. Companion to the
    freeze-pass counting; together they remove every per-particle hash
    pass after phaseA.
13. LIKELY DONE 2026-08-11 (Kale) — dissolved as an instrument
    artifact, pending one confirming datapoint. The 49x loadCache
    anti-scaling claim (0.022 -> 1.073 s over 8 -> 128 Frontier nodes)
    came from the framework print formerly labeled "TreeCanopy cache
    loading", which in fact brackets the ENTIRE app preTraversalFn —
    all of phase 1 included (caught by Kale; the 2B/128-process run
    decomposed its 5.56 s print as 5.17 phase1 + 0.12 encode + 0.35
    upwardPass + 0.028 TRUE cache load). The print label is fixed on
    main. CLOSE for good when any future multi-node run.s FOF3STAT
    loadCache field confirms the true bracket stays flat with node
    count (rides along free on the next sweep; no dedicated run). The Anvil 80M tables show the
    same shape in miniature (0.002 -> 0.048 s over 1 -> 16 nodes).
14. Load-balancing benefit study (GreedyRefine path is validated;
   deferred by Kale until "much later").
15. htram: OFF by default since 2026-08-05 (build-stack.sh); revisit
    only as an explicit study, e.g. >16-node dist campaigns.
16. ChaNGa integration: waiting on Kale's go.
