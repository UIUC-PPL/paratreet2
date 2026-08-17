# uploads/ manifest — 2026-08-17

**This file is the entry point. Read it first, then `relay10.txt`.**

Everything here is a COPY (`cp`, never `mv`); delete what you have pulled.
**25 MB, 21 files.** Trace tarballs pruned to one (see Traces below), and the
batch you already pulled on 2026-08-16 (everything up to `relay9.txt`) has been
moved out to `~/software/uploaded-2026-08-16-0900/` — so **everything in this
folder is new to you.**

---

## If you are the LAPTOP session, do this

1. Read `relay10.txt` — the whole Frontier session, items 0–54, in order.
   Items 46–54 are the current state; everything before is how it got there.
2. Read `pe-sets-scaling.md` for the final numbers, and
   `pe-sets-dist-results.md` for why the configuration is what it is.
3. **The deliverable is `0012-pe-set-split-narrow-veto-and-topN.patch`, and
   it is the ONLY one to apply.** These patches are CUMULATIVE full diffs
   against `4f49227`, not incremental — 0012 already contains 0010's ranked
   shedding and 0011's split. Verified: `git diff` on the Frontier tree is
   byte-identical to 0012. Applying 0010 or 0011 first will conflict.
   0010 and 0011 are kept only so the intermediate states are inspectable.
   Frontier never pushes; vet and land from the laptop.
   Tree state there: `4f49227` of `phaseab-campaign`, six files modified
   (`fof/FoFPhase1.h`, `fof/FoFPhase3.h`, `src/{Driver,Traverser,TreePiece,common}.h`),
   uncommitted, linked against production charm.
4. **Two design-doc updates the code now requires**, both already made in the
   Frontier working tree and therefore inside the patches, but worth carrying
   into `design/` prose as well:
   - `design/piece-load-model.md` — targeted shedding is SUPERSEDED as a
     recommendation. It works (−49 ms) but the PE-set split does the same job
     an order of magnitude better with no migration. Keep the cost model and
     the ranker; they are what proved the split's phase-3 budget.
   - `fof/FoFPhase3.h` — the 2026-07-18 correctness comment said different-tip
     pairs within b are necessarily cross-process. **That invariant is now
     deliberately false.** The patch rewrites the comment; do not let a future
     optimisation reintroduce an ownership test on the strength of the old
     wording. This is what cost a failed gate here (10k 3642 vs 3549).
5. The campaign narrative in `design/phaseab-balancing.md` needs a new section:
   phase 1 is no longer the bottleneck at scale (7% of Iteration 0 at 128
   nodes), and the next one is Pre-traversal + Tree traversal.

---


## START HERE — the current state of the work

| file | what |
|---|---|
| `relay10.txt` | **the whole session in one file**, items 0–54. Read this first. |
| `pe-sets-scaling.md` | **the final result**: scaling to 64/128 nodes. Best measured = 64 nodes, 2771 ms vs 4384 ms serial baseline. |
| `pe-sets-ssweep.md` | the `s` sweep: MODE matters, not s. |
| `pe-sets-dist-results.md` | **the current recommendation and why**: `-u dist` + `FOF_PE_SETS=14` machine-wide + `FOF_S3=0`. Iteration 0 −17.0%, phase 1 −38%, phaseB eliminated. |
| `pe-set-split-results.md` | how the split got there: Parts 1 (victim-only, the failed gate, the ownership prune) and 2 (narrow veto, top-N). |
| `pe-set-split-analysis.md` | the up-front analysis of the idea, including the part I got wrong (see below). |
| `open-issues-explained.md` | the three open issues in plain terms. Issue 1 is now CLOSED by the dist result; issue 3 is what remains. |
| `phase1-idle-structure.md` | where the phase-1 idle actually is, and why every earlier attempt was small. The counters behind your time profile. |
| `targeted-shedding-2b.md` | the shedding line, with addenda A/B/C. Superseded as a recommendation but it is where the cost model and the ranker came from. |

**One correction carried in the reports, so it is not buried:** my up-front
analysis of the PE-set split said phase 3 would pick up the omitted pairs, and
it was wrong about *why* it would. I checked the visitor's edge predicate, the
SEEN table and the same_frag prune, found all three safe, and stopped one layer
too early — `SkipLocalSource` in `Traverser.h` discards local-source pairs
before `open()` is ever called. The 10k gate caught it (3642 vs 3549). Fixed in
patches/0011, narrowed in patches/0012.

---

## Patches — all applied to the tree, all UNCOMMITTED, nothing pushed

