# Phase-B cross-process offload: steal-based work shipping

**STATUS: DESIGN FOR DISCUSSION (2026-08-04, from a working conversation
with Kale). No implementation yet. Target: the cross-process phase-B
floor — at 2B the heaviest process carries ~20x the average phase-B
work, and the process-local pool cannot move work across process
boundaries (design/phase1-scaling.md, the 2B milestone entry).**

## 1. The enabling invariant (verified against the walker, 2026-08-04)

A phase-B pool unit is a PURE FUNCTION over frozen data:

    (subtree A snapshot, subtree B snapshot, b^2)  ->  set of (PE-tip, PE-tip) edges

- Everything a unit reads is frozen at the phaseA barrier: the two
  subtrees' node structure (boxes, children) and their leaf particles'
  position + group_number. phaseA's freeze-and-compress writes the
  PE-tips INTO the particles precisely so phase B never consults the
  union-find.
- The executor-side dedup set and certificate memos are local
  accelerators, not shared state; duplicate edges across executors are
  idempotent at the merge.
- The returned edges are PE-tips of the ORIGIN process — valid in its
  namespace no matter where the unit executed. A helper needs no
  translation and keeps no state.

So a unit can execute on any PE of any process on any node, at the cost
of shipping the two subtree snapshots.

## 2. Assets that already exist

- **Wire form**: flat_subtree / MultiData is exactly the needed snapshot
  (nodes + particles); with FoFCachedParticle slimming a shipped subtree
  is ~20 bytes/particle (~580 KB at 2B chare sizes of ~29k particles).
- **Ship-once, use-many**: pool units are 8x8 (and gap-gated depth-2)
  fragments of subtree pairs, so many units share a parent subtree. A
  helper that holds a subtree can execute every unit referencing it.
- **Completion machinery**: the phase-1 chain's atomic deposit counter
  extends naturally to "expected returns" bookkeeping; the merge is
  already idempotent.
- **A remaining-work metric that is honest, not predicted**: the pool is
  LPT-sorted with a cost key per unit, and the claim cursor position is
  the exact remaining-unit count — so "how much phase-B work remains on
  process P" is a two-number read (units left, sum of their keys), not a
  model.

## 3. Why STEAL-based, not prediction-pushed (Kale, 2026-08-04)

Load prediction here has a measured credibility problem: the density
predictor's correlation with actual phase times fell from 0.90 (laptop)
to 0.26-0.28 at 480 PEs and ~0 at 1920 — the certificate and
suppression machinery absorbs exactly the work the predictor counts
(design/phase1-scaling.md). A push scheme built on predicted load would
ship the wrong pairs. Stealing reacts to observed progress instead:
victims do nothing until an idle helper asks.

Scheme sketch:

1. **Victims keep their pools.** Nothing changes for a process whose
   pool drains before anyone asks.
2. **Idle processes request work** once their own pool is empty (they
   know phase B is still open process-wide from the chain state).
3. **Affinity re-steals**: the victim records which subtrees each helper
   has already received. A repeat steal request from that helper is
   served preferentially with units over subtrees it already holds —
   zero incremental shipping. First steals ship the snapshot; later
   steals ride on it.
4. **Priority tiers for victim selection**:
   - Same PHYSICAL NODE first: co-located processes can exchange
     snapshots through shared-memory-class transport, making the
     shipping cost near-zero. (To verify on reconverse/LCI: what the
     on-node inter-process path actually is and its bandwidth — the
     lci-handover intranode measurements are the starting point. On
     classic Converse this is the pxshm-class path.)
   - Then BUDDY physical nodes, not random victims: pre-wire each
     physical node to a small fixed set of buddies using the
     low-diameter c-regular random graphs from the seed-balancing
     project (bounded degree, small diameter, no hot spots; Kale to
     link the seed-balancing notes — see ~/software/seedbalancing/
     SEEDLB_DESIGN.md when imported).
5. **A physical-node-level monitor** to drive targeting: one agent per
   physical node tracking, for its processes, (a) who has finished
   phase B and (b) the remaining-work metric from section 2. Steal
   requests consult the local monitor first (tier 1), then the buddy
   nodes' monitors (tier 2). This keeps steal-request traffic off the
   processes doing the work and gives requests a target instead of a
   broadcast.

