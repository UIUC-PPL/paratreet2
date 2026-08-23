# The UF_2 under-merge at 24B/58B particles (2026-08-23) — RESOLVED

## Symptom

`FOF3STAT components` drifted UPWARD with process count on both large
NChilada snapshots, while `max_size` fell — i.e. an under-merge, growing
with the number of processes. A correct FoF answer is a property of the
data, so this made the counts unpublishable as physics even though the
timing and memory numbers were sound.

    cosmo25    16n 6730993881 | 32n 6731064598 | 64n 6731158296 | 128n 6731273452
    romulus25              -- | 32n 29194400113 | 64n 29194556684 | 128n 29194744777

**The control that made this tractable**: the 1.98B tipsy set was
bit-identical at 8/16/32/64/128 nodes (424897832 / max_size 185317566,
design/fof3-2b-scaling.md). 1.98B is 92.3% of 2^31 — BELOW it. So the
defect had to be something that only fires above 2^31, and process count
only modulated how often it fired.

## Cause

`unionfind/unionFindLib.C`, `local_union`:

    auto arrIdx = [this](uint64_t vid) -> int { return getLocationFromID(vid).second; };

`getLocationFromID(...).second` is a `uint64_t` local index — in lazy mode
a RAW PARTICLE ORDER (`fof/FoFPhase1.h`, kUF2IdxBits = 43). The lambda
returned `int`. Above 2^31 particles every `vertexAt(arrIdx(...))` in the
function addressed the wrong vertex, including the merge itself:

    vertexAt(arrIdx(tip2))->parent = (int64_t)tip1;

`vertexAt` EMPLACES on a miss, so this wrote a parent pointer into a
freshly created bogus vertex and the two real components stayed apart.
A silently lost merge: no abort, no `-1` label, just one extra component.
That is why it outlived the three defects fixed earlier the same day —
those tripped `applyUF2Labels`' CkEnforce; this one is invisible.

### How the path is reached

Phase-3 edges are cross-process by construction, so the initial
`union_requests` submission never takes the `local_union` fast path
(which requires BOTH endpoints on this chare). It is reached from
`find_boss2`'s min-heap flip — `union_request(boss1ID, src->vertexID)` —
which re-enters `union_request` locally; as merging proceeds roots
concentrate onto shared chares. The census bears this out: at cosmo25/32n,
`fb2_flip` 708820 -> `local_union` 399611.

## The evidence (why this is the whole story, not a contributing factor)

`[UFSTAT] local_union` counts the calls, so the hypothesis is directly
falsifiable: each truncating call should cost exactly one component.
Post-fix cosmo25 settles at 6730729617 at every node count, so the merges
recovered at each scale are measurable against that run's old census:

    nodes   old comps   recovered   old local_union   ratio
       16  6730993881      264264            312154   0.847
       32  6731064598      334981            399611   0.838
       64  6731158296      428679            517575   0.828
      128  6731273452      543835            657461   0.827

Four scales agreeing on "fraction of local_unions that truncated", against
a uniform-order prediction of 1 - 2^31/N = 0.912 — below it because tips
are MIN-order representatives and therefore skew low.

romulus25 settles at 29193922694 and reproduces the model independently at
its own, higher N (prediction 1 - 2^31/N = 0.963):

    nodes    old comps   recovered   old local_union   ratio
       32  29194400113      477419            506760   0.942
       64  29194556684      633990            677971   0.935
      128  29194744777      822083            879734   0.935

Seven measurements across two datasets, each the ratio of an independently
counted quantity (components recovered) to an independently counted
instrument (`local_union`), all inside 0.827-0.942 and ordered correctly by
N. Nothing else on the merge path has that signature.

## Also fixed (same call, same file)

Three `std::pair<int,int>` receivers of `getLocationFromID`, which returns
`std::pair<int, uint64_t>`:

  - `find_boss2`'s `add_size` target
  - `add_size`'s own parent forwarding
  - the `ANCHOR_ALGO` size path (not compiled by default)

`size` is DEAD in FoF3 (relay79 comment in unionFindLib.C), so these never
lost a merge — but `add_size` reaches `vertexAt`, so each truncation
MANUFACTURED a bogus lazy vertex at a wrapped key, and a bogus vertex has
`parent == -1`, i.e. it counts as a boss. They were inflating
"UF_2 cross-process (touched) components" outright: at cosmo25/128n that
counter fell 7507169 -> 3602873 once they were fixed.

`check_same_chares` had the same pattern but reads only `.first`; widened
for hygiene, no behaviour change.

## Validation

  - cosmo25 16/32/64/128 -> **6730729617 / max_size 2214117459 at every
    node count, log2 histogram bit-identical** (jobs 5333161-64).
  - romulus25 32/64/128 -> **29193922694 / max_size 125856955 at every node
    count, log2 histogram bit-identical** (jobs 5333172-74). All three
    ended before the binary was next relinked (17:53:14 vs 17:53:47), so
    the sweep is one binary throughout.
  - 2B tipsy regression gate -> **424897832 / max_size 185317566**, whole
    FOF3STAT line byte-identical to job 5310026 (job 5333167). The fix
    does not touch the sub-2^31 answer, as it must not.
  - `local_union` still fires at the same rate post-fix (312554 / 398618 /
    518372 / 660347) — the path was not disabled, it was corrected.

The 2B set is now wired into the sweep harness as `DSET=cosmo2b`, a
permanent regression gate: it sits below 2^31, so no width defect can fire
there, and any future change to THAT number means the merge logic itself
broke.

## Lesson (extends the one in width-audit-2026-08-23.md)

The audit checked `.ci`/`.h` agreement and consumption sites in `src/` and
`fof/` — and was right that the marshalling class was clean. It did not
cover the sibling library, where all four defects actually lived. When the
symptom is scale-triggered, audit every repo on the data path, not just
the application's.

Second, and more transferable: **prefer a counter over a bisection.** The
first three defects each cost a full-scale run to localise. This one was
found statically and then CONFIRMED arithmetically against an existing
instrumentation counter (`local_union`) before a single validation job was
submitted — and the same counter turned "the drift is gone" into "the
drift is gone for the predicted reason, at the predicted magnitude, at
four independent scales".


## Follow-on: the process side of the tip encoding is now bounded

`applyTipEncoding` already enforced the INDEX side of the encoding
(`tip <= kUF2IdxMask`); the PROCESS side had no runtime bound
(width-audit item 8). Above 2^kUF2ProcBits = 1,048,576 processes the node
bits overflow into the sign bit of the `long group_number` and silently
INVERT the touched/untouched sign contract `applyUF2Labels` depends on —
a wrong answer, not a crash. Production runs 1024 processes, so this is
1024x of headroom and was never reachable. It is guarded now
(`CkEnforce(CkNumNodes() < (1 << kUF2ProcBits))` in `initUF2`) for one
reason: every defect in this series was silent, and a silent encoding
overflow is the one that would have been hardest to attribute.
