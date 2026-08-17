# Codebase cleanup after the campaign (2026-08-17)

## How much the codebase grew

Reference point `4d23ce1` (2026-07-25, "fix warnings") to HEAD, counting
only `src/`, `fof/`, `examples/`:

    78 files changed, 10,060 insertions(+), 3,220 deletions(-)

i.e. **net +6,840 lines**. Where it went:

| area | 2026-07-25 | now | note |
|---|---|---|---|
| `src/` | 12,481 | 11,636 | **SHRANK** by 845 |
| `fof/` | 0 | 6,504 | the FoF module was extracted after this date |
| `examples/fof3` | 1,003 | 1,211 | +208 |

So essentially all of the growth is the `fof/` module itself, which did
not exist at the reference point — not bloat added to the framework. The
framework proper is smaller than it was.

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
