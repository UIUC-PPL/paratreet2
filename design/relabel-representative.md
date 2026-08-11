# Representative-indirect relabeling and the sliced phase-3 label map

Design for agenda items 12 (representative-indirect relabeling, Kale's
proposal 2026-08-06) and 7 (slim the serial-mode phase-3 relabel
broadcast to per-process slices). One document because they compose:
item 12 builds the per-representative structure, and item 7's best form
(owner-sharded slices) becomes cheap only once item 12 has made tip
encoding a per-representative operation. Written 2026-08-10; not yet
implemented.

## 1. Current state: every pass that touches per-particle labels

After phaseA, a processor's particles never change component membership
at finer granularity than their frozen phaseA fragment. Every later
stage only RENAMES fragments. The code nevertheless re-visits every
particle with a hash probe at each renaming. The passes, in pipeline
order (all in fof/FoFPhase1.h unless noted):

1. **phaseA freeze** (`phaseABody`/`freezeAndAnnotate`): runs `find(i)`
   on every particle, writes `group_number = flat_order[root]`, and
   counts members per root (`root_counts`, the item-10 freeze-pass
   counting). Because `find` path-compresses and is called on every
   index, `uf_parent[i]` holds the root index directly for every i when
   the pass ends. `uf_parent` is `int` (PE-local flat indices),
   `flat_order` maps flat index -> global particle order. Both persist
   until `reset()`. This pass stays as is; it produces the asset the
   rest of the design uses.
2. **relabelBody** (after the process-level phaseB merge): for every
   particle, a hash probe into the process merge map `tip_map`
   (identity if absent). 2B/16 nodes: 0.22-0.3 s; the fof3-2b-scaling
   table shows 0.048 -> 0.3 s growing from 8 to 128 Frontier nodes.
3. **applyTipEncoding** (dist mode only): for every particle, an
   arithmetic rewrite to the owner-encoded id
   `(process << kUF2IdxBits) | tip`, plus a full rekey of `tip_counts`.
   2B: ~0.24 s.
4. **Phase-3 serial** (`runFoFPhase3` in FoFPhase3.h ->
   `applyGlobalMap`): processor 0 builds `map_vec` (one entry per
   edge-touched tip, value `-(root+2)`) and broadcasts the whole vector
   to the per-PE FoFPhase1 GROUP. Every PE then constructs its own
   `unordered_map` copy of the full global map and probes it once per
   particle. 2B/16 nodes: **2.84 s** (`relabel(p3)`), the number that
   motivates item 7. Three costs stack: the broadcast payload is
   copied per PE (1920 copies at 2B), the hash map is built per PE,
   and the probe runs per particle.
5. **applyUF2Labels** (dist mode): per-particle hash probe into the
   node-shared touched-label map (the map itself is already built once
   per process — only the probing is per-particle).
6. **rekeyTipCounts** after passes 2, 3, 4, 5: a hash pass over the
   per-tip counting map.

The exact 2B map size is printed as `tips_remapped` in serial-mode
logs; harvest it from the cost-2b job (19772491) log for the design's
final arithmetic. Order of magnitude: edge-touched tips only, bounded
by twice the unique cross-process edge count.

## 2. Item 12: apply maps to representatives, materialize by indexed load

Invariant the design rests on: after the freeze, all particles of a
frozen fragment carry the same label forever after; every subsequent
map is a function of the current label only. So a label map never needs
to be applied per particle — only per DISTINCT frozen root, of which a
PE has thousands (2B: ~12,500 roots/PE against ~1M particles/PE; 80M/4
nodes: similar ratio).

Structure added per PE (FoFPhase1 members, built during the freeze):

- `roots`: compact vector of the frozen root indices
  (`root_counts[r] > 0`), size = number of local fragments.
- `rep_label`: dense `long` array over the flat index space;
  `rep_label[r]` = the fragment's CURRENT label, initialized to
  `flat_order[r]` at the freeze. Non-root slots unused. Memory: 8 B per
  particle per PE (~8 MB/PE at 2B) — accepted for simplicity; a
  compacted variant (rank the roots, add an `int` rank array) saves
  most of it if ever needed.

Every label pass then becomes:

- **apply**: `for r in roots: probe map with rep_label[r]; update.`
  Thousands of probes instead of a million.
- **materialize** (only where a consumer actually reads particle
  labels): `parts[i].group_number = rep_label[uf_parent[i]]` — a pure
  indexed load, no hashing, one pass.

Materialization points — the pipeline needs exactly two:

- **M1**, before `TreePiece::upwardPass`: the upward pass annotates
  nodes from particle labels, and the phase-3 walk ships and reads
  them (FragData min/max fragment, SEEN suppression), so labels must
  be real in the particles by then. M1 replaces BOTH the relabelBody
  hash pass and the applyTipEncoding arithmetic pass: apply the merge
  map to `rep_label`, then encode `rep_label` in place (dist mode; see
  section 3 for serial), then materialize once.
