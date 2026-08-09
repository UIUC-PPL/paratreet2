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

## 8. v3 verdict (2026-08-06 late, 2B on 16 Anvil nodes)

Correctness exact in all six runs. The need gate concentrated grants
(single victims shipped up to 496 units — 43 percent of a heavy-sized
pool — against v2's scatter), and the wall still did not move (off
3.12-3.47 s, on 3.17-3.26 s). The contradiction with drain arithmetic
is the finding: if that share of the heavy pool had really executed
elsewhere, the wall must fall. Conclusion: the 2-rounds-of-local-work
admission threshold is far too permissive — mid-weight victims with a
few hundred units still grant freely, and the top shipper was likely
not the wall-owning process at all.

v4 requirements (the original stage-1 design, no longer optional):
1. Publish the remaining-work metric (pool size minus cursor, and its
   cost sum) so helpers target the MAXIMUM, instead of gating admission
   locally at each victim.
2. Per-process instrumentation first: one line per process at merge
   (pool size, phaseB stage wall, units granted) so the heavy process
   and its actual grant share are identified from one run, not
   inferred. The three nulls so far were each diagnosed indirectly;
   this line ends that.
3. Re-examine the admission threshold in time units (projected
   remaining seconds at the victim's own drain rate), not unit counts.

## 9. v4 refinements (Kale, 2026-08-07)

1. Load estimates are RANKINGS, not gates. The remaining-cost prefix
   sum and the observed drain rate rank victims for helpers; prediction
   error there only sends help to the second-neediest. The deny gate is
   set so low it almost never fires (remaining time below ~50 ms —
   genuinely trivial tails only). Err in favor of granting: an
   unnecessary grant wastes milliseconds of shipment, a wrong denial
   preserves a multi-second wall. (History: the density predictor's
   correlation with real phase times collapsed at scale; no admission
   decision may depend on prediction accuracy.)
2. Steal overhead is small and mostly measured: on-machine transport
   ~15 GB/s / 1.3 us latency (lci_multipeer sweep) puts a multi-
   megabyte batch at ~1 ms wire time; flatten and rebuild are
   memory-copy passes estimated at a few ms — the v4 accounting must
   time them so the estimate becomes a measurement. The helper's walk
   time is moved work, not overhead.
3. No output inside timed regions: per-process accounting (pool size
   and cost, wall, granted/received, flatten/ship timings) is stored on
   the branch and delivered by one concat reduction at the end of
   phase 1, printed by the driver after the phase. The current
   merge-time steal print is replaced by this mechanism.

Amendments (Kale, 2026-08-07, second round):
- The admission gate is a UNIT-COUNT floor only (remaining units above
  roughly one batch), given the measured milliseconds-scale steal
  overhead. No time projection in the gate; projection survives only in
  the helper-side ranking, where error is harmless.
- Donors serve any number of helpers concurrently: every grant is an
  independent atomic claim on the shared cursor, served in parallel by
  whichever donor processor received the request. No helper selection
  or binding exists on the donor side; convergence of many helpers on
  the neediest donor is the intended many-to-one shape, on the helper
  side only.

## 10. v4 implemented (2026-08-07, commit 44edf47)

Scheme as specified in sections 8-9, with the grant gate at MORE THAN
TWO BATCHES remaining (Kale). Helpers probe every machine-mate, target
the maximum remaining, take a batch, re-probe; donors serve any number
of helpers concurrently via independent atomic claims.

Two protocol lessons from bringing it up, both of which cost a hang:

1. ONE AUTHORITY PER DECISION. The first draft filtered victims on the
   helper side using its own copy of the two-batch threshold, so a
   donor with a small remainder was never asked — and the donor is the
   only party that knows whether its own threads are still draining
   that remainder. Result: a stranded tail and a phase that never
   merged. Helpers now rank; donors decide. The donor also skips its
   own gate once its threads have deposited, because only helpers can
   drain what is left at that point.
2. PACE EVERY RETRY. An immediate re-probe after a denial turned
   helper and donor into a deny/probe message storm — the machine ran
   at full load with no forward progress. Both retry paths (not-ready,
   not-needy) are timer-paced at 1-2 ms.

Measured by the new accounting, answering section 9.2's open item:
flattening costs about 0.09 ms per unit (110.6 ms for 1178 units), so
a 16-unit batch costs ~1.5 ms to prepare and ~1 ms on the wire against
~0.6 s of moved walk work. Steal overhead is not a factor at any
threshold we would plausibly choose.

Accounting line (printed after the phase, one per participating
process): FOF3STAT stealacct: process P pool U wallB S out X in Y
denials D flatten_ms F.

## 11. Round 4 (v4) verdict: the donor's shipping cost is the ceiling

2B, 16 Anvil nodes, three interleaved pairs. Correctness exact. Wall
unmoved again (off 3.11/3.11/3.15 s, on 3.15/3.17/3.15 s) — but the
new per-process accounting finally shows WHY, and it is not targeting,
throughput, or timing:

    donor flatten cost, measured: 1.27 - 5.64 ms per unit shipped
    donor execution cost, derived: ~0.33 ms/unit (light process:
      8596 units, 0.19 s wall, 15 threads)
                          ~5.2 ms/unit (heavy process: ~9000 units,
      3.1 s wall, 15 threads)

Shipping a unit costs the donor as much as executing it (heavy), or
more than ten times as much (light). The donor is the serializing
resource: it cannot shed load faster than it can drain it, so no
number of helpers can move the wall. Every previous round's null
follows from this, and the protocol work (targeting, batching,
persistence) was necessary but could never have been sufficient.

Root cause in the implementation: units are flattened INDIVIDUALLY at
grant time. Since pool units are 8x8 (and depth-2) fragments of
subtree pairs, one child subtree appears in up to eight units and is
re-flattened every time — precisely the waste section 2 named
("ship-once, use-many") and this implementation never took.

v5: memoize flattened subtrees on the donor, keyed by node, and ship
deduplicated blobs plus per-unit index pairs. Expected: up to 8x fewer
flattens within one shipment, and free re-use across grants to
different helpers, taking the donor's per-unit shipping cost well
below its per-unit execution cost — which is the precondition for any
of the protocol work to matter.

## 12. Round 8 verdict: the wall is indivisible work, not misplaced work

2B, 16 nodes, direct-ask helpers (no probe phase). Helpers now issue
requests in volume — 230,094 job-wide against zero in round 7 — and the
denial breakdown ends the investigation:

    not-ready 216,633 (94%) | drained 13,440 | not-needy 0 | grants 21

Two facts, both from the same run:

1. Helpers spend almost the entire window asking a process whose pool
   does not exist yet. A pool can only be built after its own process
   finishes phaseA, and phaseA is itself skewed (0.395 / 0.990 / 2.427 s
   min/avg/max), so the process that will own the phaseB wall is often
   still in phaseA while its would-be helpers are already idle.
2. When the pool finally appears, the process's own 15 threads claim
   every unit within roughly 200 ms — mean unit cost is 0.26 ms — and
   after that there is nothing left to give away, even though 2.5 s of
   work is still in flight. The stage runs 2.735 s because a few threads
   are grinding units up to 0.58 s each that they already own.

Claiming is not doing. Work becomes unstealable the instant it is
claimed, and claiming runs orders of magnitude faster than execution.

So the phaseB wall at 2B is the same phenomenon measured at 80M: a
handful of units that no scheme can divide after the fact. Stealing
cannot fix indivisible work. The lever is making units divisible
(design/agenda.md items 4 and the FOF_POOL_SPLIT_SIZE rule), which the
parallel pool build now makes affordable. Stealing remains correct,
cheap, and useful once units are fine enough to be worth moving — it is
no longer the primary lever.

## 13. Connecting the steal protocol to the two-queue deque, and the four
## correctness defects that surfaced (2026-08-08)

The two-queue scheme (section 12's conclusion: make units divisible)
replaced the pre-enumerated pool with a shared deque that threads refine
on demand. The steal protocol still pointed at the old pool, so with the
new scheme switched on no work could move between processes at all —
Kale: "I thought the whole point of 2B jobs is the steal across
processes." Connecting it needed four changes on top of the transport
that already existed:

- `requestSteal` serves whole units from the BACK of the deque, where
  the coarsest unrefined pairs sit, and keeps one unit per local thread.
- `receiveSteal` rebuilds the shipped subtrees, keeps them alive for the
  phase, pushes the units into the local deque tagged with the owning
  process, and wakes every processor of this process.
- The helper returns the edges to the owner, which absorbs them and
  settles its account.
- Termination is steal-aware: a processor with nothing to do polls
  instead of finishing, until every mate reports it has nothing to give.

Four defects showed up in validation, in this order. All four are the
same shape: work or edges belonging to one process being handled as if
they belonged to another, or being counted twice.

1. **Circular wait (8M hung).** The helper returned borrowed edges only
   when it finished its own phaseB, but it does not finish while it is
   still polling for more work, and the owner cannot merge until those
   edges arrive. Fixed by counting, per origin, how much derived work is
   still queued or in a thread's hand: when that count reaches zero the
   edges go home immediately, whatever the helper does next. Split
   children inherit the origin and raise its count, so a unit that
   becomes eight is still settled exactly once.

2. **Edges left behind on sibling processors (126 components too many at
   1M).** Each processor buffered borrowed edges privately, so whichever
   processor happened to see the origin's count reach zero reported only
   its own. Fixed by moving the buffer to the process level: a processor
   deposits its edges there BEFORE decrementing the count, under the same
   lock, so seeing zero means seeing everything.

3. **Borrowed work passed on to a third process.** The steal server took
   units from the deque without checking their origin, so a process could
   hand on units it had itself borrowed. Their edges would then be
   returned to the intermediary rather than to the process whose
   particles they describe, and that process would never learn the work
   was done. The server now serves only its own units.

4. **The certificate path wrote to the wrong buffer.** When a node pair
   passes the positive certificate test, `emitSubtreeTips` emits a star
   of edges through a representative tip. It wrote into the executing
   processor's own edge buffer unconditionally. For a borrowed unit those
   edges name the owner's particles, so they were submitted to a union-
   find that has no such tips. The destination is now a parameter that
   follows the unit's origin.

5. **A processor depositing its phaseB result twice.** This was the last
   one and the hardest to see, because the symptom was identical to the
   others: a handful of missing merges, varying run to run. A processor
   can be sent into the work loop from three places at once — its own
   slice hand-off, its poll timer, and the wake that follows an arriving
   shipment. Two of those could reach the finish path, and the second
   deposit let the process merge while another processor's edges were
   still unsubmitted; those edges were then dropped. The deposit is now
   once per processor.

What found each one matters for the next person. Defects 1 and 2 came
from reasoning about the protocol. Defect 3 came from asking what a
borrowed unit means when it is stolen again. Defect 4 and 5 needed
measurement: the existing self-test (`FOF_STEAL_SELFTEST=1`, which
compares every unit's direct walk against the full flatten, serialize,
rebuild and walk replica) passed on every unit, which ruled out the
transport and the rebuilt-tree walk and pointed at the plumbing around
them. A leftover-work check at merge time — queued units, unsettled
borrowed work, undelivered edges — ruled out stranded work as well.

The validation mode was extended to the two-queue path
(`FOF_STEAL_TEST=1`: odd processes run nothing of their own, so their
work can only leave through a steal). It needed one adjustment: a process
that refuses its own work can sit idle holding a full queue, which the
real scheme never does, and it then deposited with work undone. The
restriction is now released at the point where the processor would
otherwise finish.

Local validation after the fixes, netlrts SMP, 2 processes of 2 threads:

| configuration | runs | result |
|---|---|---|
| 1M legacy pool, full verification | 2 | 333889, passed |
| 1M two-queue, no stealing, full verification | 2 | 333889, passed |
| 1M two-queue, stealing, full verification | 3 | 333889, passed |
| 1M forced steal, full verification | 4 | 333889, passed |
| 8M two-queue, no stealing | 3 | 7865983 |
| 8M two-queue, stealing | 6 | 7865983 |
| 8M forced steal | 3 | 7865983 |

Before the fixes the 8M stealing arm was wrong in all six runs, by one
to eleven components, and never wrong in the same way twice.

One measurement to keep in view: at 8M the stealing arm spends about 1.2
seconds in phaseB against 0.004 seconds without it, on a stage that has
almost no work to move. That is the polling cost of a process waiting to
be told there is nothing left. It does not matter at 2B, where phaseB is
several seconds, but it rules stealing out as a default at small scales.

## 14. Two more defects, found only at scale, and the first measurement
## where work actually moves (2026-08-08)

The five defects in section 13 were all found on the laptop with two
processes. Two more appeared only on Anvil, and both were liveness, not
correctness. The 2B run on 16 nodes produced no stage output in eleven
minutes where the same binary without movement finished in fifty seconds.
Reproducing it cheaply mattered: 80M on 4 nodes showed the same stall in
under two minutes, and everything below was diagnosed there.

6. **A request in flight when the asking process runs out of mates.** The
   trace made it plain: 24 shipments granted, 24 received, 21 returned.
   Three helpers took work and never reported, and their owners waited
   forever with `outstanding 16`. The order of events on the helper was
   its own phaseB completing, and only then the shipment arriving. Its
   processors had already deposited, and the guard added for defect 5
   made them ignore the wake, so the units sat in the queue.

   The fix is a semantic one rather than another guard. A processor that
   has deposited may still run BORROWED work: those edges go to the
   owner's buffer, so running them changes nothing about a result this
   process has already submitted. What it must not do is deposit a second
   time, or run one of its OWN units, whose edges are already gone. With
   that, a late shipment is simply work, and the whole class of "the
   request arrived after we stopped listening" disappears. Refusing late
   work instead would have needed the owner to take it back, which means
   the owner keeping every shipped unit in case it returns.

7. **Every processor polling.** While waiting for an answer, all fifteen
   processors of a process ran a half-millisecond timer, which is fifteen
   times the timer and message traffic to learn one process's worth of
   information. Only the processor that asks now polls; the others stop
   and are woken either by the arrival of work or by the asker once it
   learns there is no more to be had. This did not fix the stall — defect
   6 did — and it did not measurably change the phaseB time either.

### Measurement: 2B, 16 nodes, 128 processes, 1920 processors

Component count exact (424,897,832) in every run, no leftover-work
reports, and units out equals units in on every process.

| arm | phaseB, two runs | units moved | grants |
|---|---|---|---|
| threshold 250k, no movement | 3.164, 3.423 | — | — |
| threshold 250k, movement | 2.636, 2.071 | 2320, 2527 | 145, 158 |
| threshold 25M, no movement | 3.052, 3.050 | — | — |
| threshold 25M, movement | 3.292, 2.500 | 2145, 2429 | 135, 154 |
| threshold 250k, movement, group 32 | 2.157 | 2972 | 186 |

Work moves, and at the 250k threshold it buys roughly a third off the
phaseB wall. The 25M threshold gains nothing reliable: its units are too
coarse for the amount that can be shifted to matter. Widening the group
of processes a request may reach from 8 to 32 moved the most work of any
arm and gave the single best phaseB time.

Two cautions on reading this table. Run-to-run variation is large —
phaseA ranged 1.75 to 3.11 s across these same runs — so a 30 percent
difference is not settled by two runs, which is why a four-repetition
round follows. And the per-process phaseB timer now includes the time a
process spends waiting to be given work, so the average over processes
(0.58 to 0.70 s against a 2 to 3 s maximum) is no longer a measure of
work; only the maximum is comparable to the earlier no-movement numbers.

### Four repetitions each (2B, 16 nodes), job 19748037

phaseB seconds, exact component count in all twelve runs:

| arm | rep1 | rep2 | rep3 | rep4 | mean | units moved |
|---|---|---|---|---|---|---|
| 250k, no movement | 4.156 | 3.372 | 3.168 | 4.213 | 3.73 | — |
| 250k, movement, group 8 | 2.284 | 2.232 | 2.417 | 2.823 | 2.44 | 2384-2552 |
| 250k, movement, group 32 | 2.010 | 2.709 | 2.640 | 3.681 | 2.76 | 2784-2983 |

Movement is worth about 35 percent of the phaseB wall at 2B on 16 nodes:
3.73 s down to 2.44 s. The four repetitions matter — the no-movement arm
alone spans 3.17 to 4.21 s, so the two-run round in section 14 could not
have separated these.

Widening the group to 32 moves more work and produced the single fastest
run (2.010 s) but the worst mean, and its spread is the largest of the
three arms. Reaching further costs more to reach: a request to a process
on another node is a longer round trip, and the shipment that answers it
is larger. Group 8 — the processes sharing a node — is the better default
on this machine; group 32 is worth revisiting only if a cost model
decides per request how far to reach.

What movement does NOT do is make phaseB scale. The average process still
spends 0.6 s where the busiest spends 2.4 s, so roughly three quarters of
the imbalance survives. The limit now is that a process cannot give away
what it has not yet reached: it seeds its queue with subtree-root pairs
and refines them on demand, so early in the stage the coarse units are
few and enormous, and by the time refinement has produced many movable
units the stage is nearly over. Making the busiest process refine ahead
of its own consumption, rather than on demand, is the next lever.

## 15. Making units earlier: three changes, all of which cost time
## (2026-08-08, Kale's proposals)

The section 14 measurement left one explanation for the surviving
imbalance: a process cannot give away what it has not yet reached. It
seeds its queue with subtree-root pairs and opens them only as its own
threads consume them, so early in the stage the only units in the queue
are a handful of enormous ones. Two changes were proposed to fix that,
and a third followed from a question about them:

- **Seed refinement** (`FOF_SEED_SPLIT`): open every seeded pair once
  when the queue is built, so the stage starts with many more units.
- **Division on request** (`FOF_STEAL_REFINE`): when a request arrives,
  open one unit at the coarse end before answering and hand over one of
  its parts, so answering leaves this process holding MORE chunks than
  before, not fewer.
- **Cost ordering** (`FOF_SEED_SORT`): order the refined seeds by the
  particle-pair estimate, largest at the end requests are served from.
  Asked for after noticing the deque was in enumeration order with the
  key field unused — the per-thread cost sort that exists sorts the
  legacy pool, which the two-queue scheme does not use.

2B, 16 nodes, three repetitions each, all correct (424,897,832):

| arm | phaseB mean | starting units per process | units moved |
|---|---|---|---|
| all three off (control) | 2.55 | 1570 | 2326-2479 |
| seed refinement | 2.77 | 3520 | 4852-5104 |
| seed refinement + cost ordering | 3.65 | 3520 | 1152-1216 |
| division on request | 3.23 | 1570 | 15372-20301 |
| all three | 3.94 | 3520 | 15147-19082 |

Every one of them costs time. The control reproduces the earlier 2.44 s
mean, so the binary itself did not change anything.

The mechanisms did work as designed — that is what makes the result
informative rather than a bug. Seed refinement did produce more units
(1570 to 3520 per process, a bit over twice rather than eight times,
because most products of an opened pair are too far apart to link and
are dropped on the spot). Division on request did move far more work:
15,000 to 20,000 units against 2,400. The extra work moved simply did
not pay for what it cost to move it.

Two candidate reasons, being measured next:

1. Answering with ONE chunk costs a full round trip per unit. Moving
   20,000 units that way is 20,000 requests, each with a flatten, a
   shipment and a reply. Answering with 4 or 8 parts of the unit just
   divided would amortise that over one round trip.
2. Cost ordering was applied in the direction that helps movement and
   hurts local progress: largest units at the end requests are served
   from means this process's own threads take the SMALLEST first, which
   is the wrong order for finishing early and leaves the big units for
   the tail if nobody asks for them. It moved half as much work as the
   control while being the slowest single change, which fits that
   reading. The other direction is now selectable (`FOF_SEED_SORT=2`).

Until those are settled, all three default to on in the code but are
NOT part of the configuration that measured best. The best measured
configuration remains: two-queue on, movement on, threshold 250k,
requests limited to the eight processes sharing a node, and these three
switched off.

### The tuning round settles it: moving work is the cost (2026-08-08)

Both explanations for section 15's losses were measured, and neither
holds. 2B, 16 nodes, three repetitions:

| arm | phaseB mean | units moved | answers |
|---|---|---|---|
| control (all three off) | 2.48 | 2417-2549 | ~155 |
| division, 4 parts per answer | 3.49 | 43k-69k | 11k-17k |
| division, 8 parts per answer | 3.24 | 61k-68k | 7.6k-8.5k |
| cost order reversed | 3.26 | ~11000 | ~700 |

Eight parts per answer cut the round trips from twenty thousand to eight
thousand and still lost 0.8 s against the control. Reversing the order so
this process's own threads take the largest units first lost the same
amount. Across all seven arms measured this day the relationship is
monotone in the wrong direction: **every configuration that moved more
work was slower.** The control moves about 2,400 units and gains 1.2 s
against no movement at all; moving twenty times more loses 0.8 s.

So finding units to move is not the constraint, and neither is the number
of round trips. What costs is moving a unit: flattening its two subtrees,
shipping them, rebuilding them on the other side, and sending the edges
home. Section 11 reached the same conclusion about the donor's shipping
cost at a different operating point; this fixes it as the property that
governs the whole scheme.

The three switches therefore DEFAULT OFF, and the shipped configuration
is the one measured best: two-queue on, movement on, threshold 250k,
requests limited to the processes sharing a physical node.

What follows from it is a different lever. Threads inside one process
share the queue and ship nothing, so the way to need less movement is a
bigger process. The standing layout on Anvil is 8 processes of 15 threads
per node; 4 of 31 and 2 of 63 use the same cores while turning
cross-process movement into ordinary queue sharing. That comparison, with
and without movement at each layout, is job 19748939.

## 16. What actually makes moving work expensive (2026-08-08, Kale's question)

Kale asked whether the cost of sending work to another process is the
shipping itself, or the fact that the deduplication and pruning — both
hash maps — do not work for shipped units. The shipping half was already
measured: `steal_flatten_us` has been counting the donor's serialization
all along. Summed over the 128 processes of a 2B run on 16 nodes:

| arm | units shipped | flatten total | per unit | phaseB |
|---|---|---|---|---|
| coarse units, 16 per answer | 2384 | 5.13 s | 2.151 ms | 2.28 s |
| divided on request, 1 per answer | 15372 | 0.96 s | 0.062 ms | 3.17 s |
| divided on request, 8 per answer | 67992 | 1.71 s | 0.025 ms | 3.19 s |

The arms that LOSE time ship six to thirty times more units, at one
thirty-fifth to one eighty-fifth the serialization cost each, for less
total serialization. Section 15's conclusion — "the cost is moving a
unit" — was the wrong reading of the same experiments.

To answer the other half, every counter was split by whether the unit was
this process's own or borrowed. The first version of that instrumentation
put a shared atomic increment on the emit path, which runs 8.5 billion
times in a 2B run, and took phaseB from 2.4 s to 13-58 s; the counters
are now per processor and folded into the process record at the end of
the stage. With them cheap enough to trust:

| | own work | borrowed work |
|---|---|---|
| units | 7,367,472 | 139,807 |
| per unit | 36.1 us | 96.8 us |
| emissions per unit | 1055 | 3618 |
| per emission | 0.034 us | 0.027 us |
| certificate hit rate | 93.1% | 96.3% |
| duplicates suppressed | 100.0% | 100.0% |

Borrowed units take longer only because they are bigger: they come from
the coarse end of the queue and carry three and a half times the particle
pairs. Per pair examined they are marginally cheaper, and their
certificate memo hits MORE often, not less.

The one place the hypothesis does bite: the borrowed duplicate set was
erased whenever an origin's outstanding count reached zero, which with
one chunk per answer is after every unit, so borrowed work restarted
deduplication from empty over and over. Measured, that costs about ten
thousand extra edges shipped out of 1.09 million — a percent, not the
missing factor. It is fixed anyway (an edge already delivered never needs
delivering again), with the old behaviour behind FOF_DROP_SEEN.

Two things the same instrumentation showed that matter more:

**phaseB is 94 percent waiting.** The whole stage does about 280
core-seconds of work spread over 1,920 processors: 0.15 s if it were
perfectly balanced, against a 2.3 s wall.

**The walk emits 8.48 billion candidate edges to keep 1.09 million.**
The deduplication hash map absorbs 99.99 percent of them, which makes
that map, not shipping, the busiest structure in the stage. Consecutive
particle pairs in one leaf-leaf visit usually carry the same pair of
tips, so a one-entry check in front of the map now catches repeats
without hashing: 96 percent of duplicates at 1M, identical results. It
saves about 7 percent of aggregate walk time and does not move the wall,
which is consistent with the wall being waiting rather than work.

## 17. The grant rate, and why the division on request never ran (2026-08-09)

Almost nothing moves, and not for want of asking. In one 2B run,
585,694 requests produced 146 grants and 12,023 refusals for an empty
queue. Only 4 to 6 percent of the work is ever borrowed. A busy process
is not unwilling to share; at the moment a request arrives its queue is
below the reserve of one unit per thread, because its threads pull units
out as fast as refinement publishes them.

That reserve is also what made section 15's division on request look like
a bad idea. The dividing code sits INSIDE the loop the reserve guards, so
when the queue is short — which is nearly always — it never executes. The
one mechanism built to manufacture something to hand over could not run,
and what little it moved came from the rare moments the queue was long
enough, paying settlement overhead without ever doing its job.

Removing the reserve when dividing (dividing adds units, so holding back
is pointless) confirms the diagnosis and exposes the next problem.
2B, 16 nodes, three repetitions:

| arm | grants | work moved | phaseB |
|---|---|---|---|
| reserve 15 (the shipped default) | 137-146 | 4-6% | 2.10, 2.79, 3.99 |
| reserve 0 | 140-148 | 3-6% | 2.01, 2.12, 2.36 |
| divide on request, reserve 0 | 17303-22339 | 0.0% | 3.09, 3.09, 3.55 |
| divide, four parts per answer | 8793-11747 | 0.1% | 3.46, 4.98 |

Dividing raises the grant rate by a factor of 120, exactly as intended,
and moves no work at all: 24,464 units handed over with under a
microsecond of walking each. The units it manufactures are empty. Two
reasons, both fixable: the answer takes `coarse_q.back()` after pushing
the products, which is whichever child happened to be pushed last rather
than the largest; and opening one level of a pair yields up to eight
children of which most contain few particle pairs actually within the
linking length. Handing over the costliest product, rather than an
arbitrary one, is the obvious next thing to try.

Removing the reserve without dividing is a small win on its own (median
2.12 against 2.79) and is the better default of the two.

### A bigger process beats moving work between processes

Same 16 nodes, three layouts, movement on and off, with the layout set at
the allocation level so srun uses the idiom that gives healthy runs. (An
earlier attempt overrode -N and --cpus-per-task on the srun line and ran
phaseA at 20 to 47 s against the usual 2.4 s in every arm, including the
standing layout — a CPU binding effect that hit the memory-heavy first
pass and left phaseB alone. Those numbers are void.)

| layout | phaseB, no movement | phaseB, movement |
|---|---|---|
| 8 processes x 15 threads | 3.00, 2.98 | 2.24, 2.16 |
| 4 x 31 | 2.33, 2.77 | 3.16, 2.42 |
| 2 x 63 | 1.94, 1.78 | 1.67, 2.94 |

Two processes of 63 threads with NO movement at all (1.86 mean) beats
eight processes of 15 with movement (2.20). Threads inside a process
share the queue and ship nothing, so making the process bigger converts
cross-process movement into ordinary queue sharing and removes the
problem instead of solving it. Movement still helps at 8x15, where there
are eight separate queues per node; by 2x63 it is within noise.

This is the cheapest available lever and needs no new mechanism, only a
launch-layout change. What it costs elsewhere — cache duplication is per
process, so fewer processes means less of it, while decomposition and
phase 3 behave differently — has not been measured, and the whole
iteration, not phaseB alone, has to be the judge before the standing
configuration changes.