## 4. Open questions (for the review discussion)

- **Termination**: phase-B completion is currently "every PE deposited
  once". With steals in flight it becomes deposits + expected returns;
  a steal request racing pool-drain needs a clean deny path. The chain's
  counters extend, but the protocol needs writing down.
- **Steal-worthiness threshold**: a steal pays when (remaining cost at
  victim / its drain rate) exceeds (ship time + unit walk time at the
  helper). With tier-1 shipping near-free, same-node steals can be
  nearly unconditional; cross-node steals should require a minimum
  remaining-cost bar.
- **Victim NIC serialization**: the heavy process ships tens of MB
  total; milliseconds on InfiniBand but serialized on its NIC while it
  also walks its own units. Same-node tier avoids the NIC entirely.
- **When to build it**: the floor this attacks is ~46-50 ms at 4 nodes
  (80M) but seconds at 2B on the heavy process. The 2B runs are the
  justification; measure the post-keep-alive 2B phase-B spread first.

## 5. Staged plan (each stage separately measurable)

1. Instrument and publish the remaining-work metric + finished-set on a
   node-level monitor group; no stealing yet — just visibility (also
   directly useful in traces).
2. Same-node steals through the existing MultiData path, unconditional
   when the victim's remaining cost is nonzero and a same-node helper is
   idle. Measure at 2B: the heavy-process phase-B wall against the
   ~20x-average baseline.
3. Buddy-node tier with the c-regular random-graph wiring + the
   steal-worthiness bar.
4. Affinity re-steals (subtree-tracking on the victim).

## 6. Measured (2026-08-06, 2B on 16 Anvil nodes; three protocol rounds)

Stage 1-2 are IMPLEMENTED on main (same-machine tier). History, each
round one interleaved off/on job, correctness bit-exact in every run:

- v1 (single serving processor, 4 units per grant): heavy process
  shipped 260 of ~1,150 units; phaseB wall unmoved at ~3.1 s. Grant
  throughput was the bottleneck, and the heavy process's pool exists
  only after its own (also-slowest) dense phase.
- v2 (grants from all 15 of the victim's processors, 16 units per
  grant, pipelined helpers): shipped units tripled (848-1,072) but
  scattered across every victim — round-robin helpers steal from
  whoever has a pool, so self-sufficient victims absorbed the grants.
  Wall still ~3.1 s. Targeting, not throughput, is the constraint.
- v3 (need-gated serving + persistent helpers, commit 23f0248): a
  victim grants only when its remaining pool exceeds two rounds of its
  own local claiming; light victims deny instantly (kept on the
  helper's list for later passes), so helpers converge on the heavy
  process without any global knowledge. At-scale verdict pending
  (queued); the floor remains the largest indivisible unit (~0.68 s)
  and the heavy process's dense-phase skew (~3x) is the next item
  behind it.

## 7. Running the steals elsewhere (for Ritvik; larger machines)

Everything is on main. Build with clusterfinding/build-stack.sh
(aggregation off is the default). Environment knobs:

- FOF_STEAL=0 disables (default on).
- FOF_STEAL_GROUP = number of consecutive processes per physical
  machine — MUST match the machine's tasks-per-node (8 on Anvil
  wholenode; set it for the target machine or the "same machine" tier
  steals across machines).
- FOF_STEAL_K = units per grant (default 16).
- FOF_STEAL_TEST=1 (debug): odd processes skip local claiming so their
  pools drain only through steals — end-to-end forcing.
- FOF_COUNT_VERIFY=1 (debug): recompute component counts from the
  particles and abort on divergence.
- FOF_WALK_QD=1: quiescence detection instead of the credit counter on
  the serial-mode walk (the A/B oracle).

Readout lines: "FOF3STAT steal: process P out U in V denials D" per
involved process at its merge; "FOF3STAT balance: phaseB_s min/avg/max"
for the wall; the components line is the correctness gate (80M
lambb.00500: 23,707,197; 2B cosmo25cmb: 424,897,832).