- **M2**, after the phase-3 label application (either mode): apply the
  global map (serial) or the touched-label map (dist) to `rep_label`
  per representative, materialize once. The verification harness, the
  component counter deposit, and any writer read labels after M2.

Counting side: `root_counts` (dense, already built) replaces the
`tip_counts` hash map entirely. `depositLabelCounts` emits
`(rep_label[r], root_counts[r])` per root at deposit time;
`rekeyTipCounts` is deleted. The sign convention (negative = touched
by a cross-process edge, needs global summing) is carried by
`rep_label` values unchanged, so `histogramShard`/
`collectTouchedCounts` are untouched. `FOF_COUNT_VERIFY=1` keeps the
recount-and-compare debug path.

Safety: `uf_parent` must never be mutated after the freeze. Add a
debug-build flag set at freeze time and a `CkEnforce` in `unite()`
against post-freeze unions.

## 3. Item 7: owner-sharded slices instead of a full-map broadcast

Fragments are process-local, so the set of labels present on a process
is disjoint from every other process's. Each process therefore needs
only the map entries whose key is one of ITS labels — a slice about
1/P of the map. The obstacle today is that serial-mode tips are RAW
global particle orders, which carry no owner information, so processor
0 cannot shard the map.

Design: adopt the owner-encoded tip namespace in serial mode too.

- With item 12 in place, encoding is a per-representative arithmetic
  update folded into M1 — the historical cost objection to encoding on
  the serial path (a full extra particle pass) is gone.
- The serial union-find then runs over encoded tips, union by minimum
  ENCODED tip. This gives up the "global root = minimum particle order
  of the component" property inside the pipeline. The fof3 harness
  already tolerates arbitrary labels (it canonicalizes each label
  group by minimum order when comparing against references — added for
  dist mode, design/step4.md decision 3), and the component counter
  uses only the sign convention. Audit before implementing: FragCheck
  visitors, the stats-mode distributed checks, and any output writer
  for a leftover assumption that labels equal minimum orders.
