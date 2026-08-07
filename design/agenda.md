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
3. Steal-based phaseB cross-process offload (design/phaseb-offload.md),
   built against the framework pool from item 1.
4. Decomposition anti-scaling at 16 nodes (80M: 0.78 s at 8 nodes ->
   1.65 s at 16; design/speedup-campaign-2026-08-05.md follow-up 2):
   profile splitter computation and particle flush at 1920 PEs.
5. -u serial as the production default; retire the keep-alive ring
   (the serial bracket is quiescence-free and stall-immune; the ring
   measurably does not suppress the stall in the dist pattern).
6. Slim the serial-mode relabel broadcast to per-process map slices
   (2.84 s at 2B/16 nodes for the full-map broadcast).
7. Rename Subtree -> TreePiece (code + documentation dedicated pass,
   before or during report/paper writing; "subtree" collides with the
   generic word in prose).
8. LCI items for the handover: packet-pool exhaustion at 2B on 4-8
   nodes (refill_recvs deadlock alerts then poll_comp_impl assert
   during the input flush); the idle-stall itself (dist uf2 3.7 s at
   80M/16 nodes with the keep-alive ring on).
9. DONE 2026-08-06 (freeze-pass counting commit): eliminated the component counter's particle
   pass (Kale's design, 2026-08-06). Count per union-find root during
   the phaseA freeze pass (dense array increment beside the existing
   find()); carry the per-processor tip-count map through every label
   rewrite the particles undergo; depositLabelCounts then deposits the
   map instead of re-counting all particles. Removes the 60 ms band at
   80M (projected ~300 ms per processor at 2B); debug flag keeps the
   old particle loop as a cross-check.
10. Distributed union-find mode: batch the component-labeling requests
   per destination chare. The labeling scatter (boss_count_prefix_done ->
   insertDataNeedBoss, observed ~1400 sends from one chare at 2B) already
   deduplicates by parent through a cache; flushing per destination would
   collapse it to at most one message per peer chare. Dist mode remains
   important (Kale, 2026-08-05) and has been competitive — this and its
   quiescence-closed labeling phase are its main remaining fine-grained
   patterns.
11. loadCache anti-scaling (Ritvik's Frontier 2B sweep, design/
    fof3-2b-scaling.md): the starter-pack load grows ~49x from 8 to 128
    nodes (0.022 -> 1.073 s) as pack size tracks subtree count — batch
    or coarsen the starter-pack shipment. The Anvil 80M tables show the
    same shape in miniature (0.002 -> 0.048 s over 1 -> 16 nodes).
12. Load-balancing benefit study (GreedyRefine path is validated;
   deferred by Kale until "much later").
13. htram: OFF by default since 2026-08-05 (build-stack.sh); revisit
    only as an explicit study, e.g. >16-node dist campaigns.
14. ChaNGa integration: waiting on Kale's go.
