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

## 17. phaseBMidRelabel anatomy + the optimization it needs (Kale's question, 2026-08-11)

What the mid-relabel does per PE, from the code: (1) relabelBody —
ONE HASH PROBE PER FROZEN ROOT (not per particle; representative-
indirect since the relabel work), ~50k roots/PE at 80M against a ~1k
merge map; (2) materializeLabels — one INDEXED store per particle
(rewrites group_number, the label, not a parent pointer), no hashing;
(3) annotateFrozenTips — a FULL recursion over every local node
READING EVERY PARTICLE's group_number to rebuild min/max_frag. So no
per-particle hash lookup exists; the ~18 ms/PE cost (traced) is the
per-particle STREAMING of (2)+(3) — each label read pulls a 120-byte
Particle record through the cache, and (3) also walks every node.
IS IT PAYING? At 80M: NO — B2 is 0-12% of m2 on typical processes and
the wall moved ~nothing; the flag stays default-off pending the 2B
double-run. THE FIX IF 2B JUSTIFIES IT: touch only AFFECTED pieces —
the merge map is ~1k entries, so only roots whose label changed need
materialization and only pieces CONTAINING changed roots need
re-annotation; most pieces have zero changed roots and skip entirely,
making the barrier cost proportional to the map, not to N.

Related (secondary, Kale): the serial-mode applyGlobalMap broadcast
path still builds a per-PE hash of the map (480 copies of ~40k
entries at 80M = the 0.03-0.06 s relabel(p3) there). The sliced path
already stores the map ONCE per process on the node branch — lowering
FOF_SLICE_MIN_BYTES to 0 (always slice) removes the per-PE
construction at every map size; queued for the next measurement round.

## 18. Measured: the 2B stage-0 verdicts (job 19815941, 2026-08-12)

All runs exact under the pinned configuration (the per-arm retry armor
prevented the IBV-assert losses of the first two attempts).

- 0c DOUBLE-RUN — THE MERGE DOES NOT PAY AT 2B: pass1 per-process
  phaseB min/med/max 0.010/0.066/1.304 s; pass2 0.003/0.053/1.304.
  The BOTTLENECK PROCESS IS IDENTICAL TO THREE DECIMALS — full-merge
  compression (the upper bound of any partition-level merge) removes
  nothing from the process that sets the wall; only the median moves
  (-20%). Same shape as 80M. Per the design's own rule, STAGE 4 IS
  CANCELLED as a wall lever: FOF_PB_MERGE stays default-off (the
  machinery remains as validated opt-in — its barrier costs ~2-10 ms
  and it may serve other datasets). The 1.304 s lives on one process
  and can only MOVE — S3 stealing is confirmed as THE phaseB lever.
- G0 KD DRY RUN at 2B (process 0): cross-partition m2 5.4% / 24.3% /
  25.1% at k=8/32/64 — cost concentration holds (geometric prediction
  was 60-70%), with the standing bimodal caveat (process 0 is not the
  hot process; per-process pb_regions data showed up to 99% there).
  With the merge cancelled, partitions matter only as SHIPPING units,
  and the numbers say the right thing for that: at k=32 the hot
  partition holds 4.47e7 of m2 (8.3x the average partition) INCLUDING
  the giant 3.56e7 unit — one grant ships ~26% of the process's
  work, exactly the whole-partition-steal shape S3 wants.
- THE 2B SKEW SPLIT (the half-1 gate, now definitive, pinned config):
  within 1.70-1.72 x cross 1.44. Within-process still dominates at
  2B/128 processes — S1's claim pool has ~30% of the phaseA wall to
  recover at 2B (its 80M A/B already measured 1.45 -> 1.13).
  size_r -0.035: the per-PE size predictor is dead at 2B too, dynamic
  claiming confirmed once more.

## 19. S3 detailed scheme (reviewed by Kale 2026-08-12; IMPLEMENTED same
day on phaseab-campaign, commits through fbc04ed — status in section 20)

Coordinator-mediated whole-partition phaseB stealing within the physical
node. Everything below is inside the existing FoFPhase1Node NODEGROUP —
no new chare types. FOF_S3=0 (default) is bit-identical current behavior.

