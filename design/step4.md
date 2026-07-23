# Step 4: distributed UF_2

Status: implemented and validated, 2026-07-20. Replaces v1/3a's
gather-to-one serial UF_2 (design/phase3.md) with a real distributed
union-find over the merge-graph edges, selectable via fof3's `-u dist |
serial` flag (default: `dist`; `serial` kept for A/B and as a fallback).
No htram/aggregation this step (plain sends only), per the design-note
sequencing.

## 1. Import

Copied from the sibling checkout `~/software/clusterFinding/unionfind`
(read-only) into `src/uf2/`: `unionFindLib.{h,C,ci}`, `types.h`, and
`prefixLib`'s `prefixBalance.{h,C,ci}` (renamed flat into `src/uf2/`,
provenance noted in `unionFindLib.h`'s header comment). Compiled into
`libparatreet.a` (simpler than a separate `libuf2.a`; `src/Makefile` gained
rules for `uf2/*.ci`/`*.C` and `UF2_OBJS` folded into the archive).
`src/Makefile.common`'s `INCLUDES` gained `-I$(PARATREET_PATH)/uf2`.
`src/paratreet.ci` now has `extern module unionFindLib;` unconditionally
(every paratreet app links the library; only FoF instantiates its chares).

**`unionfind-bugfixes.patch` applied**, both hunks:
- Zero-initializing `UnionFindLib()` constructor (`myVertices(nullptr),
  numMyVertices(0)`) — applied cleanly.
- `set_component`'s recursion rewritten as an explicit work-queue (avoids
  stack overflow on deep, uncompressed chains) — the patch's context missed
  (the sibling checkout had already drifted to `int64_t my_parent`) and was
  applied **manually**, reproducing the patch's "b" side verbatim; verified
  byte-identical to the patch's intended result by inspection.

**AGGREGATION (htram)/PROFILING/ANCHOR_ALGO stripped**: `unifdef -U` on all
four files removed the compile-time-disabled branches, then the remaining
unconditional htram surface (the `#include "htram_group.h"`, `tram_proxy_t`
typedefs, `myTramProxy`/`tram` members, `set_tram_proxy`/`flush_buffers`/
`quiesce`'s tram branch, the two htram readonlies) was deleted by hand —
`unifdef` only strips `#ifdef` blocks, and several htram references in this
library were unconditional (present regardless of `AGGREGATION`). This
import is plain point-to-point sends only, matching the "no htram this
step" decision.

**Trap (b) fixed** — `local_union`'s hard-coded vertexID decode
(`chare = vid >> 32`, `idx = vid & 0xFFFFFFFF`) was inconsistent with the
registered `getLocationFromID`. Both local lambdas (`arrIdx`/`chareOf`) and
the crossed-boundary `findBossData` construction now call the registered
function, so the application's encoding (see §2) is authoritative
everywhere in the library, not just on the paths that happened to call
`getLocationFromID` explicitly.

**New library-side additions** (beyond the patch, needed to embed the
library the way step 4 needs it — see design decision 2's array-per-vertex
vs. array-per-process framing): `UnionFindLib::unionFindInitOnePerNode`
(one chare per FoF process, placed via a new `UFNodeMap : CkArrayMap` on
`CkNodeFirst(node)`, instead of the original `unionFindInit`'s "shadow
array bound to a client array" — there is no 1:1 "client array" here, tips
are per-process fragments, not one array element per vertex);
`passLibGroupID` gained a `CkCallback` contributed once every element is
wired (avoids a race against the caller's next broadcast); `union_requests`
batches a PE's edge buffer into one marshalled message instead of one entry
invocation per edge; `initialize_vertices` now resets `parentCache`/boss
counts so a library instance is safe to reuse (not exercised this step —
fof3 always runs one iteration — but cheap and correct to have).

## 2. Tip encoding (design decision 2, Option C)

Process tips (assigned by phase 1) are renumbered to a UF_2 vertex id that
carries its own owner:

```
encoded_tip = (owning_process << 40) | dense_index
```

`dense_index` is a per-process-dense enumeration of that process's own
fragments (`paratreet::uf2EncodeTip`/`kUF2IdxBits`/`kUF2IdxMask`, in
`src/FoFPhase1.h`). `getLocationFromID` (`paratreet::uf2LocationFromID`)
decodes with pure bit math — `{vid >> 40, vid & mask}` — no directory, no
communication, O(1), and it scales: the alternative directory-of-owners
(Option A) was flagged in the design decision as an all-gather that gets
expensive at high tip density (~5.3M tips at 16M scale); this needs zero
bytes of directory state at any scale. 40 index bits allow ~1.1e12
fragments per process (never binding); the remaining 24 bits address up to
~16M processes.

**Why encoding must happen before the walk, not after** (this is the crux
Option B failed on): the phase-3 walk reads `Particle::group_number`
directly, including on cache-shipped remote copies, and emits edges as
`(remote_tip, local_tip)` pairs. If encoding happened only after the walk,
a process holding an edge to a remote tip `g` would have no way to learn
`g`'s dense index (only `g`'s owner knows its own encoding). So encoding
runs BEFORE `Subtree::upwardPass`/`Driver::loadCache`/the walk — the same
class of ordering hazard step 3 already fixed for annotation validity, and
fixed the same way (do the rewrite before the thing that ships it):

```
runFoFPhase1 (relabel)
  -> fof.countFragments        (builds frag_counts: process-tip -> size)
  -> fof_node.computeTipEncoding  (frag_counts keys -> encode_map + UF_2
                                    vertex array, dense-indexed)
  -> fof.applyTipEncoding      (owner-writes: particles' tips -> encoded)
  -> upwardPass -> loadCache   (annotations/starter pack now carry encoded
                                 tips; FoFEdgeVisitor is UNCHANGED — it
                                 never has to know encoding happened)
```

`examples/fof3/FoF3.C`'s `preTraversalFn` runs this sequence for `-u dist`
only; `-u serial` is byte-for-byte the v1/3a path (tips stay as
min-particle-order values throughout, no encoding).

**Correctness fix found along the way**: the phase-3 SEEN table and the
per-PE edge dedup packed an unordered `(tip, tip)` pair into a single
`uint64_t` (min tip in the high 32 bits, max in the low 32). That was exact
for step 1-3 tips (particle orders, always `< N`) but silently truncates
step 4's wide encoded tips — two DIFFERENT pairs could alias onto the same
packed key, causing a false "already SEEN" suppression, i.e. a real edge
silently dropped. Fixed by widening the key to `paratreet::TipPairKey`, a
two-`uint64_t` struct compared by exact equality (only its *hash* combines
lossily, which is fine — `unordered_set` resolves same-bucket entries via
`operator==`, not the hash). Applies uniformly to both modes; harmless for
`-u serial`'s always-small tip values.

## 3. Flow (`FoFPhase1::initUF2`/`fireUF2Edges`/`verifyEncodedTips`/
`applyUF2Labels`, `FoFPhase3.h::runFoFPhase3Dist`)

```
verifySharedLeaves -> resetPhase3
-> startDown<FoFEdgeVisitor> + CkWaitQD          (walk; edges are already
                                                    encoded-tip pairs)
-> phase3Stats                                    (counters only; no
                                                    gather-to-one this path)
-> UnionFindLib::unionFindInitOnePerNode          (one chare/process, fresh
                                                    per call)
-> fof.initUF2      (home-PE-only: registerGetLocationFromID +
                      initialize_vertices, handing the library a POINTER
                      into FoFPhase1Node::uf2_vertices -- the library
                      mutates that storage in place, so no gather is needed
                      to read results back)
-> fof.fireUF2Edges (each PE submits its edge_buf3 as one batched
                      union_requests message)
-> CkWaitQD()        (message-driven completion of the union cascade)
-> uf_proxy.find_components  (library's own internal QD, via
                               register_phase_one_cb-equivalent machinery
                               inside start_component_labeling)
-> fof.applyUF2Labels   (owner-writes: encoded tip -> componentNumber,
                          read straight out of uf2_vertices)
```

`verifyEncodedTips` (CkEnforce that every registered particle's tip decodes
to this process's own node bits and a dense index within its own vertex
array) is the step-4 counterpart of the existing tip-sentinel check, run in
both check modes exactly as the original.

## 4. QD strategy (deviation from the design decision's wording)

The design decision said "completion via CkStartQD (armed by
`register_phase_one_cb` on chare 0)". I used a plain `CkWaitQD()` after
firing the edges instead of calling the library's `register_phase_one_cb`.
Both are the identical mechanism (`register_phase_one_cb` just calls
`CkStartQD(cb)`, restricted to being invoked on array index 0); `CkWaitQD()`
avoids that index-0 restriction and matches the shape already used
everywhere else in this codebase (the v1 walk's `CkWaitQD()` after
`startDown`). `find_components`' own labeling-phase completion is handled
internally by the library (its `start_component_labeling` arms
`CkStartQD` on index 0 automatically) and needed no extra code here.

This is sound with plain sends (no htram) for the same reason the v1 walk's
QD is sound (design/phase3.md): all work is entry-method-driven, nothing is
buffered outside the RTS's visibility. When htram enters (later step),
tram-buffered messages become invisible to RTS QD, forcing the counter-QD
design of design/step3.md §5 for the WALK; this note is a placeholder that
the same argument will eventually need to be revisited for the union
cascade's completion detection if/when it is aggregated too (out of scope
this step — no aggregation, per the task's explicit exclusion of htram).

### 4a. htram aggregation ON (2026-07-23) — the revisit happened

Kale's call: turn aggregation on now — with htram's frequent flushes (the
shipped tram config flushes every 10 us) the small-scale regression risk is
low, and at multi-billion-particle / 100+-process scale on real networks it
is likely beneficial, especially on clustered/filament data where UF_2
traffic grows. Changes:

- `../unionfind` now built with its DEFAULT `make PROFILE=` (AGGREGATION on,
  profiling off). paratreet2's `src/Makefile.common` gained an `AGGREGATION`
  toggle (default `-DAGGREGATION -DUNIONFIND`, disable with
  `make AGGREGATION=`) threaded through src and all examples via INCLUDES —
  REQUIRED to match the library build because `unionFindLib.h` changes class
  layout under the flag (ABI). `-DUNIONFIND` rides along because
  aggregation makes every client TU include `htram_group.h`, whose datatype
  selection needs it.
- The union cascade's completion in `runFoFPhase3Dist` is now, under
  `#ifdef AGGREGATION`, the library's `quiesce()` = htram's htramQuiesce
  loop (arm QD -> flush all buffers -> QD -> reduce residual buffered-item
  counts -> repeat until zero -> fire callback), replacing the bare
  `CkWaitQD()` which is UNSOUND under aggregation (buffered items invisible
  to QD -> silent dropped unions). htram-off keeps the plain CkWaitQD.
  Only this one QD needed changing: tram sends occur solely in the
  boss_send/anchor_send cascade (between fireUF2Edges and find_components);
  find_components' internal completion and the walk's QD see no tram
  traffic. The step-3 §5 counter-QD design remains unneeded — htramQuiesce
  provides equivalent tram-aware termination for the only aggregated phase.

Validation (laptop, classic Converse): fof3 12-run matrix PASSED with the
"Compiled with aggregation optimizations" banner (72/390/3549); 10k serial
vs dist match; 1M b0.2 -> 333,889 and b0.8 -> 41,315 both grid-verified;
5x 10k + 3x 1M-b0.8 multi-process repeats deterministic (QD-race probe);
fof1/annotate/searchAlgos rebuilt and pass. Timing parity at 1M b0.2:
uf2 0.342s (off) -> 0.292s (on), walk 1.169 -> 1.085 (run noise; no
regression). No htram-off uf2 baseline exists for b0.8 (0.619s on).
Reconverse/Anvil validation still pending — tram + reconverse QD is an
untested combination (the reconverse gate applies; see charm notes).

## 5. Harness fix: label-agnostic final comparison

`-u serial`'s gather-to-one UF_2 unions by min tip, so the final label
happens to equal "the order of the component's min-order member" — the
same canonical value the serial/grid reference computes, so the pre-step-4
harness compared raw labels directly. `UnionFindLib::find_components`
assigns componentNumber via a parallel-prefix boss count (`prefixLib`) —
an **arbitrary serial id**, not order-related at all. `examples/fof3/FoF3.C`
now re-derives, from the gathered records, the canonical min-order
representative per label group (`tip_min_order`, same pattern as
`examples/fof1/FoF1.C`'s `verifyPhase1`) before comparing to the serial
reference. This is applied uniformly to both modes; for `-u serial` it is a
no-op behaviorally (`tip_min_order[label] == label` always holds there),
so this is not a behavior change for the already-validated serial path,
only a widening of what the check tolerates.

Stats mode's component-count/histogram line and the fragment histogram are
unaffected by labeling scheme (both are counts-per-distinct-label, which a
bijective relabeling preserves) — no changes needed there, and they are
exactly the cross-mode determinism observable used at 8M (§7).

## 6. prefixLib

Imported (`src/uf2/prefixBalance.{h,C,ci}`) and used **unmodified**:
`UnionFindLib::find_components` calls it to assign globally-sequential
serial ids to root vertices (a hypercube parallel-prefix sum over local
boss counts, `Prefix` array bound 1:1 to the `UnionFindLib` array). Given
§5's label-agnostic harness fix, prefixLib's arbitrary numbering is exactly
sufficient — there was no need to replace it with anything that produces a
"meaningful" label. Kept as an internal implementation detail of
`find_components`; not otherwise touched.

## 7. Test matrix (2026-07-20)

**Small matrix** (`{100, 1k, 10k}.tipsy x {+p1, +p2, +p2 ++ppn1, +p4
++ppn2} x {serial, dist}`, 24 runs): all PASS, identical `FOF3STAT
components:` lines (full histogram, not just count) between `-u serial` and
`-u dist` at every one of the 12 configs, and identical to the pre-existing
3a numbers (design/step3.md §6a): 72 / 390 / 3549 components for 100 / 1k /
10k respectively.

**100k** (`-u dist -c full`, 2 proc x 2 PE): 33,933 components, max 26,042
— PASSED, exact match to design/step3.md §6b's recorded 33,933 (max
26,042).

**1M** (`-u dist -c full`, 2 proc x 2 PE): 333,889 components, max 259,128
— PASSED, matches §6b's 333,889 (max 259k).

**8M** (`-u dist -c stats` AND `-u serial -c stats`, 2 proc x 2 PE):
identical `FOF3STAT components:` lines both modes: 2,657,656 components,
max 2,055,507 — matches §6b's 2,657,656 (max 2.06M) exactly. This is the
determinism cross-check (design decision 4's requirement): dist and serial
agree on the full partition at 8M scale via the label-agnostic count/
histogram comparison.

## 8. uf2/edge_gather timing, serial vs dist

Measured on the same macOS laptop as design/step3.md's 6b campaign
(correctness-grade, not tuned; netlrts SMP, 2 proc x 2 PE, ++local/TCP
loopback):

| scale | unique edges (serial) | uf2 time, serial | uf2 time, dist | walk time (shared) |
|---|---|---|---|---|
| 1k   | 11      | ~0.000 s | ~0.005 s | 0.001 s |
| 1M   | 866     | 0.000 s  | 0.158 s  | ~0.62 s |
| 8M   | 3,558   | 0.001 s  | 0.487 s  | ~13.8 s |

("uf2 time, dist" = `initUF2 + fireUF2Edges + CkWaitQD + find_components`;
"uf2 time, serial" = the old gather-to-one serial map build.) At every
scale tested, the distributed path is **slower** than the trivial serial
gather-to-one UF_2 by roughly 2-3 orders of magnitude in absolute terms,
though both stay a small fraction of the walk time (which dominates total
iteration time at 1M/8M: uf2/dist is ~1.2% of the walk at 8M). The gap is
fixed distributed-systems overhead — `UnionFindLib` array creation, the
`prefixLib` hypercube prefix sum, and two full QD rounds — that does not
amortize until the edge count is large enough to matter, and the actual
UF_2 problem here (a few thousand edges over a few million fragments) never
gets there within the tested scales. This matches the v1 design note's own
framing ("[gather-to-one] fine to ~1e6 edges") — step 4 buys *scalability*
past that edge-count ceiling (no more O(all edges) gather to one PE), not
speed at the scales that ceiling was never a problem at. Whether the
crossover point is reachable at plausible target scales (the design note's
|E(M)| ~ 1e8 regime, 100s of processes) is not established by this data —
worth a dedicated scaling run before relying on step 4 for performance
rather than just correctness/scalability headroom.

## 9. Default shipped

`-u dist` is the default (`examples/fof3/Main.h`'s `UF2Mode::Dist`),
per the task's instruction ("make `-u dist` the default IF all tests
pass"). All required tests passed (small matrix identical both modes;
100k/1M dist full-check PASSED; 8M dist/serial stats-mode determinism
match). `-u serial` remains selectable for A/B and as a fallback if a
future scale/topology surfaces a distributed-path-specific issue the small
matrix didn't exercise.

## 10. Deviations from the task's design decisions (summary)

- QD via `CkWaitQD()` directly rather than `register_phase_one_cb` (§4
  above) — same mechanism, avoids the index-0 call restriction, matches
  existing codebase idiom.
- `unionFindInitOnePerNode` (new) instead of the library's own
  `unionFindInit` (client-array-bound shadow array) — there is no
  client array in this application's shape (design decision 2 already
  anticipated a custom placement: "one element per FoFPhase1 process").
- `TipPairKey` widening (§2) was not anticipated by the design decisions
  text but is required for correctness once tips can exceed 32 bits;
  flagged here as a fix found during implementation, not a deviation
  from intent.
- Harness label-agnostic canonicalization (§5) was explicitly anticipated
  by design decision 3 ("verify FoF3.C's comparison is label-agnostic...
  and if not, adapt the check") — implemented as anticipated, not a
  deviation.
- `UnionFindLib` array created fresh inside `runFoFPhase3Dist` on every
  call (no cross-iteration reuse). fof3 always runs a single iteration
  (`conf.num_iterations = 1`), so this is unexercised; an app that reused
  phase 3 across iterations would need to hoist creation out and rely on
  `initialize_vertices`'s per-run reset (already added to the library
  import for this reason) instead of recreating the array.
- Not implemented (out of scope per the task): `prune_components` (design
  decision 3 explicitly said skip), htram aggregation for the union
  cascade (explicitly out of scope this step).
