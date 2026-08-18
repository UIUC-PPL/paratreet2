# The phase-1 engine contract (2026-08-18)

What the rest of the pipeline assumes the phase-1 engine has done, written
so an alternative engine — concretely Ritvik's GPU phaseA/phaseB
(`gpu-phase1`) — can replace the CPU engine's internals and be validated
against the same post-conditions, without touching phase 3, uf2, or the
stats. Everything here is the state at merge `75ae684` (post-campaign,
post-cleanup); sources are the comment blocks in `fof/FoFPhase1.h` (head),
`fof/FoFPhase3.h` (head), and `src/common.h` (PE-set block).

## The boundary

`paratreet::runFoFPhase1(treepieces, fof, fof_node, b, pbc, stages,
grid_threshold)`, called from a `[threaded]` context. The driving sequence
is reset → register → phaseA → phaseB → merge → relabel, each sub-phase
separated by a barrier; the call returns only when relabel is complete on
every PE. An engine may reorganize everything INSIDE that call (what the
GPU port does); the contract is about the state at return.

Lifetime: the Particle blocks handed over by `fof::TreePieceRegisterFn`
are stable from end of tree build to the next rebuild/reset; the whole
engine must run inside that window. The safe-migration window for
TreePieces is BEFORE tree build, so no element may move while the engine
runs.

## Post-conditions at return

1. **Labeling.** Every registered particle's `group_number` holds a TIP: a
   global particle order in `[0, n_total)` (checked by
   `runFoFVerifyTips`). Two registered particles on the same process hold
   the same tip iff they are in the same component of the graph the engine
   actually scheduled (see 2). The representative must itself be a tip of
   that component; the CPU engine canonically uses the MINIMUM global
   order (union-by-min), and an alternative engine should too — not for
   correctness of the final counts, but so per-fragment output and
   cross-engine A/B diffs compare bitwise.

2. **Coverage.** Within each process, the engine must have merged every
   particle pair within linking length b EXCEPT pairs it deliberately
   defers under the PE-set split — and a deferred pair is legal only if
   its two pieces sit in DIFFERENT sets in `pieceSetTable()`. That table
   is the single source of truth the phase-3 prune veto
   (`paratreet::peSetKeepLocalPair`) reads: any pair the engine skipped
   but the table calls same-set is silently lost (a wrong count, not a
   crash). Note the table is keyed by the CLAIMED/OWNING assignment of a
   piece, not by where a particle happens to reside (the FOF_STEALA
   lesson: claimed-by ≠ resident-on).

3. **`pieceSetTable()` consistency.** Written once per iteration, before
   phase 3 starts, one instance per process (all PEs read the same
   vector); entry = set id for pieces of this process, -1 for pieces it
   knows nothing about. An engine that does NOT split (covers all
   intra-process pairs) must leave the table in the sets=1 state — then
   the veto never fires and phase 3 keeps its full ownership prune. **A
   GPU process is exactly this case: sets=1 on that process.** The split
   is per-process, so mixed jobs (CPU processes at FOF_PE_SETS=14, GPU
   processes at 1) are well-formed.

4. **Relabel-before-annotation ordering.** After the engine returns, the
   app runs `upwardPass` (annotates `min_frag`/`max_frag`/`n_below` over
   the relabeled `group_number`s) and only then `loadCache`. The engine's
   relabel must be COMPLETE at return because cache-shipped copies are
   taken at ship time: any particle relabeled after its copy shipped is a
   stale copy in some other process's walk. The annotation-validity
   CkEnforce (`min_frag >= 0`) trips on the ordering violation. Under
   `-u dist` (the standing default) the app additionally rewrites tips to
   the owner-encoded form between the engine and upwardPass — same
   ordering hazard, same rule: engine first, completely.

5. **Quiescence hygiene.** Phase 3's completion detection is CkWaitQD.
   The engine must leave NO Charm-level messages in flight at return, and
   any background traffic it keeps running (keep-alive-style) must be raw
   Converse, which QD does not count. A Charm-level heartbeat would hang
   phase 3's and uf2's QD brackets.

## What the engine need NOT do

- **Cross-process pairs.** Phase 3's tree walk covers them; its predicate
  is "different tips within b" with NO ownership test. Since the PE-set
  split, same-process different-tip pairs are real and load-bearing (the
  walk merges what the split deferred) — do not reintroduce an ownership
  prune in phase 3 on the strength of the pre-2026-08-16 invariant (full
  warning at the head of FoFPhase3.h).
- **Global labels.** Phase 3's gather/relabel produces them.
- **Load balance.** Engine-internal. The campaign's survivors (S1 claim
  pool, S2 partitioning, PE-set split) are the CPU engine's own business;
  a GPU engine balances its own way. The retired mechanisms are at tag
  `campaign-2026-08-stealing`, not on main.

## Validation recipe for a new engine

1. Laptop matrix: `make test` in examples/fof3 (16 runs, serial O(n^2)
   end-to-end comparison), then 1m `-c full` (333889) and split-active
   10k `-u dist FOF_PE_SETS=2` (3549) — the mixed case in miniature.
2. `runFoFVerifyTips` after the engine, `runFoFFragmentHistogram` for the
   per-fragment comparison against the CPU engine on one input.
3. At scale: component count exact (2B gold 424,897,832 at the standing
   config); both runtimes (reconverse is the deployment runtime).
4. Compare walk-work counters (edges emitted, leaf visits, prunes) only
   WITHIN a job: they are claim-race- and placement-dependent and move
   0.1-0.3% between jobs at identical code (relay15, job 5304461).