ROLES. Every process's branch is an AGENT: it owns the local partition
table (built by S2's KD split, FOF_PB_PARTS on — with the merge cancelled,
partitions exist purely as shipping units), executes ship orders, runs
foreign work, returns edges. The lowest-rank branch of each
FOF_PROCS_PER_PNODE block is additionally the COORDINATOR for that block.
Decision entries on the coordinator are [exclusive]; any free PE serves
them (Kale's spec). v1 scope: within the physical node only.

PROTOCOL.
1. PUBLISH: when a process finishes its pool build it sends the
   coordinator s3Publish {k partition costs, total m2, n_units} (~300 B).
2. MONITOR: the coordinator polls its block every FOF_S3_POLL_MS
   (default 10 ms, CcdCallFnAfter — pace-every-retry); agents reply
   s3Report {remaining_m2, drained, idle_pes}. remaining_m2 is an atomic
   counter decremented at each unit claim (one atomic add per unit,
   ~9k/process at 2B — negligible). Agents ALSO push an unsolicited
   report the moment they drain (event beats next poll tick).
3. DECIDE ([exclusive] s3Decide, fires on report arrival): helper = a
   drained process; donor = block max remaining_m2. Ship iff
   donor_remaining > FOF_S3_IMBALANCE (1.5) x block average AND
   donor_remaining > an absolute floor (~20 ms of predicted work via the
   cost-model constant) — below that, shipping costs more than it moves.
   One outstanding order per (donor, helper) pair; [exclusive]
   serializes decisions; new decisions only on fresh reports (no retry
   storms). Order: donor's costliest partition not yet ordered.
4. SHIP (donor agent, s3ShipOrder): mark the partition SHIPPED (CAS; the
   local unit-claim loop skips unclaimed units of shipped partitions),
   then collect its still-unclaimed units using the SAME per-unit CAS the
   local drain uses — whoever wins the CAS owns the unit, so zero double
   execution by construction, no new locks. If nothing remains, reply
   s3Declined (coordinator picks another partition). Serialize one
   shipment: subtree snapshots deduped by root across the partition's
   units (preorder-flattened nodes with frag summaries incl. n_below +
   FoFCachedParticle 20 B leaves) + the unit list as snapshot-index
   pairs + period/b. Feasibility is the measured probe: 8 MB moves in
   3.2-4.8 ms; the 2B hot partition (~26% of the hot process's work in
   one grant, section 18) against a 1.304 s imbalance.
5. EXECUTE (helper): rebuild ephemeral trees (dense arrays), run the
   units through the existing sliced drain with shipment-scoped SEEN and
   cert memos. Edges stay in a shipment-local buffer in GLOBAL
   particle-order tips — phaseB precedes tip encoding, so tips are valid
   on any process; the helper NEVER encodes (section 3 rule). Helpers
   take foreign work only when their own pool is drained (true by
   construction of the decision rule).
6. RETURN (s3Return to donor): edge vector deduped WITHIN the shipment
   + per-unit walltimes. The donor feeds them through the ordinary
   submitEdges door — which re-checks every edge against the donor's
   LIVE seen set, grown since the shipment left (the same tip pair can
   arise from different units) — and decrements ships_outstanding
   (Kale's review point 2026-08-12: helper-side dedup alone is not
   enough; donor-side re-dedup at return is required. A slipped
   duplicate would be a cost bug, not a correctness bug — union() is
   idempotent — but the re-check is nearly free).
7. TERMINATION: the donor's process merge fires when b_done == nranks
   AND ships_outstanding == 0 (the return handler may be the closer) —
   a one-condition extension of the deposit chain. The helper's OWN
   merge never waits on foreign work; foreign slices keep being served
   from the scheduler after its deposit, interleaved with whatever its
   process does next. The global relabel already waits for every
   process's merge, so the critical path counts foreign work exactly
   once, at the donor.

KNOBS: FOF_S3, FOF_S3_POLL_MS (10), FOF_S3_IMBALANCE (1.5),
FOF_S3_MIN_SHIP (absolute floor), FOF_PROCS_PER_PNODE (block size).
STATS: FOF3STAT s3 line — ships out/in, declined, shipped m2 and bytes,
build/rebuild/return-latency ms.

GATES: fof1 exact + FOF_COUNT_VERIFY both runtimes at 8M/80M laptop;
Anvil 80M A/B (S2 vs S2+S3); then 2B, where the target is the 1.304 s
hot process — its physical node holds 7 near-idle siblings, so the
ceiling is ~1.304/8 + overhead; the v1 success bar is a 2-3x cut.

SCOPE (confirmed with Kale 2026-08-12): S3 steals ONLY phaseB pair
units (cross-TreePiece pairs from the process pool, grouped in
partitions). Nothing in phaseA is ever stolen across processes —
phaseA work moves only WITHIN a process via S1's claim pool.

DEFERRED to v2: cross-physical-node escalation (coordinator-to-
coordinator handoff) if the hot process's whole block turns out hot;
shipping arbitrary unit sets instead of partitions; phaseA cross-process
steals.

## 20. S3 v1 implementation status (2026-08-12)

Implemented as designed in section 19 with these v1 simplifications,
all laptop-gated on BOTH runtimes (classic netlrts and reconverse,
-u serial per the campaign measurement policy):

- Decisions are DRAIN-EVENT driven, no poll layer yet: the coordinator
  acts on publish/drained/declined events only. The donor pick is
  "largest un-ordered partition of any other member" — without polls the
  coordinator does not know remaining work, so declines prune emptied
  partitions (one decline each, permanent). Polls refine the donor pick
  later if 80M/2B show bad picks.
- The helper executes a whole shipment on the PE that serves the
  s3Shipment entry, unsliced (helpers are drained by construction).
  Parallel foreign drain and slicing are v2 items.
- Helper-side in_ships/in_units print at the helper's OWN merge, which
  can precede its helping — donor-side numbers are authoritative
  (cosmetic; move the print to phase3Stats when it matters).
- FOF_S3_TEST=1 forces the maximal exercise: even processes refuse every
  local claim, so their entire pool must travel order/ship/execute/
  return and the counts must still be exact.

The termination invariant sharpened during bring-up: the merge fires
only when b_done is full AND every pool unit is OWNED (one CAS per unit,
won by a local PE or a shipment collection — the s3_units_owned ledger)
AND every shipment returned. Cursor exhaustion alone is NOT ownership;
the forced test lost 2031 components to exactly that assumption before
the ledger existed. Second bring-up defect: the decision pass initially
gave up when the FIRST free helper had no donor, orphaning the second
helper's work — every free helper now gets a donor search per pass.

Gates (all counts exact; 1M and 10k under the full serial-oracle
verification): classic 2x4 and 4x2 forced tests (entire pools executed
remotely, e.g. 8 ships/4030 units off node 0; 4-proc run had two donors
helping each other), natural mode, S3-off bit-identical base; reconverse
2x4 forced (with a bidirectional 21-unit remnant exchange), 4x2 forced,
natural, base. Next: 80M Anvil A/B (S2 vs S2+S3), then 2B where the
1.304 s hot process is the target (ceiling ~1.304/8 + overhead; v1 bar
2-3x).

## 21. Measured 2026-08-12 afternoon: daytime2b (19833850) + the S3 80M gate (19833867)

Every arm of both jobs exact on the FIRST try — the --mem=0 steps rode
a daytime window, so the OOM mitigation is untested-by-adversity but
nothing failed. Findings:

- FIRST 2B S1 MEASUREMENT (serialsum arm, S1b+m2key): phaseA
  1.43 -> 1.03 s (-28%; reference = probes2bv2 no-claim serial arms,
  same config previous night), within-skew 1.70 -> 1.38. Direct steal
  evidence at 2B: EVERY process stole (foreign claims 30-118 pieces per
  process, typically ~60-90 of ~550 total; 9-14 of 15 PEs per process
  took at least one foreign piece).
- THE S1->PHASEB TRADE AT 2B: phaseB 1.30 -> 1.61 s (+0.3) against
  phaseA's -0.4. Net phase1 gain is small at 2B; claims scatter pieces
  and the pool pays. This is the 80M trade (0.031->0.040) at scale, and
  it sharpens the argument that S3 (move phaseB work, not phaseA
  pieces) is the lever that matters at 2B.
- V3 VERIFIED AT 2B: parts mode (k=32, costliest-first concatenation)
  phaseB 1.554 vs flat 1.609 — the v2 regression (+60% at 80M) is gone;
  partitions are FREE as the S3 shipping substrate.
- ITEM 11 ANVIL POINT: labelbase uf2 0.391 vs labelbatch 0.288 s
  (-26%, single rep each; Frontier measured -13% at the same scale).
- DIST HEALTH: plain -u dist on current main ran clean in the daytime
  window — nothing in the stack changed; the overnight failures were
  the OOM window (agenda item 9 root cause).
- SUM-DETAIL: first campaign-era 2B image data captured (serial, S1b,
  1920 PEs; traces/sumd2b-serial-19833850.tar.gz, copied to laptop).

S3 80M GATE (19833867, 4 nodes/32 procs, all counts 23707197):
- natural arms shipped NOTHING and cost NOTHING (phaseB 0.058 = base
  0.058): the 80M pool drains in 58 ms, no helper is ever matched —
  S3 idles for free.
- forced arm: 16 even processes shipped their ENTIRE pools — 513
  shipments, 377,167 units executed remotely, 51,286 edges returned —
  counts exact, and phaseB actually dropped to 0.045 s (16 idle
  helpers soak half the machine's phaseB).
- protocol exercised at 4 coordinator blocks (FOF_PROCS_PER_PNODE=8
  default) with donors that also helped (node 0: 32 ships out, 14 in).

Next: the 2B S3 job (base/S3 x2 interleaved + S3 sum-detail + forced
last) — the hot process's 1.3-1.6 s phaseB is the campaign's target.

Frontier 4-node 80M shakedown (Kale's relay, 2026-08-12 afternoon):
all three serial arms exact (23707197); forced arm 511 out_ships /
382,475 units / 48,950 ret_edges — the same shape as Anvil's 80M gate
(513 / 377k / 51k) on a different transport. S3 v1 has now moved whole
pools correctly on classic-netlrts (laptop), reconverse-local (laptop),
InfiniBand (Anvil), and Slingshot/CXI (Frontier). The 16-node 2B 2x2
A/B (design/frontier-s3-ab.md) is cleared to run.

## 22. The Frontier 2B verdict (2026-08-12, design/frontier-s3-ab-results-2026-08-12.md)

9/9 exact at 2B/16 nodes, serial AND dist. The forced arm moved 2,035
shipments / 1.15M units / 611k returned edges, still exact — S3 v1 is
CORRECT at full scale on Slingshot. But the natural arms shipped 1-8
grants against ~1,100 declines, and phaseB is unchanged (3.25 s +-
noise, both modes).

READ THE DECLINES CAREFULLY — they falsify one hypothesis and sharpen
another:
- The order-servicing-starvation suspicion is WRONG: ~1,100 declines
  per run means donors served every order promptly (a starved donor
  yields silence, not declines). The coordinator solicited all phase.
- What the declines actually say: WHENEVER A HELPER WAS FREE, EVERY
  ORDERED PARTITION WAS ALREADY FULLY CLAIMED. Under the v1
  drain-event protocol, helpers appear only when a process's cursor
  exhausts — and by then, apparently, everyone's cursor is exhausted.

Two candidate explanations, and the discriminator between them is the
PER-PROCESS phaseB wall distribution, which no current print shows:
(a) S2 already balanced phaseB: m2-LPT + costliest-first partitions
    flattened the per-process walls, so processes finish nearly
    together and there is genuinely nothing to steal. Then S3 is
    insurance, not a lever, and the 3.25 s phaseB is a THROUGHPUT
    problem (walk speed), not a balance problem.
(b) The imbalance survives but the protocol misses it: claims (one CAS
    ahead of each walking PE) do not exhaust a hot pool early, but if
    the hot process's cursor nevertheless runs out before the fast
    processes' LAST units complete (helpers announce drain once, at
    cursor exhaustion, possibly while still executing), the match
    window is narrow. Needs the distribution to judge.
The queued Anvil 2B job's S3 SUM-DETAIL arm answers this visually
(per-PE time profile), and a one-line FOF3STAT pb_wall per-process
print is the cheap permanent instrument. DECISION DEFERRED until that
evidence: if (a), the campaign pivots from balance to phaseB
throughput; if (b), S3 v2 = polls + earlier helper availability.

Also measured: Frontier phaseB 3.25 s vs Anvil 1.55-1.61 s at the same
2B/16-node scale (different CPUs, 14 vs 15 workers/process) — worth
keeping in mind when comparing campaigns across machines. Frontier
cross-skew point: 1.40-1.42 at 128 processes (phaseA), stable across
all arms. FOF_S3_TEST deadlocks single-process (rank 0 refuses claims,
no helper exists) — documented limitation, >= 2 processes required.

## 23. Frontier follow-up (same day): SMT split verdict, the straggler's
## anatomy, and the starvation mechanism recovered

From the updated Frontier report (design/frontier-s3-ab-results-2026-08-12.md,
sections added after the first A/B):

- THE HOT PROCESS IS ALIVE under full campaign flags: per-PE phaseB
  min/med/max = 0.008/0.190/3.221 s. Median process finishes in ~0.19 s;
  the wall is one straggler at 3.2 s. Explanation (a) of section 22
  (S2 balanced it away) is DEAD.
- THE STRAGGLER IS AN ACCUMULATION, NOT A GIANT: max single unit
  0.256 s against a 3.2 s PE wall (~13 units deep at minimum). The
  work is divisible — it just never moved.
- THE STARVATION MECHANISM, CORRECTLY LOCALIZED: section 22 dismissed
  order-servicing starvation because declines flowed freely — but the
  declines come from the IDLE donors, whose PEs serve entries
  instantly. The HOT donor's 14 PEs sit inside the unsliced drain loop
  for the full 3.2 s and serve its ship orders only at the end, when
  everything is claimed. The idle donors' promptness masked the hot
  donor's silence. Fix: FOF_PHASEB_SLICE_MS on S3 arms (the sliced
  drain surfaces between units); under test in Anvil job 19842202
  (arms: base, S3+slice, base+slice, S3-no-slice, sum-detail on the
  fix, forced). If the fix ships, make slicing the S3 default.
- SMT SPLIT VERDICT (16-node 2B, same 56 physical cores, ppn 14 vs 7):
  phaseA +27% from SMT (2.037 vs 2.584) — Kale's pointer-chasing
  intuition confirmed there; phaseB -31% (3.285 vs 2.504); net phase1
  4.4% faster WITHOUT SMT. CONFOUND to respect: halving ppn changed
  the decomposition (max_piece_n doubled, pool units 3.2x fewer), so
  the phaseB number is not pure SMT — bigger pieces shift work between
  self-pairs, the grid gate, and the pool. Per-core the machines are
  ~comparable (the 1.9x aggregate gap is mostly the 15-cores-vs-7
  accounting).
- OPERATIONAL: +lci_ndevices must track ppn (min(8, ppn/2)); 7 devices
  on ppn 7 hangs at cache-manager init. Frontier per-core baselines
  need 1M-8M inputs (10k/100k sit at timer resolution).

## 24. S3 v2 scheme (PROPOSED 2026-08-12 evening — awaiting Kale's review)

Where v1 stands after the 7b4b3f2 Frontier round (job 5250048): slicing
+ S3 is FREE (wall back to the unsliced baseline; the 2.2-3.5x
regression is gone) but INERT (0.10% of units moved; straggler
untouched at 3.29 s vs 0.19 s median). Root cause of inertness: the
grant cap is denominated in m2 against SINGLE-PE helpers, and the
coordinator orders the costliest partitions — whose units are the m2
giants (the 2B giant alone is 7x the cap), so grants collapsed to 1-3
units. The v1 serial helper is the binding constraint everywhere.

v2 = four coupled changes, all inside the existing machinery:

1. PARALLEL FOREIGN DRAIN (the structural fix): s3Shipment (served by
   any helper PE) rebuilds trees and appends units to a helper-local
   FOREIGN QUEUE (atomic cursor, same shape as the home pool), then
   wakes every PE of the process (self-sends, steal-branch idiom).
   PEs drain foreign units through the same sliced loop into
   per-origin edge buffers; a per-shipment completion counter fires
   s3Return from the last finisher. Helper capacity: 1 PE -> all 14/15.
2. GRANT SIZED TO CAPACITY: with parallel helpers the cap rises to
   FOF_S3_MAX_GRANT_M2 ~ 1e8 (or a predicted-seconds knob x worker
   count); a grant should be a few hundred ms x the WHOLE helper.
3. RE-ORDERABLE PARTITIONS: the coordinator's ordered[] flag becomes
   in-flight[]; cleared when the shipment returns or is declined; only
   an EMPTY decline retires a partition permanently. The coordinator
   can return to the straggler's partitions until they are drained.
4. REMAINING-WORK POLLS (from Kale's original design): every branch
   maintains remaining_m2 (one atomic subtract at each claim CAS);
   the coordinator polls its block every FOF_S3_POLL_MS (default 10)
   and picks donors by CURRENT remaining, not initial cost — so orders
   chase the actual straggler.

Unchanged: per-unit CAS ownership, the owned-units + returns
termination ledger, forced mode, the loopback self-test, exactness
gates both runtimes. Capacity check: a block's idle capacity once the
median process drains (~7 procs x 14 PEs x ~3 s ~ 290 core-s) dwarfs
the straggler backlog (~45 core-s) — the movable fraction is not the
constraint once helpers are parallel.

Kale's standing review point also attaches here: the yield mechanism
(FOF_PHASEB_SLICE_MS wall-clock) should eventually become per-unit or
every-k-units yields — the natural message-driven grain.

## 25. v2 gate results (2026-08-12 night) and a known-stall encounter

v2 (commit "S3 v2: parallel foreign drain...") gates:
- CLASSIC: all 7 exact — 1M loopback zero-mismatch, forced 2x4/4x2
  with ships, natural, base, 10k full-oracle, and the dataset guard
  (100k-uniform: S3-off == S3-forced at 98275).
- RECONVERSE: -n 4 forced PASS (39 s; ships BIDIRECTIONAL — nodes 0/2
  shipped 18/19 grants and also helped 9/8 in; exact). -n 2 with S3
  armed STALLS: lldb attach shows every PE of both processes idle in
  CsdScheduler -> LCI poll_comp, no app code on any stack — the known
  reconverse LCI IDLE-PROGRESS STALL (keep-alive-ring family), not an
  S3 logic fault. Discriminators: classic -n 2 identical scenario
  passes; reconverse -n 2 with S3 OFF passes; pre-v2 -n 2 with v1 S3
  passed (v1's serial helper kept a PE busy; v2's efficient wait is
  what goes fully idle). Cheap candidate fix if it matters beyond the
  laptop: extend the keep-alive ring into phase 1 while FOF_S3 is
  armed. Laptop reconverse gates use -n 4 until then; Frontier's CXI
  fabric is the real test.

## 26. v2 at 2B/16 Frontier (job 5250364): FIRST NET WIN

Four-generation table in the relayed report (stored below §25's file
pointer; reps x3, all exact incl. traced):
- WALL BELOW BASELINE FOR THE FIRST TIME: Pre-traversal 4.47-4.53 s vs
  5.32 unsliced (~15% under); Iteration 0 6.98-7.24 vs 8.01 (~13%).
- Per-PE phaseB max 3.29 -> 1.98-2.11 s (36-40% off); imbalance ratio
  17x -> 12x. Improved, not solved.
- 4.3-4.8% of units moved (95-106k) at 206-224 units/grant — HALF the
  448 sizing. Explanation (to verify): costliest-first concatenation
  puts the coordinator's favorite partitions FIRST in the local drain
  order, so orders land on half-eaten partitions; the local cursor and
  the coordinator compete for the same end of the pool. declines also
  highest of any generation (~1420) — same signature.
- The phaseB timer excludes inter-slice gaps; the report correctly
  judges by wall-clock rows (which moved WITH it this time — unlike
  v1-sliced where they diverged).

Next levers, ranked (not yet built):
1. Grant attrition compensation: FOF_S3_GRANT_UNITS_PER_PE 32 -> 64
   (pure env knob, zero code) — tests whether grant size is binding.
2. Partition pick beyond the cursor: order partitions the local drain
   will reach LAST (with costliest-first concatenation that is the
   tail), trading per-unit m2 for intactness. One-line pick change.
3. Multiple outstanding grants per helper (removes the
   order->ship->return->drained serialization; moderate change).

## 27. The composition finding, and reservation (IMPLEMENTED overnight 2026-08-13, commit 309673c on phaseab-campaign; Kale approved implementing while asleep)

Frontier msg2 (2026-08-12 night, ~30 exact runs): STEALING MOVES THE
CHEAPEST WORK BY CONSTRUCTION. The pool is costliest-first (deliberate,
twice: slice claiming and partition concatenation — the +60% regression
proved LPT matters); the local cursor eats from the front; grants
collect only what the cursor has not reached — the cheap tail. Donor
eats giants, helpers get dust. Evidence: m2/shipped-unit FALLS as
grants grow (19.1 -> 16.4M across GRANT 32->128); straggler = 3x units
x 3x cost/unit; phaseB_s max floor 1.53 s across every knob of four
generations; granularity NOT binding (floor would be 0.25 s under
perfect balance — 6x headroom). Instruments to confirm shipped-dust
directly (tot_m2 denominator, s3_grant_m2_hist) are in at 0963ded+.

PROPOSED FIX — donor-side reservation, with one refinement over the
Frontier suggestion: reservation must be DYNAMIC AND CONDITIONAL.
A static ship-only prefix on every process re-creates the giants-last
pathology (+60%) on every NON-straggler. So:
- All processes start normal (LPT drain, no reservation).
- The coordinator already polls remaining_m2. When a member's remaining
  exceeds FOF_S3_RESERVE_FACTOR (~2x) x the block mean EXCLUDING SELF
  ((sum - mine)/(P-1)), the coordinator sends it RESERVE. (Frontier
  review point 2026-08-12: a self-inclusive mean with one 12x straggler
  among 8 members inflates the average to 2.375x, so a nominal 2x
  trigger really fires at ~4.75x and misses second-tier ~3x hotspots;
  self-excluded mean restores the stated intent. Keying on m2 rather
  than unit count is confirmed by the straggler's decomposition — 3x
  units AND 3x cost per unit — a count trigger under-detects ~3x.) the donor then treats its top-m2 unclaimed units
  (up to a budget) as ship-only: local claims skip them while other
  units remain, grants collect them FIRST.
- Starvation valve: a local PE that finds ONLY reserved units left
  takes them anyway (reservation is advisory; termination ledger
  unchanged). Coordinator can send UNRESERVE when balance is restored
  or helpers dry up.
- Mechanism sketch: reservation = an index threshold into the
  costliest-first pool order (the top-m2 prefix IS the pool front), so
  "reserved" is just k < R with R set by the RESERVE message; local
  drain starts its cursor at R and wraps to [0, R) at the starvation
  valve; grants scan [0, R) first. One extra cursor, no new locks.
Expected effect: grants carry giants (s3_grant_m2_hist shifts high),
straggler max falls toward the 0.25 s granularity floor. Decision
knobs for review: reserve trigger factor, reserve budget (fraction of
remaining), and whether UNRESERVE is needed in v1 of this.

## 28. Measured: Anvil 2B S3 pair harvested late (job 19842202, run 2026-08-12 evening, harvested 2026-08-13)

The resubmission of the cancelled 19839458. Six arms, 1920 PEs, all
timing arms exact (424897832):

| arm | phaseA | phaseB | phaseB_s max | ships / units | declines |
|---|---|---|---|---|---|
| base1 | 35.81 | 1.572 | 1.549 | 0 | 0 |
| baseslice | 39.02 | 1.570 | 1.547 | 0 | 0 |
| s3slice | 36.49 | 1.343 | 1.288 | 421 / 85,505 | 1,504 |
| s3noslice | 27.32 | 1.571 | 1.549 | 57 / 9,034 | 1,658 |
| s3forced | 113.1 | 9.149 | 6.328 | 2,029 / 1.22M | 1,015 |

Readings:
- SLICING IS WHAT LETS S3 ENGAGE, independently confirmed on a second
  machine: s3noslice ships 57 grants and changes nothing; s3slice
  ships 421 and cuts phaseB 15% / per-PE max 17%. Consistent with the
  Frontier v2 story at a much milder straggler (Anvil max 1.55 s vs
  Frontier 3.29 s at the same dataset — different node count/CPU).
- The S3-SUMDETAIL ARM CRASHED (rc=1, no components, 0 trace files),
  so the section-22 discriminator (per-PE phaseB wall distribution
  under S3) is STILL unanswered on Anvil. Rerun it when Anvil work
  resumes; the balance: line's min/avg/max is the interim proxy.
- phaseA noise on Anvil is large (27.3-39.0 s across arms with
  identical phaseA config) — phaseA deltas in this job are not
  interpretable; phaseB rows are the signal.
- Forced arm exact at 1.22M units shipped — S3 correctness at 2B now
  demonstrated on five transport configurations.

## 29. Measured: Anvil 2B reservation A/B (job 19860455, 2026-08-13 morning) — machinery engages, trigger over-fires late in the drain; a fabric caveat resets the Anvil picture

Five arms, all exact (424897832), including the sum-detail rerun
(3,841 trace files at traces/2b-resv-sumd-19860455 — the section-22
per-PE dataset finally exists). Base env = 19842202's + SLICE_MS=2 on
every arm; binary staged from 0f30988 (campaign-bin/FoF3.2b.resv,
traced-bin/FoF3.2b.resv-sumd).

| arm | phaseA | phaseB | phaseB_s max | ships / units | resv_shipped | declines |
|---|---|---|---|---|---|---|
| base | 1.029 | 1.595 | 1.573 | 0 | 0 | 0 |
| s3resv1 | 1.038 | 1.868 | 1.688 | 7,340 / 48,034 | 37,294 | 1,524 |
| s3noresv | 1.027 | 1.782 | 1.609 | 10,540 / 38,674 | 0 | 1,781 |
| s3resv2 | 1.040 | 1.754 | 1.612 | 7,803 / 43,273 | 33,377 | 1,784 |
| s3sumd (traced) | 1.045 | 1.745 | 1.579 | 10,074 / 48,868 | 35,968 | 1,381 |

1. FABRIC CAVEAT, and it is large: phaseA fell 27-39 s (every arm of
   19842202, evening) to 1.03-1.05 s (every arm here, morning), with
   cross-skew 16.5 -> 1.35 and max_piece_n identical (129019). Nothing
   in the code delta touches phaseA's walk. Most plausible: the
   evening job ran in Anvil's degraded messaging mode (the ~100x mode
   Ritvik's aba7833 comment documents); this morning's fabric is
   healthy. So section 28's ABSOLUTE numbers — including the s3slice
   phaseB win — carry a degraded-fabric caveat; morning runs are the
   trustworthy Anvil baseline. (Not fully excluded: the windowed
   flush changing network state entering phaseA; a
   PARATREET_FLUSH_WINDOW=0 arm would discriminate, if ever worth a
   run.)
2. The WINDOWED FLUSH (aba7833) is confirmed as the 19842202-sumd OOM
   fix: per-task MaxRSS 12.2-12.5 GB -> 8.3-9.3 GB untraced; the
   traced arm completed at 13.5 GB where its predecessor was
   OOM-killed >14 GB mid-flush. vmhwm after decomposition 6.9 GB
   (untraced) / 11.5 GB (traced), flush window 131072.
3. On a HEALTHY-fabric Anvil, S3 at 2B is net overhead: every S3 arm
   is slower than base (phaseB +0.15-0.27 s), per-PE max not helped
   (1.58-1.69 vs 1.573). Anvil's straggler is mild (max 1.6 s,
   17x max/avg concentrated in a few PEs); the ~40k-unit reshuffle
   plus its 16-22k returned edges costs more than it saves.
   Reservation is not distinguishable from noreserve in wall time
   here. Anvil validates MACHINERY, not the composition win — the
   composition problem (section 27) is Frontier's 3.3 s straggler.
4. RESERVATION MACHINERY ENGAGES: 69-72 windows per run,
   resv_shipped = 33-37k of 43-49k shipped units (~78% of shipped
   units came from reserved windows); s3_grant_m2_hist has real mass
   in the top buckets (19-27). But noreserve's hist ALSO peaks
   high — on a fast, balanced drain the cursor has not eaten the
   giants by grant time, so composition was never Anvil's problem.
5. THE DESIGN LESSON — the trigger over-fires late in the drain:
   72 of 128 members received RESERVE, and many windows fence
   negligible work (window m2 observed at 1.3e3, 1.22e4, even 0.203).
   The 2x-block-mean-excluding-self trigger has no absolute floor, so
   as pools drain toward empty the mean shrinks and jitter trips it
   everywhere. Grants are dust-sized in every S3 arm here (3.7-6.5
   units/ship vs 203 in 19842202) — that part is v2-on-healthy-fabric
   behaviour (orders chase remaining work that is nearly gone), not
   reservation-specific, but reserving a near-empty pool is pure
   overhead. PROPOSED (pending Kale, per section 27's
   knobs-for-review): an absolute-work floor on the trigger —
   RESERVE only if the member's remaining_m2 also exceeds
   FOF_S3_RESERVE_MIN_FRAC (say 0.05) x its tot_m2 (both already
   known at the trigger site) — plus, eventually, the same floor as
   an UNRESERVE condition. Frontier's reservation pair should wait
   for this decision if it has not already run.

## 30. The Frontier grant-m2 verdict (2026-08-13 morning): the 5e7 budget was the whole regression; reservation INVERTS composition once grants are sized; defaults changed

Full report verbatim in
design/frontier-grantm2-reserve-verdict-2026-08-13.md (jobs 5253386 +
5253475, 21 2B runs, all exact). The distillation:

1. FOF_S3_GRANT_M2=5e7 (default shipped with 0963ded) strangled every
   grant to ~18 mean-cost units — ~25x tighter than the count cap,
   which was dead code at 2B. Arithmetic, and confirmed by ladder:
   noreserve at GRANT_M2=1e11 reproduces v2 (5250364) on all five
   metrics. Nothing else between b210b6f and 0f30988 costs anything;
   aba7833 is exonerated (base arm matches pre-v2 base).
   THIS — not "orders chasing drained pools" — is also the real cause
   of section 29's dust grants on Anvil (3.7-6.5 units/ship at the
   same 5e7 default; Anvil mean m2/unit ~2.3e6). Section 29 item 5's
   fragmentation attribution is corrected accordingly; its
   trigger-over-fire observation (72/128 windows, window m2 down to
   0.2) stands on its own evidence.
