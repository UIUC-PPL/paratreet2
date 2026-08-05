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
2. Serial-mode credit-counter termination for the dual walk (replace
   the traversal's quiescence detection; design agreed 2026-08-05 —
   credit transfer across request/reply, parked-walker dispatch, and
   the pause protocol; dist keeps the combined walk+union quiescence).
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
9. Load-balancing benefit study (GreedyRefine path is validated;
   deferred by Kale until "much later").
10. htram: OFF by default since 2026-08-05 (build-stack.sh); revisit
    only as an explicit study, e.g. >16-node dist campaigns.
11. ChaNGa integration: waiting on Kale's go.
