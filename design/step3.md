# Step 3: certificates, SEEN suppression, and the per-pair state machine

Status: DRAFT for review (Kale), 2026-07-18. No code until the
termination logic here is approved.

Context: `../fof_design_note.md` §4, §6.3a, §8.3–8.4. This is the
paper's core mechanism and its most fragile part: parked work with no
message in flight can let quiescence fire with edges silently dropped,
and the failure mode is a rarely-wrong answer, not a crash.

## 0. Proposed staging: 3a (certificates + SEEN) before 3b (parking)

**3a** adds the two certificates and SEEN suppression, but *no*
IN_FLIGHT state and *no* parked lists:

- open(A,B) with A uniform-g, B uniform-f:
  - (g,f) SEEN            -> prune                       [case 3]
  - maxdist(A,B) <= b     -> emit edge, mark SEEN, prune [case 2]
  - else                  -> descend                     [redundant OK]
- First leaf-level witness emits the edge and marks (g,f) SEEN;
  descents already in progress over the same pair get pruned at their
  next open(). Concurrent redundant descents before the first witness
  are permitted.

Why stage: 3a keeps the termination argument trivial — every node
pair either prunes (finishes synchronously) or descends (its
continuation is carried by messages: cache requests/resumptions), so
work parked nowhere, and **RTS-level QD remains sound exactly as in
v1**. Search failure needs no handling at all (each redundant search
self-completes; no state to release). All correctness-relevant
machinery of the paper lands in 3a; what 3b adds is *elimination of
the redundant concurrent descents* — a performance mechanism whose
value §8.3 tells us to measure anyway. 3a's counters give us that
measurement (redundant-descent count per fragment pair) before we pay
for 3b's complexity, and 3b's QD accounting can then be validated
against a known-correct 3a baseline on identical inputs.

**3b** adds IN_FLIGHT + parked lists + counter-based QD, per §4.1.
Separately reviewed and validated (see §5–6 below); its design is
specified here so both halves are approved together, but it does not
land until 3a's measurements are in hand.

## 1. Where the state lives

SEEN table (3a) and the pair-state table (3b): **process-level, on the
FoFPhase1Node nodegroup**, hash map guarded by a mutex (striped if
contention shows; the ParaTreeT cache uses the same shape). Process
level, not per-PE: with the transposed traversal the *target* fragment
f is always process-local, so all node pairs over (g,f) are generated
on f's owner process — one process's table sees every pair over its
keys, and suppression needs no global state (design note §4.2's
structural asymmetry, which survives our transposed-walk formulation
with g and f swapping roles: the walker owns f, the remote side is g).

Reset per iteration alongside the existing phase-3 reset.

## 2. What the visitor needs (and the cache-ordering fix)

Case 2/3 both read `min_frag`/`max_frag` (via `uniform()`) on
**internal, possibly cache-shipped** nodes. Today that is broken for
two node populations:

1. **Starter pack**: `Driver::loadCache` ships top-of-tree
   CachedBoundary FragData computed at *build* time — before phase 1,
   so annotations are garbage. Fix: reorder the FoF app's
   `preTraversalFn` to run phase 1 + `upwardPass` *before* it calls
   `loadCache`. This is app-level sequencing, no library change
   expected — but the implementation must verify with a CkEnforce
   (e.g., a validity flag in FragData: min_frag <= max_frag or
   n_particles == 0) on every node the visitor consults.
2. **TreeCanopy nodes above subtree roots**: `upwardPass` already
   refreshes these (phase 1 relabel happens before it). Covered, but
   the same CkEnforce applies.

New geometry helper: `maxdist2(boxA, boxB)` (max over the 8x8 corner
distances reduces to per-axis max of |lo-hi| endpoints — closed form,
same shape as mindist2). Offset-parameterizable like mindist2 (PBC).

Empty nodes: `uniform()` is already false for empty (min>max
identities), and case 2 requires both sides non-empty — n_particles
is present on cached nodes. No change needed, but tests must include
empty-leaf-adjacent geometry.

## 3. The 3a state machine (two states)

Key: (g,f) = (remote tip, local tip), packed u64 as in phase-B SEEN.
States: UNSEEN (absent from table) | SEEN.

Transitions:
- leaf() finds a witness pair within b  -> insert SEEN, emit edge
  (emit exactly once: insertion returns whether we won the race;
  only the winner emits — under one mutex this is atomic).
- open() case 2 fires                  -> insert SEEN, emit, prune.
- open() sees SEEN                     -> prune.

