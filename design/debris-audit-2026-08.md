# Post-campaign debris audit — unionfind + paratreet2 fof (2026-08-24)

Audit of dead code and bloat left by the August 2026 FoF optimization
campaign (see `campaign-report-2026-08.md`). Every claim was verified by
grepping call sites, not declarations. "Dead" = zero invocation site outside
the definition itself (generated `.decl.h`/`.def.h` excluded).

**Dispositions (Kale, 2026-08-24).** Clean up only what the campaign added;
older machinery stays — it may matter for the broader (explicit-graph,
contiguous-array) union-find study. The compression wave is retained
compile-gated (`CONCURRENT_COMPRESSION_WAVE`) rather than deleted: the FoF
refutation is workload-bound (shallow forests) and deep-chain explicit-graph
inputs may invert it. Applied in the `post-campaign hygiene` commits; the
pre-cleanup state is tagged `pre-cleanup-2026-08` in both repos.

| item | provenance | action |
|---|---|---|
| compression wave (~230 lines; 40 B/vertex unconditional) | campaign | **compile-gated** `CONCURRENT_COMPRESSION_WAVE`, zero cost when off |
| UFSTAT branch census (`ufs_`, ~70 lines, ungated hot-path increments) | campaign (db73766) | **deleted** (results archived in relay78/82) |
| climb-hop histogram (`ufh_`, ungated `clzl` per climb) | campaign (cd2d9c8) | **deleted** (relay83 archives the data) |
| `FOF_UF_SHORTCIRCUIT` mechanism | **initial commit** (campaign only added the env gate) | **kept** — old code, gated, default off; chain-shortening may matter for deep-chain studies |
| `FOF_UF_SIZES`, `FOF_UF_LOCALCOMP` | campaign | kept (shipped / cheap gated arm) |
| `-u gather` (444 lines) + `-u serial` (257) | campaign | kept (validated instrument + bitwise oracle) |
| dead dense tip-enumeration path: `computeTipEncoding`, `encode_map`, `uf2_vertices`, `.ci` entry (D1 below) | paratreet2, orphaned by sparse-uf2 2026-07-25 | **deleted** |
| `verifyTips` + `runFoFVerifyTips` (superseded by `verifyEncodedTips`) | paratreet2 | **deleted** |
| `phaseBWalker` (superseded work-division helper) | paratreet2 | **deleted** |
| `runFoFFragmentHistogram` | paratreet2 | **KEPT — the audit's "zero call sites" was scoped away from fof1; `examples/fof1/FoF1.C:145` calls it** |
| `-S` switch (compat no-op) | paratreet2 | kept — deliberate compatibility with old sbatch scripts |
| 2^20-process encoding guard | new | added as debug-only `CkAssert` (fof3 `Main.C`; compiled out under `--with-production`) |
| everything below (pre-campaign) | old | **kept untouched** per scoping |

Net: `unionFindVertex` is 120 B → 80 B in default builds (wave fields gated
out, including a per-vertex `std::vector` ctor/dtor); the remaining dead
fields (`process_tip`, `findOrAnchorCount`) are pre-campaign and were left.

---

## Pre-campaign machinery left in place (reference for a future pass)

These are dead or inert **today** but predate the campaign and may be
relevant to the broader union-find study. File:line as of tag
`pre-cleanup-2026-08`.

### unionfind

- **`UnionFindLibGroup` dense component-count machinery** — orphaned by
  bbe0856 (sparse labels): `build_component_count_array` (`.C:1379`, a
  reductiontarget with zero contributors), `perform_pruning` (`.C:1335`,
  only caller is the former), `get_component_count` (`.C:1388`; also reads
  `component_count_array`, which the constructor never initializes — latent
  dangling read), `contribute_count`/`done_profiling` (only call site is
  commented out in `examples/prob_mesh/mesh.C:50`),
  `increase_message_count` (incremented, never read; NOTE `PROFILE =
  -DPROFILING` is ON by default in Makefile.common and the six
  `CProxy_UnionFindLibGroup` constructions sit OUTSIDE the `#ifdef` — a
  proxy is built on every union_request even with profiling off).
- **`componentCountMap` + `merge_count_maps` custom reducer** (`.C:19-52`) —
  only contribute site is inside `#if 0`; still registered at initnode.
- **`prefixLib` dense-renumbering subsystem** — completely inert but wired:
  `startPrefixCalculation` has zero call sites, `prefixLibArray` is assigned
  and never dereferenced, yet every run allocates n Prefix chare elements
  that receive no messages (~104 lines + a build stage). README already
  records the reconstruction recipe.
- `compress_path` (self-documented unused), `get_parent`, `flush_buffers`
  (clients use `quiesce`), `prune_components`/`report_surviving_components`
  (documented public API, no in-tree caller, aborts under `FOF_UF_SIZES=0`;
  possible out-of-tree clients).
- Dead fields: `process_tip` (written only, 8 B/vertex + pup slot),
  `findOrAnchorCount` (read only inside dead profiling code, unconditional
  hot-path increment).
- Dead text: `.C:1588-1702` `#if 0` block (115 lines), `.h:335-388`
  commented functions, scattered `//` fragments.
- Obsolete trees, untouched since 2018: `k-way-merge/` (152), `sequential/`
  (343), `scripts/` (347, PBS-era), `uf_changa_integration.patch` (593),
  `examples/random_graph/` (45, no UnionFindLib calls at all).
- `ANCHOR_ALGO` (~145 lines): compile-time-off ALTERNATIVE algorithm, not
  debris; separate decision.
- htram `AGGREGATION` (~152 lines): compile-time-off, refuted for the UF
  tail (relay89/91); carries the documented vertex/class-ABI hazard
  (Makefile.common) — keep-or-kill is a standalone decision.

### paratreet2

- `FoFPhase3.h` gather path carries a verbatim ~33-line duplicate
  (`:815-842` duplicating `:484-516`, self-documented) — factor, don't
  delete.

## Verified-live (do not touch)

Lazy vertex storage (`lazy_store`/`vertexAt`/`makeVertexID` — the fof3
path); dense vertex-array mode (`initialize_vertices`/`return_vertices` +
`unionFindInit` — standalone examples `prob_mesh`, `simple_graph`);
`runFoFFragmentHistogram` (fof1) and `runFoFFragmentHistogramNode` (fof3
`-g`); `countFragments` (feeder for the `-g` histogram).

## Validation of the applied cleanup

- unionfind builds clean with and without
  `WAVE=-DCONCURRENT_COMPRESSION_WAVE`; default build restored.
- fof lib + fof3 clean-rebuilt; smoke run `-u dist -c full` on 2m.tipsy:
  **FOF3 TEST PASSED, 666737 components** (grid-reference oracle exact).
