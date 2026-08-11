# Agenda: future work items

Standing list of agreed future work, in rough priority order. Each item
points at its design note where one exists. Started 2026-08-05.

1. **Extend paratreet2 to host FoF phase 1** (design/walk-unification.md;
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
4. Parallelize the phaseB pool build across the process's threads. It
   is currently a serial per-process enumeration and LPT sort performed
   by the last phaseA depositor while the other 14 threads wait, and it
   is what blocks finer unit splitting: at 80M/4 nodes,
   FOF_POOL_SPLIT_SIZE=6 cuts the largest unit 0.063 -> 0.039 s and the
   slowest thread 0.063 -> 0.053 s, but the stage still regresses
   0.067 -> 0.121 s because the build now enumerates 2.34M units instead
   of 199k. The enumeration is embarrassingly parallel over TreePiece
   pairs; the sort can be per-thread with a merge. Once it is parallel,
   size-based splitting should be turned on by default and phaseB should
   scale from one node instead of waiting until eight.
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
   REMAINING: the Anvil measurement — relabel(p3) at 2B/16 nodes
   against the 2.84 s baseline, plus phase1 relabel/tip_encode at 80M
   (rides the next measurement round; append to the design note).
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
   significant effect on InfiniBand (charm-notes), so reconcile the
   two before adopting a setting.
10. DONE 2026-08-06 (freeze-pass counting commit): eliminated the component counter's particle
   pass (Kale's design, 2026-08-06). Count per union-find root during
   the phaseA freeze pass (dense array increment beside the existing
   find()); carry the per-processor tip-count map through every label
   rewrite the particles undergo; depositLabelCounts then deposits the
   map instead of re-counting all particles. Removes the 60 ms band at
   80M (projected ~300 ms per processor at 2B); debug flag keeps the
   old particle loop as a cross-check.
11. Distributed union-find mode: batch the component-labeling requests
   per destination chare. The labeling scatter (boss_count_prefix_done ->
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
13. loadCache anti-scaling (Ritvik's Frontier 2B sweep, design/
    fof3-2b-scaling.md): the starter-pack load grows ~49x from 8 to 128
    nodes (0.022 -> 1.073 s) as pack size tracks TreePiece count — batch
    or coarsen the starter-pack shipment. The Anvil 80M tables show the
    same shape in miniature (0.002 -> 0.048 s over 1 -> 16 nodes).
14. Load-balancing benefit study (GreedyRefine path is validated;
   deferred by Kale until "much later").
15. htram: OFF by default since 2026-08-05 (build-stack.sh); revisit
    only as an explicit study, e.g. >16-node dist campaigns.
16. ChaNGa integration: waiting on Kale's go.
