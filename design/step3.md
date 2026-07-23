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

## 6g. Opt-in node keys threaded to the visitor (2026-07-22)

Landed the framework prerequisite for the exact within-vs-across measurement
(and for 3b's searcher-identity). It is a GENERAL, opt-in capability, not FoF
weight on the core (the toolkit-boundary constraint): `maybeSetKeys` in
Traverser.h (anonymous namespace) sets `v.trav_source_key/trav_target_key`
before each open()/leaf() IFF the visitor declares those members; a SFINAE
no-op overload covers every visitor that doesn't (gravity, SPH, annotate,
searchAlgos), so their open()/leaf() signature and codepath are unchanged and
zero-cost. Verified: fof3/annotate/searchAlgos all build; FoFEdgeVisitor
receives valid keys at runtime (e.g. source 0x10a1); regression green.

Consumer (the acquisition-vs-continuation split = distinct-searchers vs
within-search fan-out) is the NEXT unit, and it has two subtleties I hit while
scoping it -- record them so the measurement is designed correctly, not rushed:

1. **Concurrency ordering.** recordRedundant funnels from all of a process's
   partitions/PEs (they descend the same source g-subtree against their own
   f-leaves), so a deep source node can be recorded before its shallow ancestor
   arrives from another partition. Any acquisition test that assumes
   parent-before-child (key-ancestry against a growing set, or "parent key in
   seen-set") will OVER-count acquisitions under out-of-order arrival.

2. **Remote-node parent reliability.** The clean O(1) test -- "acquisition iff
   the source node's PARENT is not uniform over the same g" (which also solves
   3b's self-park WITHOUT key ancestry: a searcher's children have a uniform-g
   parent = continuation = don't park; a fresh arrival has a mixed parent =
   acquisition = park) -- needs `source->parent`, which may be null or a local
   reconstruction for cache-shipped remote subtrees (the majority of descents
   in the distributed walk). Must verify parent validity on cached nodes before
   relying on it; keys are always valid, parent pointers may not be.

Note (nice discovery): the parent-uniformity test, if parent is reliable, gives
3b's searcher-identity rule with NO node keys at all -- reconsider whether 3b
even needs the key machinery, or just parent frag-data + the IN_FLIGHT table.

## 6h. Anvil 80M-LAMBS redundancy sweep: 3b is a NO-GO (2026-07-23)

Ritvik ran `redundancy_sweep.sh` on Anvil (80M LAMBS, 15 PEs/process,
b_factor 0.2, `-u dist`) — the §6d go/no-go measurement. Results (edges_sent
and skew derived from ratio = redundant/edges_sent and min/avg/max):

| procs | cores | redundant | ratio  | edges_sent | walk_s | per-proc avg | skew max/avg |
|-------|-------|-----------|--------|------------|--------|--------------|--------------|
| 1     | 15    | 98,207    | 0.000  | 0          | 83.129 | 98,207       | 1.00         |
| 2     | 30    | 100,463   | 19.652 | 5,112      | 29.779 | 50,232       | 1.005        |
| 4     | 60    | 114,944   | 7.075  | 16,247     | 7.719  | 28,736       | 1.14         |
| 8     | 120   | 123,167   | 5.517  | 22,325     | 2.816  | 15,396       | 1.14         |
| 16    | 240   | 134,671   | 4.079  | 33,016     | 1.265  | 8,417        | 1.55         |

**Verdict: NO-GO on 3b parking.** The concurrency-driven blowup extrapolated
from loopback (§6d: 6x growth 2→4 procs) did NOT materialize on the real
network:

1. **Per-process redundancy FALLS ~P^-0.89** (98k → 8.4k avg); total is
   near-flat (+37% over a 16x process sweep). Per-pair ratio falls
   monotonically 19.7 → 4.1 — redundancy dilutes as the pair set grows.
2. **The P=1 row is a control that reclassifies the counter.**
   `p3_redundant_descents` (FoFPhase3.h open()) counts EVERY both-uniform
   descent on an unSEEN pair — including the necessary first descent of each
   true-edge pair AND all descents over near-miss NO-EDGE pairs, which no
   mechanism can skip: SEEN never marks them (search failure ≠ no-edge
   globally, §4.1), and 3b parking re-releases parked lists on failure. At
   P=1 phase 1 resolves every link, M is empty (ratio 0.000 = edges_sent 0
   divide guard), so ALL 98,207 counted descents are unsavable verification
   work. That ~100k-class floor persists at every P.
3. **The parkable residual is marginal.** At P=16: 134,671 total − ~33k
   necessary first-descents (= edges_sent) − the no-edge floor ⇒ genuinely
   parkable pre-witness excess is order 10^4 descents in a 1.27 s walk on
   240 cores (~1% of walk core-seconds even at 100 µs/remote descent).
4. **The walk's perf story is the LOW-P regime, not redundancy**: 65.7x
   speedup on 16x cores (1,247 → 304 core-seconds of total walk work) — the
   §6b 16M superlinearity again. Big local working sets are the expensive
   regime, pointing at the §6f flat-target-leaf-list / dual-tree lever and
   cache footprint, not at redundant descents.
5. **Skew is mild** (max/avg 1.0 → 1.55). Watch it past P=16, but no
   giant-fragment redundancy pathology at these counts.

Side-finding (edge-submission.md): edges_sent grows ≈ P^0.9 (5.1k → 33k over
2→16 procs) — near the percolating/space-filling regime that doc predicts, so
the per-process edge buffer is roughly FLAT with P. Absolute numbers are tiny
(~2k edges/process at P=16), so batch submission stays comfortable and
streaming stays unwarranted at this scale.