| file | what |
|---|---|
| `0012-pe-set-split-narrow-veto-and-topN.patch` | **APPLY THIS ONE, ALONE.** Cumulative diff against `4f49227`: ranked shedding + PE-set split + narrow ownership-prune veto + `FOF_PE_SETS_NODES`. Byte-identical to the Frontier working tree. |
| `0011-pe-set-split-phase1-to-phase3.patch` | intermediate state (split + COARSE veto). Inspection only — contained in 0012. |
| `0010-shed-ranked-selection-and-payload-collapse.patch` | intermediate state (ranked shedding only). Inspection only — contained in 0012. |
| `0005`, `0006`, `0007` | earlier campaign patches, kept so the set is complete. Not needed to land 0012. |

Tree is at `4f49227` with 0010+0011+0012 applied and uncommitted, linked
against production charm. `BUILDS.md` here is current and now documents the PE-set binaries, the four
env knobs, the build-verification check, and why `-u serial` is unusable with
the split at scale.

---

## Traces — PRUNED to one, to keep the upload small

| file | size | what |
|---|---|---|
| `pesets-sumd-rec-frontier.tar.gz` | 24 MB | **the recommended configuration**, `+sumDetail`, full machine, 16 nodes: `-u dist`, `FOF_PE_SETS=14`, `FOF_S3=0`. phase1 2.012 s, Iteration 0 5373.7 ms (matches the untraced 5379.4). 3585 files, `.sts` inside. |

**Everything else was removed from this folder, not deleted.** All 12 tarballs
live in `/lustre/orion/csc710/proj-shared/lvkale-traces/` (verified
byte-for-byte before pruning) and any can be re-staged on request:

- `pesets-sumd-base` (25 MB) — the serial/no-split baseline. **This is the one
  to ask for first**: a sumd of the best config is much easier to read next to
  the run it improves on.
- `pesets-sumd-rec-serial` (24 MB) — same split, `-u serial`. Phase 1 identical
  to 2 ms, uf2 2.094 vs 0.692: the whole serial-vs-dist argument in one file.
- `pesets-proj-{rec,base}` (187 / 175 MB) — projections, 182-PE subset.
- `shedrank-sumd-{r2k12,base,r2k2}`, `shedrank-proj-{r2k12,base}` — the
  shedding line.
- `proto3-{proj-x4-r1,sumd-x4}` — the cross-node S3 line.

Not captured at all: **64 nodes**, where the best absolute result lives
(2771 ms) and where dist's advantage is largest. One job if wanted.

---

## Supporting data

| file | what |
|---|---|
| `load_model-shed-base-r1.txt`, `load_model-shed-shed30-r1.txt` | all 128 per-process `load_model:` lines, baseline and shed30 |
| `shed30-r2.log` | full log of the best arm of job 5286357 (phaseB max 0.788 s) |
| `BUILDS.md` | build state: every staged binary with md5, the PE-set env knobs, the build-verification check, and why `-u serial` is unusable with the split at scale |

Your `timeProfile-2b-proto3-sumd-08-16.png` moved to `~/software/images/` —
that folder is now the home for images dropped onto Frontier, rather than
`uploads/`, which is strictly the outbound staging area.

Already pulled on 2026-08-16 and moved to `~/software/uploaded-2026-08-16-0900/`:
`relay6-9.txt`, `s3-cross-node.md`, `s3-cross-node-protocol.md`,
`s3-xnode-hang-diagnosis.md`, `README-proto3-cross-node.md`, `s3trace.py`,
patches `0008` and `0009`. All but the README also still live in
`reports/`, `scripts/` or `patches/`; the README is also in
`proj-shared/lvkale-traces/`.

---

## LANDED SINCE — both jobs finished, 48 arms, all exact

| file | what |
|---|---|
| `pe-sets-ssweep.md` | the machine-wide `s` sweep (job 5288941). s=3..14 round-robin are all within 68 ms; **MODE, not s, is the variable that matters** — round-robin beats blocked by up to 2198 ms. And the two knobs are coupled: `FOF_S3=0` is only safe once phaseB is nearly gone. |
| `pe-sets-scaling.md` | **scaling to 64 and 128 nodes** (job 5288946). The benefit GROWS with scale: −16% at 16 nodes, **−36.8% at 64**. All three predictions confirmed. **Phase 1 is now 7% of Iteration 0 at 128 nodes — no longer the bottleneck.** 128 nodes is worse than 64, for reasons unrelated to the split. |

Best measured result: **64 nodes, `-u dist FOF_PE_SETS=14 FOF_S3=0`, 2771 ms**,
against 4384 ms serial at the same node count and 6409 ms at the 16-node
baseline this campaign started from.