- Processor 0 shards `map_vec` by `tip >> kUF2IdxBits`. Delivery is
  size-dependent (Kale, 2026-08-10):
  - **Slices adequately large (roughly 8-64 KB or more each): direct
    sends**, one point-to-point message per process to
    `FoFPhase1Node[p]`. P messages of |map|/P each cost microseconds
    of send overhead against megabytes saved. At 2B this is the
    expected regime: a map of order 1M entries over 128 processes is
    ~128 KB per slice.
  - **Slices small: keep the broadcast.** When the whole map is small,
    the current broadcast is already cheap, and sharding buys nothing;
    pick a total-size threshold (order 1 MB) below which the existing
    path is used unchanged.
  - **Large process count with small slices** (the corner where the
    root's send loop and per-message overhead both matter): scatter
    combined slices to ONE process per physical node, which
    redistributes to its co-located processes. Caveat recorded so the
    second hop is not overestimated: reconverse currently has NO
    cross-process shared-memory path (the CMK_USE_SHMEM producer half
    is missing; charm-notes 2026-07-26), so the intra-node hop rides
    NIC loopback (~2.2 us small messages), not IPC. The scheme still
    removes the root bottleneck; build it only when measurement at
    high process counts shows the direct-send loop matters.
- The node branch stores the slice hash ONCE per process; each PE
  applies it to its `rep_label` roots (per-representative probes into
  the shared map — no per-PE map construction at all), then M2 runs.
- Dist mode gets the same per-representative treatment of
  `applyUF2Labels` for free (its map is already node-shared; only the
  per-particle probing changes to per-representative + M2). Its
  labeling-request batching remains item 11, unaffected.

Expected effect on the 2B/16 numbers: the 2.84 s `relabel(p3)` term
contains (a) a full-map broadcast materialized 1920 times, (b) 1920
hash-map constructions, (c) 2e9 hash probes. After: (a) becomes ~|map|
total wire in 128 direct messages, (b) becomes 128 shared hashes,
(c) becomes ~1.6M representative probes plus one indexed-load pass
over the particles. Every term drops by two to three orders of
magnitude except the indexed-load pass, which is memory-bandwidth
bound and comparable to one iteration of the freeze loop (~0.1 s
scale). Predicted post-change relabel(p3): well under 0.5 s, dominated
by M2's materialization.

## 4. Staged implementation, each stage gated and measured separately

Per the standing requirement (walk->uf2 campaign): document the
measured benefit of EACH stage separately (laptop + 80M A/B + 2B
numbers appended here as they arrive).

- **Stage 1 — item 12 alone, no protocol change.** `roots` +
  `rep_label` + per-representative application in relabelBody,
  applyTipEncoding, applyGlobalMap (map still broadcast exactly as
  today), applyUF2Labels; M1/M2 materialization; counts from
  `root_counts`; delete rekeyTipCounts. Gate: bit-identical counts and
  histograms (16-run matrix, 1M both b values, serial vs dist, LAMBS
  1M), FOF_COUNT_VERIFY pass, reconverse set. Measure: phase1_stages
  relabel + tip_encode + relabel(p3) at 80M/4 nodes.
- **Stage 2 — encoded tips in serial mode.** Flip the serial pipeline
  to encoded tips (applyTipEncoding folded into M1 in both modes);
  audit and fix the label-convention assumptions listed above. Gate:
  same identity checks (canonicalized comparison), plus one 80M Anvil
  run serial vs dist cross-check.
- **Stage 3 — item 7 proper.** Shard at processor 0, direct
  per-process slice sends to the node branch, per-representative
  application, drop the group broadcast. Gate: identity as above;
  measure relabel(p3) at 2B/16 nodes against the 2.84 s baseline.

## 5. Open questions (Kale)

1. RESOLVED (2026-08-10). Module consumers: Kale — not a concern.
   Internal audit (this session): every reader of labels is
   label-value-agnostic — FragCheckVisitor compares particle labels to
   node annotations by equality only; the full check re-derives
   canonical representatives per label group (tip_min_order,
   design/step4.md decision 3); the FOF3STAT determinism line is
   counts and size histogram only; the component counter uses only the
   sign convention (which dist mode's encoded labels already exercise
   end to end); fof1 never runs tip encoding; the Writer does not
   output group_number. Encoded labels in serial mode are safe with no
   canonicalization pass needed.
2. RESOLVED (Kale, 2026-08-10): dense form. On the sparse-use cache
   question (raised and judged minor by Kale — confirmed by access
   pattern): the apply step touches only the compact roots vector
   (~thousands of entries), and the materialization loop reads
   `rep_label[uf_parent[i]]` for Morton-sorted particles, so
   consecutive particles nearly always hit the same line — about one
   distinct cache line per FRAGMENT, not per particle.
3. RESOLVED (Kale, 2026-08-10): transport is size-dependent — direct
   sends when slices are adequately large (8-64 KB+), the existing
   broadcast when the whole map is small, and a per-physical-node
   scatter with intra-node redistribution reserved for the
   large-P/small-slice corner (see section 3 for the reconverse
   loopback caveat on that second hop).

## Measured results

### 80M, Anvil 4 nodes/480 PEs (job 19774169, 2026-08-10, 3 reps x 2 arms)

All six runs exact (23,707,197 components) — first at-scale validation
of the full staged pipeline (owner-encoded serial tips, representative
application, sliced delivery machinery).

- phase1_stages relabel: **0.009-0.011 s** against 0.039-0.063 s
  measured pre-change the same evening on the same hardware (cost-probe
  job 19773099) — the stage-1 per-representative application is ~4-5x
  here.
- tip_encode (new in serial mode): 0.010-0.029 s, the folded
  per-representative encoding + materialization.
- phaseA 0.21-0.29 / phaseB 0.029-0.035 / merge 0.001 — unchanged.
- relabel(p3): 0.032-0.110 s, within the historical noise of this
  bracket at 80M.
- CAVEAT: the transport A/B did NOT arm — the map is 39,738 entries =
  636 KB, below the 1 MB gate, so both arms correctly chose broadcast.
  The stage-3 comparison rests on the 2B job (19774171), whose map
  should be well above the gate.

### 2B, Anvil 16 nodes/1920 PEs (job 19774171, 2026-08-11, 2 reps x 2 arms)

All four runs exact (424,897,832 components) — the full staged pipeline
validated at 2B. Map: 745,544 entries = 11.9 MB (well above the 1 MB
gate; sliced max_slice 19,223 against a 5,825 even split, i.e. ~3.3x
owner skew). relabel(p3), the stage this design targets:

| configuration | relabel(p3) |
|---|---|
| baseline (2026-08-05 campaign, pre-change) | 2.84 s |
| broadcast arm (stages 1-2: per-representative application) | 1.238 / 0.969 s |
| sliced arm (stage 3: per-process slices) | 0.174 / 0.144 s |

Decomposition: stages 1-2 alone are ~2.6x (the 2e9 per-particle hash
probes are gone; the full-map broadcast and 1,920 per-PE hash
constructions of an 11.9 MB map remain and dominate the residual);
stage 3 removes those for another ~7x. Net: 2.84 s -> ~0.15 s, about
18x, comfortably under the design's "well under 0.5 s" prediction.
Secondary: phase-1 merge relabel 0.076-0.136 s (prior record 0.22-0.3
at this scale); tip_encode 0.130-0.178 s is the new serial-mode
encoding cost, bought back many times over by the slicing it enables.
t_uf2 is 0.33-0.35 s (processor 0's serial union-find + exact-key
dedup at this edge count) — watch it as datasets grow; the exact
TipPairKey set is costlier per insert than the old lossy packing.
