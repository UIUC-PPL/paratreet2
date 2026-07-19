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

## 7. Explicitly deferred past 3b

htram aggregation for edge emission (counters above are already
htram-proof); distributed UF_2 (step 4); giant-fragment splitting;
PBC; suppression-aware starter-pack contents (ship annotations only
when valid — cosmetic once ordering is fixed).
