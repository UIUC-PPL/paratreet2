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
