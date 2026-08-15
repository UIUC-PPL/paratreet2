# Frontier session brief — READ THIS FIRST

Durable orientation for any Claude Code session on the Frontier login
node working the paratreet2 FoF phaseA/B campaign. Sessions here get
killed by connection resets fairly often, so this file exists so a fresh
one can be productive in minutes. Kale points you at it; everything else
you need is in this repo or on disk.

## 1. What the work is

paratreet2 is a Charm++ tree/traversal framework; the FoF app
(`examples/fof3`) is a friends-of-friends cluster finder over cosmology
datasets. The campaign is about PHASE 1's load imbalance at 2 billion
particles on 16 nodes: one process takes ~10x the median, and that
straggler sets the phase.

Structure (invariant, from FoFPhase3.h): **phase 1 is the complete FoF
restricted to a process.**
- phaseA = piece pairs on the SAME PE (self pairs + intra-PE)
- phaseB = piece pairs across PEs WITHIN a process (a claimable pool)
- phase 3 = everything CROSS-process, via the CacheManager walk
So a piece's home process decides which of its pairs are cheap (phase 1,
local) and which are expensive (phase 3, remote). Locality IS the
algorithm — remember this before proposing to move anything.

## 2. Working agreement

- **You never push.** Code and design land from Kale's laptop. You pull.
- **Reports out**: write `~/software/reports/<task>.md` plus a
  `relay<N>.txt` summary (continue the numbering — relay4 was
  2026-08-15). Kale scps them to the laptop. Plain markdown, not RTF.
- **Every report carries provenance**: commit hash, job ids, the exact
  srun line, per-arm env.
- **Write inconsistencies down.** relay4 was the most valuable report of
  the campaign because it was structured as "what the spec expected vs
  what I found", and it caught three genuine errors in the spec. Do that.
- A negative result reported plainly is worth as much as a win. Several
  ideas have been closed cheaply that way, on purpose.

## 3. Orientation on the machine

Read in this order:
1. `~/software/BUILDS.md` — build state, charm trees, staged binaries.
   **Verify tracing with `nm -C <bin> | grep -ci TraceSummary`
   (0 = production, ~465-524 = traced); never trust a filename.**
2. `~/software/notes/frontier-corrections.md` — build recipe quirks
   (e.g. `module` is unavailable in non-interactive shells).
3. This repo: `design/phaseab-balancing.md` is the campaign narrative,
   now 35 sections; §§30-35 are the current state. Read those.
4. `design/frontier-relay<N>-*.txt` are your predecessors' own reports.

Inputs (moved 2026-08-13): `/lustre/orion/csc710/proj-shared/` —
`cosmo25cmb.768g2_dm.001024` (2B) and `lambb.00500` (80M). The old
`scratch/rrao/` copies are permanently unreadable.

There is **no build-stack.sh on Frontier** — that is an Anvil script.
Use `~/software/scripts/build-*.sh`.

## 4. Standing gates

- 2B exactness: `FOF3STAT components: 424897832`, EVERY arm. A mismatch
  is a stop-and-report event.
- 10k smoke: 3549 components.
- `FOF_S3_LOOPBACK=1` replays every locally claimed unit through
  flatten -> pup -> rebuild -> walk against the direct walk. It is the
  strongest available check on the S3 WIRE FORMAT — and it says nothing
  about transport or scheduling, so do not cite it for those.
- Say what each gate scale actually EXERCISES. A 10k run ships nothing
  under S3 (the pool drains before a helper is matched), so a 10k pass
  cannot validate stealing. This has bitten the campaign twice.

## 5. Traps that have already cost time

- **`-march=` changes FoF results.** gcc defaults to
  `-ffp-contract=fast`; SSE2 has no FMA so the base binary cannot fuse,
  and enabling FMA changes the rounding of the linking-length test —
  424897833, one component over, reproducibly. Any `-march` work must
  carry **`-ffp-contract=off`** (which keeps 5479 of 5540 vector refs).
- **charmc mis-parses `-march=X -Ofast`** in that order (tokens reach
  cc1plus joined into the -march= value; the error blames -march=).
  Write `-Ofast -march=X`.
- **A nodegroup branch entry method runs on an arbitrary PE**, so
  `if (CkMyPe()==0) CkPrintf(...)` inside one prints on nobody.
  `CkMyNode()==0` is the correct once-per-job guard.
- `+lci_ndevices` must track `+ppn` (min(8, ppn/2)); 7 devices on ppn 7
  hangs at cache-manager init.
- Single-node runs need `--network=single_node_vni`, not `job_vni`.
- Do NOT `du` a traceroot right after srun returns — Lustre writes are
  still landing (59 MB once measured as 531 KB).
- Zero-copy (`nocopypost`) is unusable in any multi-device LCI config:
  `CMK_NOCOPY_DIRECT_BYTES` is a compile-time 32 while `getRMR` scales
  per device. Filed upstream as charmplusplus/reconverse#203.

## 6. Tools

- `.claude/skills/sumdetail-analysis/` in this repo: `SKILL.md`,
  `sumd_tool.py` (+sumDetail traces) and `projlog_tool.py` (full
  projections `.log.gz`). Batch every `--ep` regex into ONE invocation —
  each run costs 12-18 s on Lustre.
- `design/s3-entry-roster.md` — what every S3 entry method does and what
  it costs. Read it before reading any trace.
- Trace a protocol's LIFETIME CHAIN early and small; `+traceprocessors`
  restricts recording to a PE subset and was measured safe at 2B.

## 7. Where the campaign stands (2026-08-15)

LANDED: S3 stealing (coordinator-mediated phaseB work stealing) with a
POD wire format + bulk pup, a parallel helper-side rebuild, and grant
sizing fixed. Straggler phaseB_s max went 3.2 s -> ~1.23-1.32 s against
a 0.25 s granularity floor.

CLOSED, on measurement, cheaply:
- per-piece tree ARENA / contiguous layout — the pool-chunk knob was a
  clean null, so the arena cannot help (§35).
- hand-written SIMD — vectorisation leaves phaseB unmoved (§35).
- donor-side RESERVATION — inverted grant composition (§30).
- global load balancing — GreedyRefineLB ignores locality and cost
  5-7x on Iteration 0 (piece-load-model.md).

THE STANDING FINDING (§34): what pays is removing SERIALIZATION — work
stuck on one PE while thirteen idle — not reducing per-grant cost. Three
independent measurements agree.

OPEN: targeted shedding — migrate a few pieces off the single
worst-ranked process. The load model reproduces across machines
(r=+0.87), the outlier signature reproduces (robust z~10, 1.57-1.59x gap
to #2), and the measured ceiling is -36% of the phaseB max.
See `design/piece-load-model.md`.