Correctness: an edge (g,f) is emitted iff some particle pair within b
exists (case 2 guarantees existence geometrically; leaf witnesses are
existence). SEEN is only ever set at a moment an edge is emitted, so
suppression never suppresses an unemitted edge. Redundant concurrent
searches can at worst emit... no — they cannot double-emit (single
winner rule); they waste work only. Termination: unchanged from v1.

Non-uniform node pairs (mixed fragments on either side) never consult
the table and descend under case-1 pruning as today; uniformity is
hereditary, so every descent eventually reaches uniform territory or
leaves.

### 3a memory analysis (raised in review, 2026-07-18)

Two distinct quantities:

- **Edge buffers are bounded by dedup, independent of traversal
  duration**: SEEN makes each (g,f) emit once per process, so the
  buffer is the process's share of distinct pairs in E(M) — at target
  scale (|E(M)| ~ 1e8, 100s of processes) ~1e6 edges x 16 B = tens of
  MB/process. No accumulation risk; UF_2 consuming late costs nothing
  here. (When step 4 streams edges via htram, buffers shrink to flush
  windows.)
- **What 3a does NOT bound: transient state of redundant concurrent
  descents** over a hot (g,f) before its first witness lands
  (traversal bookkeeping, Resumer wait lists; node storage is NOT
  multiplied — the cache dedups fetches by key). This per-key
  concurrency is exactly the 8.3 dedup ratio and is the quantity 3b's
  parking eliminates. Note UF starvation is not a concern in any
  stage: UF_2 is phase-decoupled from the walk by the frozen-tip
  invariant (v1-3b: consumed after QD; step 4: streamed and consumed
  incrementally).

## 4. 3b: IN_FLIGHT and parked lists — the searcher-identity rule

The design note's §4.1 says: UNSEEN -> mark IN_FLIGHT and descend;
IN_FLIGHT -> park. A naive reading deadlocks: the searcher's *own
children* are also node pairs over (g,f) (uniformity is hereditary),
so they would observe IN_FLIGHT and park — the search would suffocate
itself.

