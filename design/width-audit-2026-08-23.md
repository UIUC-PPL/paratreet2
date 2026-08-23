# 32-bit width audit (2026-08-23)

Prompted by Ritvik hitting a 32-bit overflow on a dataset larger than our 2B
set, and by the earlier one in the sibling library (unionfind `9cf9964`: a
`.ci` entry declared `compNum` as `int` while the `.h` said `long`, so REMOTE
deliveries truncated while local calls kept 64 bits — dormant for years,
fatal once labels exceeded 2^31).

**Our 2B set is at 92.3% of the int32 limit** (1,981,808,640 of 2,147,483,647
— 166M of headroom). That is the context for everything below: N-scale `int`s
are not theoretical here.

## `.ci` vs `.h` parameter widths: ZERO mismatches

Every entry method in `src/paratreet.ci` and all 46 in `fof/fof.ci` was
checked against its C++ declaration; two independent passes agree. The
2026-07-25 audit did this deliberately — `TreePiece(…, long, …)`,
`Writer(…, long)`, `TipsyWriter::write(long, …)`,
`Partition::deleteParticleOfOrder(long)`, `FoFPhase1::verifyTips(long, …)`,
`enableUF2Streaming(…, long batch, …)` are widened on both sides. No
`sum_int` anywhere in `fof/`. **So the marshalling class is clean; look at
consumption sites instead.**

## Confirmed

1. **`src/Decomposition.C:390` — oct decomposition silently collapses at
   N > 2^31.** `long* counts` (from a correct `sum_long` reduction) narrowed
   by `int n_particles = counts[i]`. On the FIRST histogram round
   `counts[0] == N_total`, so above 2^31 it goes negative, `> threshold` is
   false, subdivision never starts, and one splitter covers the universe.
   This is the only path FoF can take (`examples/fof3/Main.C:44-45` hardcodes
   oct; `FoFPhase3.h:472` enforces matching decompositions).
   `BinaryOctDecomposition` inherits it. `SfcDecomposition` is long-clean.
   **The failure message is itself broken**: `:447` formats two `long`s with
   `%d`, so the diagnostic that fires prints garbage. Same at `:238`.
   Siblings in the same file worth fixing together: `:220`
   `int ki_next = ki + threshold` (threshold is `long`), `:213`
   `int decomp_particle_sum`.
   *(Ritvik is fixing this one — do not duplicate.)*
2. **`src/Paratreet.h:264,274` → `Partition.h:508-519`** — global N passed as
   `int` (consistently in `.ci` and `.h`, so not a marshalling bug), then
   `int first_particle = writer_idx * particles_per_writer` holds a global
   order. Wrong at N ≥ 2^31; reachable only for apps that write particles
   out, which FoF does not. `Writer.C` itself is clean and `CkEnforce`s the
   Tipsy 32-bit format limit at `:109`.
3. **`fof/FoFPhase1.h:293`** — `int order` in `FoFParticleRecord` while
   `Particle::order` is `long`. It is the union-by-min-order key and the
   canonicalization key for the `-c full` oracle, so wrapping would make the
   oracle validate against a different answer. Unreachable today
   (`kAutoFullMaxN` caps `-c full` at 20M). There is an `int pad` on the next
   line, so widening to `long` and dropping the pad keeps `sizeof` at 32.
4. **`src/Node.h:149` (+`:64-65`,`:185`)** — `Particle::order` truncated into
   `int particle_order` / `particle_min_order` / `particle_max_order`. DEAD:
   `setParticleVertexID` has no callers, and `Partition.h:101-109` declares
   `propagateVertexIDRanges`, `initializeLibVertices`, `unionRequest` which
   are never defined and absent from the `.ci`. **Recommend deleting the
   vestigial vertex-ID subsystem rather than fixing it** — it reads live.
5. **`src/Decomposition.C:516/528/532, 613/625, 750/767`** — `vector<int>` +
   `sum_int`, and `bins_sizes = vector<int>(1, universe.n_particles)`. First
   split level puts ~N in one bin. Binary/Kd/LongestDim only, already
   self-documented at `:609-610` as unaudited and unused. These are the only
   `sum_int` uses in `src/` and `fof/`.

## Plausible (unbounded from the code, not currently reached)

6. **`contribute()` takes `int dataSize`** (`ckreduction.h:442`); four sites
   pass a narrowing `size_t` product with no `CkEnforce`:
   `FoFPhase1.h:4087` (`recs.size()*32` → caps `-c full` gather ~67M records),
   `:3655` (`edge_buf3.size()*16` → ~134M edges), `:1318`, `:4029`. Fails
   with a wrapped negative size rather than aborting.
7. **`fof/FoFPhase3.h:514`** — `int n_edges = msg->getSize()/sizeof(...)`,
   while the staged path at `:869` already uses `long`. An inconsistency, not
   a choice; the `CkEnforce` at `:603` runs after the `FOF_EDGE_CHECK` loop.
8. **Tip encoding's process field has no runtime bound.** `kUF2ProcBits = 20`
   (`FoFPhase1.h:118-121`); the index side has `CkEnforce(… <= kUF2IdxMask)`
   at `:3361`, the process side has nothing. Above 2^20 processes it
   overflows into the sign bit of the `long` `group_number` and silently
   inverts the touched/untouched SIGN CONTRACT. One
   `CkEnforce(CkNumNodes() < (1 << kUF2ProcBits))` in `initUF2` closes it.
9. **`src/Traverser.h:175`** — `node->requested.fetch_or(1ull << CkMyRank())`
   is UB at ppn ≥ 64. Production runs ppn 7, so unreached, but it sits on the
   cache-request path where a wrong bit means duplicate or lost requests.
10. **`printf` truncations on N-scale longs**: `Decomposition.C:238,:447`
    (the abort path — i.e. exactly when you need to read it),
    `FoF3.C:465-468` (`N` as `%d`, fires when N > 20M), `:728`, `:770`.

## Benign, with the bound recorded

- `Traverser.h:49` `n_particles*n_particles` into a `ull`: leaves ≤ 32
  particles, and fof3 compiles `-DCOUNT_INTERACTIONS=0`. Needs ~46,341/leaf.
- `FoFPhase1.h:4581` `vector<int> uf_parent`: 2^31 particles on one PE ≈
  240 GB. Caveat — `FOF_STEALA` (default on) makes the real bound the PROCESS
  total, not the PE share.
- `src/DeviceTree.h:43-45` + the GPU rebase (`FoFPhase1.h:2277/2271/2360`):
  per-process flat index space, ~240 GB host. Unreachable but, unlike
  `uf_parent`, undocumented.
- Per-Reader/Partition/TreePiece `int` counts: memory-bounded far below 2^31.
- `src/Reader.h:132` `int n_particles = universe.n_particles` — truncating,
  but `Reader::request` has no callers. Dead-code foot-gun.
- `src/NChiladaReader.C` is `int64_t`/`off_t` end to end: the >2^31 INPUT
  path is clean. The Tipsy loader is `int` because the Tipsy header is 32-bit
  by format (documented at `Reader.C:92-93`).

## Lesson to keep

When a symptom is "correct at small scale, wrong at large", check widths
before theorising about races or algorithms — twice now the cause has been a
width, and once (unionfind) a plausible ordering-race hypothesis was tested
and disproved first. Check BOTH `.ci`/`.h` agreement AND the consumption
sites: the reduction here was correctly `sum_long`; the driver narrowed it.
