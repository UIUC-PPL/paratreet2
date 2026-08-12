# The phaseA/B balancing program: hierarchy, stealing, and placement

Kale's design (2026-08-11) plus the agent audit of it against the code
and the phaseb-steal post-mortem (same day; every claim in the audit
carried file:line evidence in the session record). This note is the
design home for agenda item 4b. Companions:
design/phasea-reassignment.md (half 1 feasibility + skew data),
design/walk-unification.md (the framework lift this program's
interface requirements feed), design/cost-model-probe.md (the m2
predictor), origin/phaseb-steal:design/phaseb-{offload,handoff-*}.md
(the prior stealing implementation and its post-mortem).

## 1. Kale's design

Correction to earlier reasoning first: phaseB tree-pair tasks are NOT
pure functional — they use dedup tables and certificates (hope: the
certificates are now in the nodes and travel with stolen work).

1. Balance phaseA by TreePiece stealing (the light-reassignment
   scheme, design/phasea-reassignment.md).
2. Make phaseB HIERARCHICAL: partition the process's units KD-tree
   fashion (~32-64 partitions) by centroid with an approximate load
   estimate. Each partition first generates all its INTERNAL edges as
   one independent task — the stealable unit, shippable to other
   PROCESSES with dedup/certificate state. When all partition-internal
   tasks finish, a union-find over their edges updates tips one level
   up (open question resolved below: overwrite vs new field), giving
   fewer tips and better dedup for the final cross-partition phase,
   run as today via the nodegroup list — hoped to need no stealing.
   Both phases yield to the Charm scheduler periodically; possibly
   Charm's own queues later. Framing: the intermediate merge is AN
   ADDITIONAL PATH COMPRESSION in the middle of phaseB.
3. Later idea: if costs predict the overloaded process, MIGRATE a few
   TreePieces cross-process BEFORE phaseA begins, restoring them after
   if the surrounding algorithm (gravity/SPH iterations) needs it.

## 2. The correction, made precise (audit result)

What a phaseB task touches today: reads only frozen data (boxes,
structure, min/max frag annotations, particle position+group_number)
plus two run-global scalars; writes only per-PE side state (the SEEN
set, the edge buffer, the pointer-keyed cert_tip memo) and the
process-level edge list under the merge lock. The connectivity memo is
NOT consulted in phaseB (the prune callback is constant false; SEEN
plays that role).

- The uniformity certificate IS in the nodes (FragData min/max_frag,
  pupped) and therefore ships free with any flattened snapshot —
  Kale's hope confirmed. The cert_tip memo does not ship but is mostly
  bypassed post-annotation (uniform nodes answer before the map).
- CORRECTNESS requires shipping NOTHING beyond the two subtrees'
  nodes and slim particles: the task writes nothing shared; losing
  SEEN can only duplicate edges, never lose one (insert and emit are
  adjacent); duplicates are idempotent at the merge; a cold cert memo
  only re-emits duplicates. The steal branch's FOF_STEAL_SELFTEST
  verified exactly this end to end.
- PERFORMANCE shipping rules: rebuild SEEN per SHIPMENT (not per
  unit — the per-unit reset was measured at only ~1% extra edges, but
  shipment scope is strictly better); dense node indices
  (walk-unification stage 0) turn cert memos into shippable arrays
  for free; and **particle slimming is a PREREQUISITE, not an
  optimization** — the steal branch shipped full ~120 B Particles
  where the walk reads exactly FoFCachedParticle's 20 B; a 20-piece
  partition is ~62 MB full vs ~10 MB slim.