**Rule that resolves it — consult the table only on key acquisition.**
A node pair consults the pair-state table iff its parent pair was NOT
already uniform over the same (g,f) (i.e., this pair is where the
descent first *acquires* the key; includes pairs that arrive already
uniform from the traversal's top level). Descendant pairs inherit
searcher status structurally and never touch the table. Consequences:
exactly one table transition per arriving top-level uniform pair;
"the search" = the whole descent subtree below the acquiring pair;
parking applies only to later-arriving acquisitions of the same key.

Parked entry: enough to re-issue the walk later = (source node key,
partition/target id, traversal context). Parked pairs are recorded on
the table entry and are NOT walked.

Transitions (all under the entry's lock):
- UNSEEN    --acquire-->  IN_FLIGHT(searcher), descend.
- IN_FLIGHT --acquire-->  park the pair on the entry's list.
- IN_FLIGHT --witness-->  SEEN; emit; **discard parked list**
                          (processed += list length; see §5).
- IN_FLIGHT --search-exhausted, no witness--> release:
      state back to UNSEEN, then **re-issue every parked pair** as
      fresh work (each re-issued pair will re-acquire; the first
      becomes the new searcher). Do NOT mark SEEN (§4.1: "no edge
      between g and f" is not a global fact — another node pair's
      geometry may still witness it).

Search-exhaustion detection is the delicate part: the searcher is a
branching descent completing asynchronously across cache misses. Each
search carries a per-search outstanding-pair counter (created at
acquisition, incremented per child pair issued, decremented at each
pair's completion); zero with no witness = exhausted. This counter is
process-local state on the table entry — no messages needed.

Re-issue mechanism (the one new library surface): a group entry
`reissuePair(source_key, target_id)` that restarts the traversal at
that node pair — implementable via the existing Resumer/goDown
machinery (same shape as resuming after a cache miss). Sketch to be
reviewed at implementation; the design constraint is only that
re-issued work is *message-driven* (so it is visible to QD counters
as below).

## 5. 3b: counter-based quiescence accounting

**Application-level, not RTS** (clarified in review): the counters are
plain members of the FoFPhase1 group branches, and detection is our
own reduction loop — each branch contributes (created, processed) via
an ordinary async reduction; the target declares termination when the
sums are equal AND unchanged from the previous round, else broadcasts
the next round (ACIC continuous-reduction pattern). CkStartQD is not
used in 3b, and the RTS's internal QD counters are not touched.
Branches start contributing rounds once locally created == processed.

Why not RTS QD: in 3b-without-htram it is actually still sound via the
§4.1 by-construction invariant (every parked list's key has a searcher
with an outstanding cache request; unpark/discard happen inside that
searcher's completion handlers) — but that argument is exactly what a
cache-hit fast path silently breaks (the note flags this), and htram
later makes tram-buffered messages invisible to RTS QD anyway, which
would force this machinery to be built twice. The pair-lifecycle
counters depend only on local counting discipline, never on message
visibility, so they carry to htram unchanged.

Per-PE counters, following §6.3a with the parked-list amendments:

- `created`   += 1 when a node pair is enqueued: issued into the walk,
               spawned as a child pair, parked, or re-issued after a
               release (a re-issue is a NEW enqueue; see invariant).
- `processed` += 1 when a node pair is pruned, completes its leaf
               work, or is discarded from a parked list (list discard:
               processed += list length). A parked pair that is
               *re-issued* is first counted processed (unpark) — its
               re-issue increments created, keeping the invariant
               local and simple rather than tracking identity.

Invariant: `sum(created) - sum(processed)` == number of node pairs
that are active, suspended on a cache miss, or parked, across the
process... across all processes. Termination = the two sums are EQUAL
and UNCHANGED across two consecutive global reductions (the paper's
double-equality guard against in-flight counter updates).

Why a suspended-on-cache-miss pair is covered: it was created and not
yet processed, so the difference stays positive regardless of message
visibility — counter QD does not rely on the RTS seeing the cache
request. This is what makes the scheme robust to htram later: when
aggregated messages become invisible to RTS QD, the counters already
never depended on message visibility, only on pair lifecycle.

The QD hazard reduced to one auditable rule: **every path that makes
a pair stop being walked must increment processed exactly once**
(prune, complete, discard), **and every path that makes work exist
must have incremented created first** (issue/spawn/park/re-issue,
counting before the work becomes visible to others). Review focus:
release-and-reissue (unpark: processed++, re-issue: created++ — order
matters: created++ BEFORE the entry's lock is dropped, so the sums
can never transiently claim completion while a re-issue is pending).

At verified termination, CkEnforce on every process: pair-state table
contains no IN_FLIGHT entries and no non-empty parked lists. This
assertion is the tripwire for the whole hazard class.

## 6. Validation

3a (matrix = fof3's 8 runs + 10k.tipsy added, x oct):
- End-to-end serial comparison unchanged (partition equality).
- New counters printed per run: prunes by case (negative / positive /
  suppression), per-pair redundant-descent count (§8.3/8.4 data, the
  go/no-go input for 3b), and — answering the memory question with
  data — peak edge-buffer size per PE and peak simultaneously-active
  node pairs per process (high-water marks, cheap to track).
- CkEnforce annotation-validity on every internal node the visitor
  consults (catches any starter-pack/ordering regression).
- Non-vacuity: deliberately break (a) SEEN marking (mark before
  witness) and (b) the preTraversalFn reordering, confirm the serial
  comparison FAILS, revert. (Both breakages must be observable or the
  harness is not protecting us.)

3b (same matrix):
- Identical final partitions to 3a on every run (3b changes only
  scheduling, never the edge set semantics).
- Termination tripwire assertions (§5) on every run.
- Soak: the 2-proc x 2-PE 1k and 10k configurations repeated >= 20x
  (parking races are timing-dependent; single green runs mean little).
- A constructed parking stress: dense-pair input (two tight clusters
  straddling the process split) where the parked-list path
  demonstrably engages (counter shows parks > 0), plus the
  search-failure path demonstrably engages (parks released > 0) —
  if a natural input doesn't produce releases, add a temporary debug
  knob that forces the first searcher to skip its witness, and check
  the re-issued search still finds the edge.

## 6a. 3a results (2026-07-19, post-implementation)

- Full 12-run matrix green; serial comparison exact (72/390/3549
  components). Non-vacuity: premature-SEEN broke 254/1000 labels
  (FAILED as required); un-reordering tripped the validity CkEnforce
  on PE 2. Counters deterministic across 5x soak.
- **Redundancy ratios ~0.32-0.36 extra both-uniform descents per
  unique pair** (1.0 at 2proc x 1PE 1k, small-number regime). At this
  scale 3b's parking would eliminate almost nothing; **3b deferred
  until larger/denser data shows ratios that justify it** (revisit
  with the 8.3 measurements on realistic inputs).
- **The positive certificate never fired** (0 across the matrix, and
  in transient probes at 3.5x and 12.5x b): depth-first descent
  reaches a leaf witness (which marks SEEN) before any interior pair
  goes entirely-within-b, and suppression precedes the certificate
  check. With suppression-first ordering, case 2 can save at most the
  first descent per pair; its value should be reassessed at high
  overdensity (design note predicts D = 1e4-1e6 is its regime) rather
  than assumed. maxdist2 is covered by permanent unit checks in the
  fof3 harness since the path has no organic coverage.
- Latent bug fixed in Driver: TreeCanopy re-sends canopies each
  accumulation round and recvTC appends, so storage holds multiple
  generations per key; unstable sort shipped an arbitrary one.
  stable_sort + keep-newest-per-key + re-sort invalidation in recvTC.
- Particle::group_number now initializes to -1 (zero-filled heap
  masqueraded as valid "fragment 0" annotations, which would have
  made the validity CkEnforce vacuous).

## 6b. Scale campaign results (2026-07-19, 100k-16M)

Grid-hash exact reference (O(n)) cross-validated vs O(n^2) at <= 10k,
then used through 16M. All runs green with exact partition match;
component count + histogram bit-identical across configs per input.

- 100k Plummer: 33,933 comps (max 26,042). 1M: 333,889 (max 259k).
  8M: 2,657,656 (max 2.06M). 16M: 5,317,213 (max 4.09M). Plummer
  shows 2 giant components at every scale (generator mirrors two
  offset half-models).
- 100k uniform at b = 0.2 mean spacing: NO giant component (max 3,
  96.6% singletons) — consistent with continuum percolation
  (threshold ~0.87 n^-1/3; 0.2 is deep subcritical). A percolation
  stress test wants b ~ 0.7-0.9 mean spacing; the design note's
  near-percolation concern applies to clustered cosmological data,
  not uniform boxes at 0.2.
- 3b go/no-go CONFIRMED DEFERRED: redundancy 0.27-0.44 extra
  descents per unique pair at all scales (absolute: 1.5k redundant
  of ~118M opens at 8M). Suppression itself earns its keep (~7
  suppressed re-opens per unique pair at 8M); peak edge buffer
  2,524 edges (~40 KB) — memory bound loose by orders of magnitude.
- Positive certificate: 0 fires at every scale including 2M-particle
  fragments. 6a conclusion stands.
- Perf notes for later (correctness-grade timings, macOS): 8M
  2p x 2PE full iteration 23 s (walk 14.2 s); 16M +p2 walk 162.9 s —
  walk grew superlinearly at +p2 (2x data, 1.84x leaf visits,
  3.4x time): investigate when phase-3 performance work starts.
  FragCheckVisitor full sweep is quadratic; gated to <= 100k.
- Peak RSS 5.4 GB at 16M (32 GB machine); 80M correctly out of
  reach on this hardware.

## 6c. Certificate/redundancy stats on REAL cosmological data (2026-07-20)

First run of the step-3a prune counters on clustered/filamentary data
(LAMBS large-scale-structure subsample,
paratreet/examples/lambs.00200_subsamp_1M; 6a/6b were synthetic Plummer
+ uniform). 1M, 2 proc x 2 PE, -d oct.

**Redundancy ratio jumps ~50x on clustered data** (the halo-dominated
regime, design note 3.2):

| 1M, 2px2PE  | negative   | positive | suppression | same_frag | unique pairs | redundancy |
|-------------|------------|----------|-------------|-----------|--------------|------------|
| Plummer     | 13,470,648 | 0        | 4,890       | 1,582,921 | 866          | 0.30       |
| LAMBS       | 11,567,085 | 0        | 1,010       | 1,296,791 | 123          | 16.1       |

LAMBS has FEWER boundary fragment pairs (123 vs 866) but each is ~16x
"hotter" (16 redundant both-uniform descents per pair vs Plummer's 0.3).
Few pairs, each hammered. **This re-opens 3b (parking):** deferral was
right for synthetic (0.3x, nothing to gain), but clustered data shows
high per-pair redundancy, and absolute redundant work = boundary_pairs x
ratio grows with process count and resolution. Revisit at higher process
counts / full resolution. (same_frag = 1.3M confirms dense uniform
subtrees are ubiquitous — but same-fragment, so pruned instantly, not the
positive-certificate case.)

**Positive certificate stays dead THROUGH percolation** (b_factor sweep,
LAMBS 1M, -c stats):

| b_factor | positive | suppression | same_frag | components | max_size          |
|----------|----------|-------------|-----------|------------|-------------------|
| 0.2      | 0        | 1,010       | 1,296,791 | 379,884    | 19,350            |
| 0.3      | 0        | 3,492       | 1,702,742 | 291,504    | 24,880            |
| 0.5      | 0        | 7,771       | 2,053,333 | 191,682    | 51,408            |
| 0.7      | 1        | 14,270      | 2,320,675 | 130,016    | 241,479           |
| 1.0      | 0        | 35,070      | 2,974,377 | 72,167     | 809,181 (81% of N)|

Percolation happens between b=0.5 and 0.7 (max_size 51K -> 241K -> 809K).
The positive certificate fired EXACTLY ONCE (b=0.7, at the transition), 0
elsewhere including full percolation. **Case 2 is structurally subsumed by
suppression (case 3) regardless of density** and could be removed with
negligible effect, for two reasons: (1) maxdist(A,B) <= b between DIFFERENT
fragments is near-self-contradictory (whole boxes within b => their
particles are FoF-linked => same fragment, not two); (2) DFS reaches a
leaf-level witness (emits edge, marks SEEN) before descending to an
internal pair small enough for maxdist <= b. The design note's predicted
D=1e4-1e6 regime for case 2 is not reached by these subsamples, but the
structural argument holds across the whole physical b range. Suppression
does all of case 2's work and fires first (grows 1,010 -> 35,070 with b).

## 6d. Redundancy is CONCURRENCY-driven; 3b is a framework change (2026-07-22)

Starting 3b, two findings changed the picture.

**(1) The pre-witness redundant descents scale with real PE concurrency,
not just b.** 3a's SEEN suppression already eliminates the *post-witness*
redundancy (subsequent node-pairs over a resolved (g,f) prune via
`suppression`); what 3b uniquely parks is the *pre-witness* window --
`both_uniform_descents`, the concurrent descents that start before the
first witness lands. That window grows steeply with how many PEs descend
simultaneously. 1M Plummer, -u dist, b_factor 0.8, `++local`:

| config            | procs | both_uniform_descents | suppression | ratio  | phase3_walk |
|-------------------|-------|-----------------------|-------------|--------|-------------|
| +p4 ++ppn 2       | 2     | 16,621                | 137,055     | 35x    | 0.246 s     |
| +p8 ++ppn 2       | 4     | 203,975               | 1,295,301   | 174x   | 0.480 s     |
| +p16 ++ppn 2 (OS) | 8     | 218,488               | 1,841,659   | 139x   | 0.652 s     |

The 2->4 jump is ~12x (a fully-subscribed 8-core box); +p16 is
oversubscribed (16 PEs on 8 cores) so real concurrency -- and the window
-- saturate. So the pre-witness window is bounded by *actual concurrent
PEs*: negligible at low concurrency (2 procs: ~0.2% of walk opens, which
is why 6b saw "nothing to gain"), but ~1.6% of opens and climbing at full
subscription, and it would be far larger on a real cluster of thousands of
concurrent PEs -- each redundant descent also being a *remote* subtree
fetch there, not a loopback one. This is consistent with 6c's LAMBS 16x
(clustered + higher effective concurrency) and confirms 3b's payoff is a
production-scale/real-network effect, NOT demonstrable as wall-time on this
8-core loopback setup.

**(2) 3b is a framework-level change, not a visitor tweak.** The
searcher-identity rule (§4) needs node-key ancestry to tell a searcher's
own descent from a redundant root, but the visitor's `open()` receives
`SpatialNode`, which carries `depth` but NO Morton `key` (the key lives on
the `Node`/`NodeWrapper` wrapper, Node.h:237). And §4.1's re-issue
(`reissuePair(source_key,...)` via Resumer/goDown) plus §5's counter-QD are
also framework-level. So 3b touches the core traverser/resumer (thread keys
into the walk, add re-issue, counter-QD) -- a substantial piece, correctly
anticipated by §4-5 but bigger than "extend the visitor."

**Instrumentation fix (committed):** the `FOF3STAT redundancy ... ratio`
divided by `edges_unique`, which is hard-0 on the `-u dist` path
(FoFPhase3.h:418), so it always printed 0.000 on the scaling path. Now
divides by `edges_sent` (distinct pairs, populated on both paths); the
table above uses the corrected ratio.

**Instrumentation for the cluster run:** `FOF3STAT balance:
redundant_descents min/avg/max over PROCESSES` now reports per-process
skew (via a post-walk deposit into a per-process accumulator, read back
through the phase3Stats min/max using the SMP process-wide trick) -- the
2-4x skew already visible on loopback answers "is redundancy concentrated
on a few dense-boundary nodes?". `examples/fof3/redundancy_sweep.sh` sweeps
process (node) count at fixed PEs/process and tabulates redundancy + ratio
+ walk time + the per-process skew; point it at the 80M LAMBS with one
process per node (`LAUNCH` overridable for the cluster scheduler).

**Go/no-go:** 3b's benefit is real but scales with production concurrency
and real-network fetch cost; it is not measurable on available hardware.
Given it is framework surgery, validate on a real multi-node cluster (or a
clustered LAMBS run at higher process counts) before committing the
implementation. The instrumentation now reports the right ratio to drive
that call.

## 6e. The redundancy is EXTREMELY concentrated in a few pairs (2026-07-22)

Added a per-(g,f) descent-count histogram (`FOF3STAT
redundancy_concentration`: distinct pairs, max-per-pair, log2 histogram;
FoFPhase1Node::redundancyHistogram). First loopback result, 1M Plummer,
4 procs, b_factor 0.8:

```
distinct_pairs 77  max_per_pair 56823  avg_per_pair 2247
log2_histogram: 0:38 1:19 2:7 3:1  7:1 8:1 10:4 11:1 13:1 14:2 15:2
```

**~5 fragment pairs (bins 13-15) hold ~85% of all 173k redundant
descents; one pair alone drew 56,823.** The other ~64 pairs have 1-7
descents each (trivial). So the pre-witness redundancy is not spread --
it is dominated by a tiny number of HOT pairs, almost certainly the giant
percolating fragment against its few big neighbors (design note §6.3e).

Implication for the go/no-go: whatever mechanism we build -- priority-queue
descent, IN_FLIGHT parking, OR giant-fragment splitting -- only has to
handle a handful of pairs to capture the bulk of the win. This makes the
giant-fragment split (§6.3e) look especially relevant: the hot pairs ARE
the giant fragments. On the cluster, watch whether distinct_pairs stays
small and max_per_pair grows with N/processes (the giant fragment gets
bigger and hotter) -- if so, a targeted fix on the top-k pairs beats a
general per-descent mechanism.

Honest limit: the histogram shows concentration and magnitude, not the
within-search-fanout vs across-search-pileup MECHANISM split (that needs
the source/target node keys the visitor lacks -- the same framework
blocker as 3b's searcher-identity rule and the dual-tree question in 6f).
Concentration + per-process skew (§6d) are the cheap signals we can get
now; the exact split comes with the key-threading.

## 6f. Why the phase-3 walk is not a symmetric dual tree (2026-07-22)

Raised in review: the walk (`startDown` -> `TransposedDownTraverser`) is
SOURCE-DRIVEN -- it descends the global (source) tree while the target
side is a FLAT set of local leaves (buckets), not a descended tree
(Traverser.h recurse(): a source node is tested against each still-active
target leaf individually). So a target-side INTERNAL node's box is never
used to prune a whole block of target leaves in one test; each leaf is
tested against the source node separately. A true dual tree would compare
(source node, target node) and prune whole target subtrees when their
boxes are > b apart.

Consequence: the case-1 negative prunes -- which DOMINATE the walk (7-8M
vs ~10^2-10^5 redundant descents) -- are paid per (source-node,
target-leaf); a symmetric dual tree would collapse far target subtrees
into single prunes. So target-side descent is a potentially large phase-3
performance lever, plausibly bigger than 3b (negative_prunes >>
both_uniform_descents). It's inherited from ParaTreeT's gravity traverser
(targets = per-bucket force-evaluation points); switching phase 3 to a
symmetric dual tree (or the existing BasicDownTraverser/a new one) is a
framework change filed under phase-3 perf. Measure the negative-prune
reduction before committing.

## 7. Explicitly deferred past 3b

htram aggregation for edge emission (counters above are already
htram-proof); distributed UF_2 (step 4); giant-fragment splitting;
PBC; suppression-aware starter-pack contents (ship annotations only
when valid — cosmetic once ordering is fixed).

Orthogonal to 3b (do not conflate): edge-submission streaming
(design/edge-submission.md) is submission-side and bounds edge-buffer
MEMORY; 3b parking is discovery-side and cuts walk COMPUTE (the
pre-witness redundant descents). Same SEEN table, different resource;
independent — either, both, or neither.
