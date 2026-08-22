# Staged gather (`-u gather`): contract-then-gather UF_2 — experiment spec

2026-08-22. Status: EXPERIMENT, built to understand the variant (Kale). Not a
commitment: the serial finisher anti-scales with process count by construction
(the phaseB lesson in degenerate form — re-concentrating scale-growing work),
and it forfeits streaming's measured −104 ms of overlap (relay76). Measured
against `-u dist` and `-u serial` in one job before any further discussion.

## Goal and invariant

Reproduce `-u serial`'s labels **bitwise** (same FOF3STAT components line)
while gathering only fully-contracted cross-process edges to PE 0 instead of
every raw edge. `-u serial` ships all ~1.24M edges at 2B; the split
manufactures ~790k same-process edges that `-u dist` absorbs for free
(relay78 §11). This variant retires them per process first.

Bitwise equivalence argument: the serial finisher unions by **min encoded
tip** (FoFPhase3.h:625-633). The per-process local UF also unions by min tip
(the mergeBody precedent, FoFPhase1.h:1178-1196). Min of a component = min
over its local-subcomponent minima, so contraction composed with the global
min-union yields the same root for every tip as the flat union over raw
edges. Local-only components (no cross edge) get their local root as boss —
identical to what the flat serial UF computes for them.

## Mode plumbing

`-u gather` → `UF2Mode::Gather` (Main.h:50 enum, Main.C:80-84 parse,
FoF3.C:529-537 branch) → `runFoFPhase3Staged(...)`, a sibling of
`runFoFPhase3` in FoFPhase3.h sharing its walk section verbatim (factor or
duplicate; duplication acceptable for the experiment, mark it). Echo the mode
in the config line (Main.C:207 area). Same serial+peSets warning suppressed
for gather (absorbing same-process edges is the point).

## Phases (driver thread, after the walk completes exactly as in serial)

Walk + emission unchanged: per-PE `edge_buf3` with per-PE `seen3` dedup and
the process-level SEEN gate (already process-unique per (g,f)).

1. **Stage to process.** Broadcast `fof.stagePhase3Edges(cb)`: each PE
   partitions its `edge_buf3` by `tip >> kUF2IdxBits` (both endpoints mine →
   LOCAL; else CROSS; every edge has ≥1 local endpoint: g is the local
   fragment) and appends into two vectors on `FoFPhase1Node` via
   `ckLocalBranch()` under a mutex (pattern: trySeenInsert,
   FoFPhase1.h:573-581). Barrier via contribute(cb).

2. **Local contraction + remote routing.** Broadcast
   `fof_node.contractAndRoute()` (nodegroup): build the per-process UF over
   LOCAL edges (unordered_map<long,long> parent, findRoot with path
   compression, union by min tip — copy mergeBody). Then for each CROSS edge,
   replace every locally-owned endpoint with findRoot(endpoint), and send the
   edge (batched per destination process) to the remote endpoint's owner via
   nodegroup entry `contractRemote(std::vector<std::pair<long,long>>)`.
   Receiver contracts the endpoints IT owns through its own local UF,
   normalizes lo/hi, dedups into a `to_root` buffer (unordered_set on the
   branch, mutex if entries can race — nodegroup entries are not serialized;
   guard with the same mutex discipline as SEEN). Driver: `CkWaitQD()` (plain
   sends only).

3. **Gather.** Broadcast `fof_node.forwardContracted(cb)`: every branch
   contributes its deduped `to_root` buffer bytes in a
   `CkReduction::concat` to a driver callback (nodegroup reduction —
   empty contributions included so the reduction closes).

4. **Serial finish on PE 0.** Identical loop to FoFPhase3.h:606-636 over the
   contracted edges: global dedup set + lazy parent map + min-id union +
   path compression. Record `edges_unique` (contracted count) and a new
   FOF3STAT field for pre-contraction cross count so the compression ratio
   is visible.

5. **Map delivery + per-process expansion.** PE 0 builds slices keyed by
   LOCAL-ROOT tip, owner = root >> kUF2IdxBits (slicer pattern
   FoFPhase3.h:677-695), label = -(globalRoot+2). New nodegroup entry
   `expandAndApplySlice(slice, group_gid, cb)`: the branch builds the full
   per-process tip→label map — for every tip it staged (in the local UF or
   any edge): r = findRoot(tip); label = slice[r] if present, else
   -(r+2) (local-only component, negated to match serial semantics —
   EVERY tip touched by any edge ends negative, including roots; see the
   sign contract FoFPhase3.h:638-657 and the 2026-08-05 phantom incident).
   Install as `global_slice` and trigger the existing
   `applySliceOnPE` (FoFPhase1.h:1112-1120, 3890-3900) → materializeLabels →
   closing reduction. Keep the broadcast fallback out: gather mode always
   slices (per-process maps are inherently sliced).

6. **Timers/stats.** Reuse the serial rows (FoFPhase3.h:702-706): t_gather =
   phases 1-3, t_uf2 = phase 4, t_relabel = phase 5. Fill edges_sent with
   raw emitted count, edges_unique with contracted-unique; print one new
   line `FOF3STAT gather: local L cross C contracted K` from the driver.

## Correctness gates (standing)

- 1m.tipsy, 16 procs, `-c full`: `-u gather` vs `-u serial` vs `-u dist` —
  FOF3STAT components line must be bitwise identical across all three.
- 8m-uniform and 16m at 16 procs, `-u gather -c full`: TEST PASSED.
- 1m at 2 procs (degenerate: fewer processes than sets) and single-process.
- With FOF_UF_SIZES=0 (env is orthogonal; gather does not touch the library).

## Non-goals

No streaming overlap, no scaling claim, no default change. `-u dist` remains
the shipping mode. The DEBUG same-process-edge tripwire in addPhase3Edge
(FoFPhase1.h:3354-3370) has a stale comment and would fire under the split in
DEBUG builds — out of scope here, noted for cleanup.