- Edge return: reuse the steal branch's solved protocol verbatim —
  destination-follows-origin on BOTH emit paths (leafLeafEmit AND
  emitSubtreeTips — the branch's defect 4 was missing the second),
  process-level foreign buffers, settle-by-outstanding-count, edges
  rejoin through the victim's ordinary submitEdges/merge door. Plus
  the two paid-for rules: serve only your OWN units (never re-lend),
  and deposit exactly once while staying willing to run late arrivals.

## 3. The tip question: OVERWRITE, under a three-part rule

The intermediate (L2) merge is mechanically just another
per-representative map application (stage-1 relabel machinery;
measured 0.08-0.14 s at 2B) — but the NODE ANNOTATIONS also go stale,
and stale annotations are not merely conservative: certTipRep/firstTip
can return dead tips. The overwrite is safe iff, as ONE step:
(1) rep_label, the particles (materializeLabels), and the node
annotations (annotateFrozenTips — already in the tree for exactly this
caller) are all rewritten together; (2) the L2 edges are RETAINED in
the process edge list so the final merge stays consistent for
collapsed names; (3) no post-freeze unite (already CkEnforced).
Estimated L2 barrier cost at 2B: ~0.15-0.3 s (one merge relabel + one
re-annotate pass) — the number the compression benefit must beat.
A separate partition-tip field is strictly worse: +8 B/particle
(~125 MB/process at 2B), doubled tip payload in every cache-shipped
node, and it only avoids rule (2), which is one line.
Non-interactions verified: the component counter is keyed by root
index and sums by label (duplicate labels already supported); the
sign convention is set only in phase 3; owner encoding is process-
scoped and runs after (one guard: a stolen partition's labels must
never be encoded on the helper — only edges return, so this cannot
happen, but comment it).

## 4. The path-compression framing, honestly assessed

Two mechanisms, very different sizes:
- SEEN-key collapse: worth ~nothing — SEEN already absorbs 99.99% of
  candidates (8.48e9 -> 1.09e6 edges at 2B).
- ANNOTATION collapse (more uniform nodes -> more O(1) certificates):
  the real mechanism — it is what took phaseB 3.36 -> 1.58 s when the
  annotation landed. Ceiling from measured data: the ENTIRE phaseB
  edge set collapses at most ~4.5% of a process's tips (~8,500 kept
  edges vs ~187,500 tips per process at 2B), and L2 delivers a subset.
  BUT the collapses concentrate exactly on the dense cross-PE
  boundaries where phaseB spends its time, so the WORK effect could
  far exceed the tip-count effect. Genuinely undetermined — settled by
  the section-8 double-run experiment before any protocol is built.

## 5. KD partitioning (audit corrections and resolution)

- Corrected counts: ~600 TreePieces/process and ~9,000 pool units/
  process at 2B (not 120-500 / 200k+; the 24x-units size rule is a
  recorded dead end).
- Partition the UNITS, not the pieces: the validated load estimate
  (m2 = rho_a*rho_b*V_int*Vb, 0.87 alone at 2B) is a PAIR property;
  poolPushInto already produces the objects with both boxes in hand;
  and the 21x skew lives in the unit-cost distribution (top 1% of
  pairs = 79.5% of time). KD on unit CENTROIDS is still spatial, so
  the edge subset stays coherent — balance and coherence are less in
  tension than they look.
- The KD build slots into buildPoolSlice between the per-slice sort
  and assembly (5-6 median-split levels over ~9,000 units — trivial).
  HARD PREREQUISITE: FragData::n_below (de21b74) on main — 4b half 2
  step (i); the KD load estimate and the LPT-key replacement are the
  same edit.
- Tail mapping at the 2B/128-process Frontier point: the 0.634 s pair
  lands inside one partition (good: it ships whole); available
  headroom = 2.812 -> 0.634 (~4.4x); going below the 0.634 floor needs
  m2-ranked tail splitting (never box-size splitting — the 24x lesson).
- **Structural finding, the most important in the audit: the pool
  build does not need phaseA.** poolPushInto reads only build-time
  geometry (boxes, counts, structure — never the annotations). The
  pool, its m2 costs, and the KD partition table can all be built
  RIGHT AFTER REGISTRATION — before or during phaseA.

## 6. Why stealing pays this time (vs the phaseb-steal post-mortem)

Each recorded failure cause, addressed or not:
- Grant rate / empty queues (585,694 requests -> 146 grants):
  structurally fixed — the whole task set pre-exists with costs;
  a grant is an atomic claim on a pre-built partition array.
- Claiming-is-not-doing (all units claimed in ~200 ms): fixed ONLY if
  partitions are claimed lazily, one at a time per thread — k=32-64
  leaves 17-49 partitions visible behind ~15 threads. The fix IS the
  coarse granularity.
- Work arrives too late (94% of denials = victim still in phaseA):
  fixed ONLY by the section-5 pool-build move — the partition table
  publishes while the victim is still in phaseA; helpers pre-claim and
  wait; shipment starts at freeze. Without the move this cause
  survives intact.
- Donor flatten cost (~1:1 vs execution): fixed by construction —
  partition-sized deduplicated shipments (StealShipment shape) at
  ~37 ns/particle ≈ 19 ms to move 0.6-1.2 core-seconds (30-60:1),
  ~200:1 with slimming.