2. RESERVATION LOSES ONCE GRANTS ARE PROPERLY SIZED, and loses worse
   the better the grants: at the best cell (GRANT=128, PARTS=16,
   M2=1e11) reserve vs noreserve = phaseB +84%, Pre-traversal +85%,
   work moved 38.0%->22.2%, ret_edges 6.7x. Mechanism: grants drain
   the cursor window BEFORE the ordered range, so they fill with
   average-cost units (1.89e7 -> 6.75e6 m2/unit) — the composition
   fix inverted into a composition pessimizer. Section 27's premise
   needs revisiting: if retried, reservation should DISPLACE the
   costliest-partition path, not preempt it.
3. BEST CONFIG FOUND at 2B/16 Frontier: GRANT_UNITS_PER_PE=128,
   PB_PARTS=16, GRANT_M2 lifted, RESERVE off -> phaseB_s max 1.572 s
   (6.3x the 0.25 s floor, vs 12.6x as-specified), Iteration 0
   7.138 s — campaign best.
4. DEFAULTS CHANGED (1996ebd on phaseab-campaign, same morning):
   FOF_S3_GRANT_M2 5e7 -> 1e10 (count cap binds first on sane
   compositions; still clips an all-giants scoop, the v1 failure the
   budget exists for), FOF_S3_GRANT_UNITS_PER_PE 32 -> 128,
   FOF_S3_RESERVE default OFF pending redesign (=1 re-enables;
   FACTOR=0 remains the correctness-gate config). PB_PARTS stays a
   run knob; best-known value at 2B is 16.
   Laptop classic gates all exact at 3549: base, s3 natural, forced
   2x4 (9 ships/1520 units) and 4x2 (16 ships), and forced with
   RESERVE=1 FACTOR=0 (2 windows) — the reservation path stays gated
   though non-default.
5. Standing gap after all of this: phaseB_s max 1.572 s vs the 0.25 s
   granularity floor — 6.3x headroom that neither grant sizing nor
   reservation-as-built recovers. Open levers from section 26's
   ranking: multiple outstanding grants per helper; partition pick
   beyond the cursor (order what the local drain reaches LAST);
   plus the displacement redesign of reservation (item 2) and the
   never-analyzed sum-detail per-PE profile (Anvil traces at
   2b-resv-sumd-19860455, section 29).