Status: 3b (parking + searcher-identity + counter-QD) is RETIRED unless a
larger-P run reverses these trends (per-process redundancy or skew turning
upward). The framework pieces it motivated that are independently useful
(§6g key-threading) stay. Caveats: sweep tops out at P=16 (240 cores).
Ritvik confirmed b_factor 0.2 and all rows ok. Phase-3 perf work should
target §6f (target-side descent) and the low-P working-set superlinearity
instead.

**Concentration histograms (same runs, 2026-07-23) — the floor is
structural and the P-growth is a tiny hot tail.** Per-(g,f) log2 descent
histograms (bin k = pairs with 2^k..2^(k+1)-1 descents), P = 1/2/4/8/16:

```
P=1:  0:50384 1:15565 2:2392 3:219 4:5
P=2:  0:50433 1:15591 2:2398 3:231 4:14 5:4 6:1 8:1 9:1 10:1
P=4:  0:50574 1:15693 2:2434 3:249 4:25 5:12 6:7 7:7 9:2 10:7
P=8:  0:50632 1:15710 2:2470 3:250 4:33 5:10 6:13 7:6 8:2 9:5 10:7 11:1
P=16: 0:50790 1:15817 2:2489 3:271 4:42 5:34 6:20 7:11 8:4 9:6 10:5 11:5
```

Two clean facts:
- **Bins 0-3 are frozen across P** (within ~1%): ~68.5k pairs, ~98k
  descents, 73% of pairs with exactly ONE descent — the §6h verification
  floor confirmed structurally, and maximally SPREAD, so no top-k/parking/
  splitting mechanism has anything to grab in it.
- **All P-growth is bins >=4**: tail pairs 5 -> 22 -> 60 -> 77 -> 127, and
  the tail's descent mass (~36k at P=16 by bin midpoints) equals the total's
  growth over P=1 (36,464) almost exactly. Total = flat 98k floor + a
  concentrated concurrency tail that IS the entire P-dependence. The tail is
  precisely what 3b parking would target: ~1% of walk core-seconds. NO-GO
  reconfirmed with the mechanism split measured, not inferred.

Corollary: the GIANT-FRAGMENT SPLIT (design note 6.3e) is DEMOTED at
production b=0.2 — the loopback alarm (§6e: one pair at 56,823 descents,
bins 13-15) was a b=0.8 percolation artifact; on real LAMBS at b=0.2 the
hottest pair is bin 11 (<=4,095 descents). Concentration SHAPE replicates
at scale; magnitude does not justify a mechanism. Watch item: max bin crept
4 -> 10 -> 10 -> 11 -> 11 over the sweep — revisit if a P~100s run shows
bin-14+ pairs.

**Cluster phase division (same runs, 2026-07-23): THE CRITICAL PATH HAS
INVERTED — phase 1 is now the bottleneck at scale.** Full `time_s` blocks
from Ritvik's logs (seconds; htram-off build — these are also the baseline
for the post-aggregation Anvil comparison):

| P  | phase1 | tip_encode | upwardPass | walk   | uf2   | relabel |
|----|--------|------------|------------|--------|-------|---------|
| 1  | 25.678 | 17.846     | 19.694     | 84.089 | 0.812 | 0.255   |
| 2  | 13.800 | 11.376     | 12.264     | 29.781 | 0.572 | 0.307   |
| 4  | 9.593  | 4.890      | 5.173      | 7.730  | 0.318 | 0.091   |
| 8  | 7.818  | 2.087      | 2.151      | 2.795  | 0.170 | 0.045   |
| 16 | 5.835  | 1.418      | 1.578      | 1.145  | 0.143 | 0.091   |

(component_histogram 1.9-3.2s is harness reporting, excluded; edge_gather/
loadCache/fragcheck/tip_sentinel all negligible.)

- **uf2 is sub-second and FALLS with P** (0.81 -> 0.14; P=1 = pure fixed
  overhead, zero edges). UF_2 definitively a non-factor at this scale;
  htram's justification stays the multi-billion/filament extrapolation.
- **The walk strong-scales superbly** (73x on 16x nodes, superlinear — the
  low-P working-set effect again) and is down to 11% of algorithmic time.
- **Phase 1 scales at only 4.4x on 16x nodes and is STILL FLATTENING**
  (8 -> 16 procs: 1.34x). At P=16 phase1 (5.8s) + tip_encode (1.4s) +
  upwardPass (1.6s) = 8.8s = ~86% of the ~10.2s algorithmic total, vs the
  walk's 1.1s. The laptop picture (phase1 "nearly free" at 4%) does not
  transfer: at production P, phase 1 IS the FoF runtime.
- tip_encode/upwardPass scale ~12.5x — acceptable, though their P=1 costs
  sit in the same cache-thrash regime as the walk's.

Undiagnosed: whether phase 1's flattening is (a) the per-process SERIAL
merge step of the frozen-phase scheme, (b) per-PE skew on clustered data,
or (c) a term scaling with N not N/P. The discriminator — `FOF3STAT
balance: phaseA_s/phaseB_s min/avg/max over PEs` — is already printed and
sits in Ritvik's same logs; get those 5 lines before designing a fix.
Consequence for priorities: dual-tree (§6f) now optimizes an 11% slice;
PHASE-1 STRONG SCALING is plausibly the bigger production lever.

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