- One-authority and pace-every-retry: preserved unchanged; the
  published table also removes blind probing.
- "Fewer, larger processes did better": not addressed — but the GPU
  geometry (8 processes/node) forecloses the 2x63 layout, and the 21x
  skew is measured AT 8/node; design against the Frontier point, not
  Anvil.
- Small-scale tax (8M: 1.2 s of pure polling): everything below a
  size gate stays off.

## 7. B2 without stealing: the hope is NOT justified at k=32-64

Measured anchor: today's 15 PE-groups of ~40 pieces already split
~39% internal / 61% cross BY TIME (A_cross 2.6-2.8 s vs B 4.1-4.6 s
per process at 80M). Surface-to-volume over a ~26-neighbor piece
lattice: at k=64/32/8/2 partitions per process the internal fraction
is ~0.29/0.40/0.58/0.72 — B2 carries 60-75% of phaseB at the proposed
k, and >=28% at ANY k (a process has only ~600 pieces to divide).
RESOLUTION — separate the two goals: keep k=32-64 for the STEALABLE
DECOMPOSITION (that argument is sound on its own), and make the
intermediate merge a separate, optional, independently gated feature.
If the double-run experiment shows a large compression benefit, add
it; if not, B2 is simply the remainder of the same task set and gets
the same stealing treatment as B1.

## 8. Point 3: pre-phaseA predictive migration (later idea, recorded)

Targets the residual CROSS-process phaseA factor (1.14-1.40 measured)
that shared-memory stealing cannot reach — which becomes the wall once
half 1 fixes within (1.84). Mechanics: TreePieces are migratable
chares with working pup (LB gate demonstrated cross-node migrations);
registration is the assignment hook, so a piece migrated BEFORE
registration is owned by its temporary process for the whole phase-1
window with no extra machinery. Prediction at process granularity is
a SUM over ~600 pieces of the per-piece self cost (R2 0.85-0.9), and
needs only to beat a 1.4x skew — easier than the per-PE ranking that
failed. The restore proviso is likely FREE: multi-iteration apps
re-home particles through the splitters at the next rebuild anyway,
and single-iteration fof3 needs no restore — VERIFY against both
rebuild paths (re-bucket and full re-sort) before relying on it.
Cost sanity: ~3-4 MB/piece; a few dozen pieces = tens of MB against
0.3-1 s of cross-process imbalance.

## 9. Scheduler yielding

- Recommended form: deadline-sliced phaseBBody re-entering by
  SELF-SEND (the steal branch's phaseBStep precedent: 2 ms default
  slice; the claim loop already reads CkWallTimer twice per unit, so
  the deadline test is free; nothing to stash — all intermediates are
  per-PE members and the cursor is shared). The recorded 2.778 s
  single-invocation drain is also a NETWORK-progress hole: a node
  whose PEs are all deep in phaseB polls no messages at all.
- The reverted walk-uf2 chunked drain does NOT condemn this: that
  slice had ~8 natural yield points already present and mid-traversal
  stash cost; phaseB has zero yield points and a unit-boundary cut
  with nothing to stash. The transferable lesson: gate the slice off
  when the stage is small, and A/B at both scales.
- CcdCallFnAfter: ONLY for the steal protocol's retry paths (paced,
  rank-0-polls-only — both paid-for rules), never for the work loop.
- Charm's own queues: do NOT build on them — no Converse queue use
  exists in this tree, seed balancers move chare seeds (reintroducing
  pup on the visitor, which walk-unification explicitly avoids), and
  reconverse's scheduler polls FIFO before priorities. The pool cursor
  + self-send slicing is the same mechanism without the exposure.

## 10. Interface requirement fed to walk-unification stage 1

