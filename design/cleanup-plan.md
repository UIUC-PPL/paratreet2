# Codebase cleanup after the campaign (2026-08-17)

## How much the codebase grew

Measured from **`f2d40c7` (2026-08-03, "extract FoF into its own fof/
module")** — i.e. AFTER the framework separation, so the numbers are the
campaign's own additions and not the extraction moving code between
directories. (An earlier cut from 2026-07-25 made `src/` look like it
SHRANK by 845 lines; that was the extraction carrying code out to `fof/`,
not restraint. Kale caught it.)

    70 files changed, 7,337 insertions(+), 1,710 deletions(-)

| area | 2026-08-03 | now | added |
|---|---|---|---|
| `src/` | 10,396 | 11,636 | **+1,240** |
| `fof/` | 3,131 | 6,504 | **+3,373** |
| `examples/fof3` | 1,099 | 1,211 | +112 |

And the number that matters most:

    fof/FoFPhase1.h    2,234 -> 5,075 lines    (+2,841, i.e. +127%)

**One file more than doubled in two weeks**, and it is the file every
retired mechanism lives in. That is the cleanup target, not the repo as a
whole.

## What is debris, and how much

`fof/FoFPhase1.h` is 5,075 lines. The retired-mechanism footprint:

| mechanism | status | rough size |
|---|---|---|
| S3 stealing (transport, coordinator, pool, protocol) | RETIRED | `fof/FoFStealTypes.h` 109 lines entire; ~605 lines from the "S3 transport layer" comment to EOF in FoFPhase1.h; 12 entry declarations in `fof/fof.ci`; 326 `s3`-mentioning lines overall |
| Section-27 donor reservation | RETIRED (default off) | ~135 lines of knobs + the window logic |
| Cross-node helpers (`FOF_S3_XNODE`) | RETIRED (default off) | the global-coordinator block |
| Targeted shedding (`FOF_SHED_*`) | SUPERSEDED | 16 lines in `src/TreePiece.h`, 11 in `src/Driver.h`, 2 in `src/paratreet.ci` |
| `PARATREET_PREBUILD_LB` + `UserSetLBLoad` model | SUPERSEDED | the LB block in Driver, the model in TreePiece |
| `PARATREET_POOL_ELEM_SIZE` | measured null | ~15 lines in CacheManager |

Ballpark: **900-1,100 lines of retired mechanism**, almost all of it in
`fof/`, plus the env knobs that reach it.

## What must NOT be removed with it

- **The cost model and the `m2_cross` ranker** (`printLoadModel`,
  `pieceM2`). They are what proved the PE-set split's phase-3 budget and
  they are how any future victim/hot-process question gets answered.
- **The instruments**: `FOF3STAT load_model`, the per-process phaseA/phaseB
  deposits, `s3_bytes`/`s3_time` if they can be decoupled from the S3 code
  they currently live in.
- **`FOF_S3_LOOPBACK`** — it validates the wire format, and the PE-set
  split does not use the wire, so this can go WITH the transport. Check
  whether anything else depends on it first.
- **Everything the PE-set split touches**: the narrow ownership-prune veto
  in `Traverser.h`, the rewritten `FoFPhase3.h` comment, `common.h`'s
  `peSetKeepLocalPair`.

## Proposed method

1. Tag the current tip (e.g. `campaign-2026-08-stealing`) and push the tag,
   so the whole stealing line stays recoverable by name without living in
   the working tree.
2. Remove in one commit per mechanism, gating after each: 10k + 1M exact,
   1M loopback while the wire still exists, both runtimes. Order:
   shedding -> prebuild-LB -> pool knob -> cross-node -> reservation ->
   S3 proper (largest, last).
3. Keep `design/` prose, since the numbered sections are the record of WHY
   each was retired. Delete `design/campaign-archive/` separately when the
   campaign formally closes (its README says so).

## Executed (2026-08-18)

Done as planned: tag `campaign-2026-08-stealing` at `0957364`, then six
commits (`2a84e7b` shedding, `df144b0` prebuild-LB + cost-model load,
`c7113eb` pool knob, `1673f02` cross-node, `465b377` reservation,
`1046c29` S3 proper). Net: **9 files, −1,789 lines / +23**;
`fof/FoFPhase1.h` 5,075 → 3,789; `fof/FoFStealTypes.h` deleted. All
keep-list items verified present after the series (cost model +
`printLoadModel`, `phaseA_stages`/`stage_pe`, `pb_unit_hist`, PE-set
split + narrow veto, S1 claim pool, S2 partitioning/slicing, keep-alive
ring).

Gates: every commit passed the classic 16-run `make test` matrix, 1m
`-c full` (333889), and split-active 10k `-u dist FOF_PE_SETS=2`
(3549); commits 4 and 5 additionally re-gated the still-live wire
(forced FOF_S3_TEST 9-ships run and FOF_S3_LOOPBACK, both exact). The
end state passed reconverse (recharm clone, clean rebuild): 10k single
and lcrun -n 2 (3549), split-active 10k (3549, pairs dropped on both
processes), 1m lcrun `-c full` (333889). Two incidental finds en route:
the second `s3XDrained` call in phaseBChained was never under its `if`
(harmless, XNODE defaulted 0), and the recharm clone's `inputs/` lacks
`1m.tipsy` — reconverse 1M gates need an absolute path to the
clusterFinding copy.

One deviation from the letter of the plan: the both-runtimes gate ran
per-commit on classic only, with one reconverse pass at the series end
(the six commits landed in one sitting; any reconverse-only breakage
would still bisect over six commits).

## Validated at scale (Frontier relay15, job 5304461, 2026-08-18)

2B/16 nodes, 3 reps at `8e08843`, full artifact-scrubbed rebuild: all
reps exact (424,897,832), Iteration 0 within 0.03% of relay13's
pre-cleanup arms, stall detector shows no dark time, and the binary
carries zero strings from any removed subsystem (twelve-string check
now aborts the job script before spending 2B time). One drift, not
wall-clock: walk-work counters +0.1-0.3% with within-process phaseA
skew 1.35 -> 1.25 — the whole vector moves as a block between jobs
while near-constant within one, and skew now reads 1.23/1.35/1.25 on
three jobs across three different trees, so it tracks node placement /
the claim race, not code. STANDING RULE from that: these counters are a
regression signal only WITHIN a job, never across jobs. The cleanup is
closed; the branch is ready to merge to main.