One addition beyond the planned opaque GroupId, in on day one (a
retrofit would re-key the same two functions a third time):
PartitionId per unit via a client partitionFn (default = flat pool =
today's behavior, compile-time-identical gate arm); partition_ranges +
per-partition cost + one claim cursor PER PARTITION; claimUnit(p) for
local threads, claimPartition() for steal grants. With stage 1's
node-index units, a partition's shipment descriptor and its GPU
launch-batch descriptor are the SAME object — and a partition is
exactly the "batch leaf pairs per wavefront" answer to the MI250X
occupancy problem. Convergence note: the device path and the stolen-
partition path both want edge output as an unsorted vector,
sort-uniqued once at the boundary, not a live hash set.

## 11. Staged order with gates

- 0a. n_below (de21b74) to main; m2 computed and printed beside the
  existing key. Gate: bit-identical everywhere.
- 0b. KD DRY RUN: after the pool builds, compute the KD split for
  k in {8,32,64}; print per-partition cost distribution, internal/
  cross unit split, largest-unit placement. ~50 lines, no execution
  change. GATE G0: if k=32 cross fraction lands at the predicted
  60-70% (not below ~50%), drop the no-steal-in-B2 premise now.
- 0c. THE DOUBLE-RUN EXPERIMENT (the riskiest-assumption killer):
  under a flag, after phaseB completes: mergeBody + relabelBody +
  materializeLabels + annotateFrozenTips, then run phaseBBody AGAIN
  over the same pool and compare work counters. Zero correctness risk
  (retained edges + idempotent duplicates + FOF_COUNT_VERIFY);
  measures the UPPER BOUND of the compression benefit (a full merge
  collapses more than any L2 subset can) AND prices the section-3
  barrier in the same run. ~100 throwaway lines; run with 0b in one
  2B allocation. If pass 2 is not dramatically cheaper, stage 4 is
  cancelled outright.
- 1. Move the pool build off the phaseA critical path (right after
  registration). Highest value-per-line change in the program: it is
  the prerequisite for the 94%-of-denials fix and costs nothing.
  Gate: bit-identical; pool contents byte-identical; phaseA unchanged.
- 2. Scheduler yielding alone, A/B'd (FOF_PHASEB_SLICE_MS=0 off).
  Gate: bit-identical; phaseB not worse at 8M, 80M, 2B.
- 3. Partitioned claim + whole-partition stealing (slimmed
  StealShipment blobs, destination-follows-origin both emit paths,
  settle-by-count, helpers-rank/owner-decides, paced retries). Gate:
  the steal branch's full validation set + fof1 phase-1-exact (the
  re-key trap, now present at TWO level boundaries). Measure: phaseB
  max/avg at the 2B/128-process Frontier point vs 0.004/0.134/2.812,
  floor 0.634.
- 4. The intermediate merge — ONLY if 0c justifies it.
- (later) Point 3's pre-phaseA migration, gated on the cross factor
  becoming the wall after stage 3.

## 12. Measured: stage-0 probes at 80M (job 19789223, 2026-08-11)

All runs exact. Process-0 lines (627 pieces, 5,401 units, m2 total
1.5e7):

- GATE G0 PASSES AT 80M, against this note's own section-7 prediction:
  cross-partition fractions by UNIT COUNT follow the geometry
  (19/33/39% at k=8/32/64) but by M2 COST collapse to
  **0.1% / 14.4% / 25.2%** — the expensive units are interior to dense
  regions, which KD-by-piece keeps together; only cheap units straddle
  boundaries. Cost concentration works FOR the hierarchy. At k=32, B1
  holds ~86% of phaseB cost internally. (2B confirmation pending.)
- DOUBLE-RUN: pass2/pass1 median 0.011/0.016 s (~70%), MAX unchanged
  (0.041/0.041) — the compression upper bound does not touch the
  bottleneck process at 80M; merge cost ~1 ms. Stage 4 is not
  justified by 80M evidence; the 2B point decides.
- TAIL: maxunit m2 3.55e6 vs avg k=32 partition 4.7e5 — the largest
  unit is 7.5x an average partition and produces empty partitions
  around it in the weighted split. m2-ranked tail splitting is
  load-bearing regardless of every other outcome.
- Control rep phaseA_skew 1.43/1.16 matches the earlier 80M
  measurement (instrument stability).

## 13. m2 tail-ranking quality (offline, from the cost-probe records)

Question (Kale): does m2 support ADAPTIVE unit splitting, and was the
huge unit predictable? Measured from the probe record CSVs (process 0,
phaseB pairs):

- At 2B the largest-by-time unit (120.45 ms) is ALSO the largest by
  m2 — rank 1 of 9,014. Perfectly predictable. At 80M the most
  expensive unit ranks 47/5,401 by m2 (top 0.9%); the top-m2 unit is
  the 2nd most expensive.
- Capture curves (time captured taking units in m2-rank order, oracle
  in parens): 2B top 1% = 63.7% (79.5%), top 10% = 90.0% (93.6%);
  80M top 1% = 46.7% (60.2%), top 10% = 76.7% (84.5%). Ranking
  quality IMPROVES with scale.
- Adaptive-split rule: m2 > 8x mean flags ~1% of units (68/83 per
  process at 80M/2B) holding 48%/63% of the time — split only those
  (children re-estimated recursively from their own boxes and
  n_below) and the unit count grows by hundreds, not the 24x of the
  geometric rule, because the geometric rule split everything while
  m2 selects exactly the dense-core pairs. This is agenda 4b half 2
  step (iii) made concrete, and the KD dry run's empty-partition
  symptom (section 12) is what it repairs.

## 14. THE CAMPAIGN (Kale's sequence, vetted 2026-08-11; branch phaseab-campaign)

Kale's proposed sequence, with the vetting refinements folded in. Every
step: env-gated arms in ONE binary, laptop identity gates (16-run
matrix + fof1 phase-1-exact + 1M/8M/LAMBS counts under
FOF_COUNT_VERIFY + reconverse spot runs), then an Anvil A/B against
the step's predecessor. All Anvil measurement under the NEW pinned
configuration (machines/anvil.md pemap) — the pemap changed baselines,
so nothing compares across the affinity boundary.

Vetting notes on the proposal as given:

- Coordinator structure (Kale's clarification, 2026-08-11): the
  coordinator machinery IS a nodegroup — every process's branch is its
  own LOCAL coordinator (holds its status: claim-cursor position,
  remaining partition costs; answers polls; executes ship orders), and
  the DESIGNATED branch (lowest process of a FOF_PROCS_PER_PNODE
  block, default 8 — the physical-node domain comes from the
  environment, since a Charm SMP "node" is a process) plays the domain
  coordinator. Nodegroup entries are served by ANY free PE of the
  process — the responsiveness property the whole scheme rests on, and
  it composes with P1's slicing (slices create the free moments; the
  P0 probe already exercises this exact pathway). The coordinator's
  DECISION entries are [exclusive] (serialized per branch, no mutex,
  no two PEs computing conflicting placements); status deposits stay
  non-exclusive with the narrow lock. Status and stealing stay within
  the physical-node domain first, as proposed.
- Coordinator authority vs the one-authority rule (the steal branch's
  paid-for lesson): the coordinator owns the PLACEMENT decision (who
  sends to whom, from reported status); the donor's compliance is an
  atomic claim on its own pre-built partition array, so no second copy
  of the admission decision exists. Status flows by coordinator poll
  (paced) plus piggyback on other traffic later; never
  donor-broadcast-on-every-change.
- Responsiveness probe FIRST (Kale's main suspect, and the vetting
  agrees): a standing coordinator->process RTT probe measures exactly
  the thing every later step needs (can a process answer a message
  while its PEs drain phaseB?). Run it BEFORE the yielding step so the
  bad baseline is on record, then after. OBSERVER EFFECT to control
  once: background traffic is itself the measured suppressor of the
  LCI idle-stall (the 2026-08-04 finding), so take one baseline pair
  with the probe on vs off before trusting probe-on numbers.
- Transfer probe: reconverse has NO shared-memory transport between
  processes (the CMK_USE_SHMEM producer half is missing — charm-notes
  2026-07-26), so "is IPC being used" has a known answer: no; the
  probe CONFIRMS by latency signature (~2 us+ = NIC loopback) and
  prices shipment sizes on the real fabric.
- The B1/B2 barrier is PER-PROCESS (deposit-chain style), not global:
  tips are process-scoped, so the L2 merge needs only its own
  process's B1 edges (plus settled returns of stolen B1 work).
- The 2b sequential union-find = the L2 merge under the section-3
  three-part atomicity rule (rep_label + materializeLabels +
  annotateFrozenTips as one step; L2 edges RETAINED). Its benefit
  bound and barrier price come from tonight's 2B double-run (0c) —
  build it behind a flag either way; the flag's default follows 0c.
- Framework note: all of this lands APP-SIDE on the branch. The
  walk-unification stage-1 lift later absorbs the pool/claim machinery
  with the opaque group-id + partition-id interface; the campaign
  keeps the re-key discipline (pool enumerates cross-ASSIGNMENT
  pairs) so that lift stays mechanical. Do not lift mid-campaign.

Steps (P = instrument, S = scheme):

- P0. RESPONSIVENESS + TRANSFER PROBE (built first; FOF_PROBE=1).
  Coordinator pings every process of its physical-node domain every
  FOF_PROBE_MS (default 25) through phases 1-3; per-target RTT
  min/med/p99/max printed per iteration. At report time, a one-shot
  transfer ladder (4 KB / 64 KB / 1 MB / 8 MB) to the next process
  prices shipments. Gate: zero effect on counts; baseline pair with
  probe on/off.
- P1. SCHEDULER YIELDING (FOF_PHASEB_SLICE_MS, default off):
  phaseBBody becomes a sliced drain — deadline check per claimed unit
  against the wall clock it already reads, re-entry by SELF-SEND
  (design section 9; nothing to stash, cursor is shared). Probe
  numbers before/after = the responsiveness delta. Charm scheduling
  note: plain self-sends, no priorities (reconverse polls FIFO before
  the prioritized queue), CcdCallFnAfter only on retry/poll paths.
- S1. PHASEA STEAL (Kale step 1): per-process claim pool of pieces;
  own-first claim order. Arms: FOF_STEALA=0 (today's static
  assignment) / FOF_STEALA_GEO=0 (claim arbitrary sibling pieces —
  Kale's 1b comparison arm) / FOF_STEALA_GEO=1 (claim nearest-centroid
  unclaimed pieces). RE-KEY CONSTRAINT: pe_treepieces re-bucketed by
  realized assignment before buildPoolSlice (the silent-under-merge
  trap; fof1 phase-1-exact is the guard). Readout: phaseA_skew within
  factor at 80M + 2B (2B within = 1.84 pre-campaign). The CROSS pass
  runs over the realized set after the pool drains (preserving global
  self-before-cross suppression ordering); its cost is NOT counted in
  claim pacing. Claim-balancing cross pairs too (Kale's question,
  2026-08-11) is blocked not by atomic cost but by OWNERSHIP: a cross
  pair unites in the claiming PE's private uf_parent, so a foreign
  executor races the owner. S1c, IF the measured cross tail warrants:
  process-wide LOCK-FREE union-find (CAS unite; union-by-min-GLOBAL-
  ORDER is a total order, so attach-larger-to-smaller is cycle-free by
  construction — the same union-find the GPU port needs, so S1b
  doubles as its CPU rehearsal), making self+cross one claimable pool;
  requires re-basing the cert/connectivity memos on a process-global
  index space and abandons the frozen-phase no-atomics principle for
  phaseA (contention = the measurable). The cheaper alternative
  (edge-emission for stolen cross pairs) costs ~2.5x per pair (13.1 vs
  5.2 us measured live-vs-frozen) and weakens the suppression
  ordering. GATE: the milestone job's stealgeo arm — if within-skew at
  480 PEs stays ~1.0x with the deferred cross pass included (laptop:
  1.02-1.05), S1c is unnecessary. ARM NAMING (Kale, 2026-08-11):
  S1a = claim ignoring geometry (FOF_STEALA_GEO=0; the milestone job's
  stealscan arm); S1b = claim nearest-centroid (FOF_STEALA_GEO=1, the
  default; the stealgeo arm). The nearest-piece SELECTION is optimistic,
  not atomic: scan unclaimed entries with relaxed loads, pick the
  nearest, then CAS-claim; a lost race just re-scans for the next
  nearest. Only the claim is exclusive — the pick is advisory.
- S2. HIERARCHICAL PHASEB (Kale step 2): KD partition of pool units
  by m2-weighted centroid median splits (k = FOF_PB_PARTS, default 32
  per process); per-partition claim cursors; recursive m2-ranked TAIL
  splitting replaces the depth rule (split only units with m2 > 8x
  mean, recurse on children's own m2 — the measured adaptive rule,
  section 13); per-process barrier, then S2b: the L2 merge behind
  FOF_PB_MERGE (default per 0c), then cross-partition phase.
  Readout: t_phaseB_maxpair and phaseB max/avg at 2B vs the 0.634 s
  floor; partition-cost spread vs the KD dry-run prediction.
- S3. COORDINATOR STEALING within the physical node (Kale step 3):
  status poll (paced, coordinator-initiated), placement by
  coordinator, donor ships a whole partition (slimmed
  StealShipment-shaped blob: FoFCachedParticle particles, dense node
  indices when available, dedup by partition-scoped SEEN rebuild),
  edges return by settle-by-count through the origin's submitEdges
  door (destination-follows-origin on BOTH emit paths). Off below a
  size gate (the 8M polling lesson). Readout: phaseB max/avg at
  2B/128 processes vs S2's; probe RTTs during steal traffic.

Baseline set (step 0 of the campaign, AFTER tonight's queued jobs
drain): 80M/4-node x3 and 2B/16-node x2 on main + pemap config,
recording phase1_stages, phaseA_skew, t_phaseB_maxpair, probe RTTs
(on/off pair). These are the campaign's reference numbers; every S
step's Anvil A/B runs against its predecessor in the same session
where possible.

## 15. Measured: campaign milestone baseline, 80M/4 nodes (job 19803928, 2026-08-11)

First measurement under the PINNED configuration (the new affinity
boundary); binary = campaign branch (all features off by default). All
8 arms exact. THE BASELINE (3 reps): phaseA 0.143-0.148, phaseB
0.031-0.032, within-skew 1.45-1.49, cross 1.15-1.19, global 1.53-1.57.

- S1 AT SCALE: within-skew 1.45-1.49 -> 1.13 (stealscan/S1a) and 1.14
  (stealgeo/S1b); phaseA wall 0.148 -> 0.121 (S1a) / 0.138 (S1b).
  Single reps — S1a vs S1b not separable yet; neither reaches the
  laptop's 1.02 (residual = the un-paced cross pass + claim-scan
  cost). phaseB rose 0.031 -> 0.040-0.042 in the steal arms (pairs
  displaced into the pool, as predicted). Net phase1 (A+B): S1a 0.163
  vs base 0.179. The 2B point decides whether the residual matters
  (S1c gate stays open, not triggered at 80M).
- P0 ON INFINIBAND WITH THE PEMAP: median ping RTT 17-130 us across
  all 28 coordinator->sibling pairs (the laptop's 12 ms burial does
  NOT occur at ppn 15 — some PE is almost always free), but p99 tails
  of 1-28 ms remain (the idle-stall family). Transfer ladder: 4 KB
  40-80 us, 1 MB 240-470 us, 8 MB 3.2-4.8 ms (~2 GB/s) — a slimmed
  ~10 MB partition shipment costs ~4-5 ms, confirming the 30-60:1
  ship-to-work estimates. 64 KB shows odd variance (66 us to 2.7 ms)
  — eager/rendezvous boundary neighborhood, worth one look later.
- P1 slice: no measurable cost at 80M (phaseB 0.032 = baseline); the
  laptop's +40% single-rep worry did not transfer.
- Probe observer effect: probe-on arm's own metrics match the base
  reps — no distortion visible at this scale.

## 16. Measured: S2 at 80M (job 19804540) — two findings, one fix

All arms exact. (1) Exclusive whole-partition claims REGRESSED phaseB
0.031 -> 0.114 s (32 skew-costed partitions over 15 PEs: one PE drains
the giant alone). FIXED same day (unit-granularity claims over the
partition-ordered pool; partitions stay the S3 shipping/GPU unit —
claim exclusivity belongs to SHIPPED partitions only). (2) The region
formulation's B2 fraction is BIMODAL: 0-12% of m2 on typical
processes, 99.3% on the hot one — the n_below-weighted KD cuts the
dense core on exactly the process that matters, so its heavy pairs
all straddle regions (the dry run's 14% was a process-0 sampling
artifact). Consequences for the design: Kale's B1-merge-B2 compression
works as intended on typical processes; the HOT process needs either
core-avoiding region boundaries (e.g., split planes snapped away from
high-density gradients) or B2 stealing — which S3 provides anyway,
and the hot process is precisely the donor S3 targets. Mid-merge
cost at 80M: sub-millisecond, map ~600-1100 entries.

Addendum (v2 run, job 19804970): unit-granular claims recovered
phaseB 0.114 -> 0.049; the residual over the 0.031 baseline was
index-order partition concatenation (cheap partitions drained before
the giant — LPT lost across partitions); fixed same evening with
costliest-first concatenation. Merge-arm overhead at 80M: phaseB
0.055 incl. the full B1/barrier/merge/relabel/re-annotate/B2 cycle —
the barrier machinery costs ~6 ms here. The bimodal B2 finding
(max 99.3%) reproduced exactly.
