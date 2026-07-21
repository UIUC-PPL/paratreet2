# Step 5: min-component-size reporting filter (`-m`)

Status: implemented and validated, 2026-07-20. Closes the actionable
follow-up from `design/investigation-80M-gap.md`: paratreet2's fof3 reported
EVERY component (singletons included), while old paratreet FoF and ChaNGa FoF
prune components below a min particle count by convention (old FoF's `-c
min_vertices_per_component`). That convention — not PBC — fully explained the
student's 23M-vs-2M gap at 80M. This step gives fof3 the same knob so it is a
fair drop-in comparison.

## 1. The flag

`-m <int>` (fof3 app flag, parsed by getopt in `examples/fof3/Main.C`):
minimum component size, in particles, for REPORTING. Default `0` = report
everything (pre-step-5 behavior, byte-identical output — no extra line).

`-m` needs **no** `conf.release_arg(...)`: unlike `-b`/`-c`/`-u` (which the
framework's Loadable registers as `iLbPeriod`/`minVerticesPerComponent`/
`iFlushPeriod` and would otherwise consume), no framework field registers the
letter `m` (see `src/Configuration.h::register_fields`), so `Loadable::parse`
leaves `-m`'s argv text in place for getopt. Confirmed by grep and by the runs
below.

## 2. Semantics — a REPORTING FILTER, not a partition change

A component **survives** if its size (particle count) `>= m`. When `m > 0` the
harness prints one extra line in the FOF3STAT block, right after the unchanged
(unpruned) `components:` line, so BOTH the full and the pruned numbers are
always visible:

```
FOF3STAT components: <total> max_size <..> log2_histogram: <k:count ...>
FOF3STAT surviving: <count> components with size >= <m>, max_size <..> log2_histogram: <k:count ...>
```

The surviving count/max/histogram are computed from the SAME per-label size
data each check mode already produces — no second gather or reduction:

- **full mode**: the `tips_seen` (label -> count) map already built on PE 0
  from the final particle gather; survivors are tallied inline in
  `examples/fof3/FoF3.C::traversalFn`.
- **stats mode**: `paratreet::runFoFComponentHistogram(fof, m)` now takes the
  threshold and fills `surviving_bins/surviving_count/surviving_max_size` in
  `FoFComponentHistogram` from the same merged `collectLabelCounts` gather it
  already does (`src/FoFPhase1.h`).

The log2 binning of the surviving histogram is identical to the `components:`
line, so the two histograms are comparable bin-for-bin.

**Deliberate NON-goal for now**: this is a reporting filter only. It does NOT
relabel below-threshold particles to a sentinel/"field" group and does NOT
touch the validated core partition or the `-c full` grid-hash verification and
partition-equality (`FOF3 TEST PASSED`) checks. Those still run over the full,
unpruned partition and stay byte-identical. If a future application needs
actually-relabeled output (field particles moved to a sentinel group) rather
than filtered reporting, `src/uf2/unionFindLib`'s `prune_components(threshold)`
entry could be wired to produce in-library pruned labels; that is out of scope
here and unnecessary for the comparison this step exists to enable.

## 3. Validation — reproducing old FoF's pruned counts

Real cosmological data: `lambs.00200_subsamp_1M` (1M particles, tipsy, box
[-0.5,0.5]^3), `-d oct`, default `-b` (harness computes
b = 0.00200000357627 at factor 0.2 — matches the reference exactly). Runs at
`+p2` (full mode; N < 20M auto-gate) and cross-checked in stats mode at
`+p4 ++ppn 2` (2 procs x 2 PEs, ++local) — identical numbers both.

paratreet2 (this step) vs old FoF (`design/investigation-80M-gap.md`):

| threshold                | paratreet2 surviving | old FoF | delta | %     |
|--------------------------|----------------------|---------|-------|-------|
| unpruned (all)           | 379,884 (`-m 0`)     | 380,000 (`-c 0`)  | 116 | 0.03% |
| `-m 3`  vs old `-c 2` (default) | 27,542       | 27,554 (default)  | 12  | 0.04% |
| `-m 21` vs old `-c 20`   | 2,321                | 2,320 (`-c 20`)   | 1   | 0.04% |

**Threshold convention — the off-by-one, mapped exactly (flagged, not papered
over).** Confirmed against old FoF source (`paratreet/examples/FoF/FoF.C`):
`min_vertices_per_component` DEFAULTS to 2 ("default from ChaNGa", FoF.C:229),
and `prune_components(V)` drops every component with size `<= V` — i.e. old FoF
keeps components with STRICTLY MORE than `V` vertices (size `> V`, `>= V+1`;
FoF.C comment: "minimum is strictly greater than this value"). Crucially, with
NO `-c` flag old FoF STILL prunes, at the default V=2, so it keeps `size >= 3`.

paratreet2's `-m M` keeps `size >= M` (the cleaner, intuitive definition — we
do NOT replicate old FoF's strictly-greater quirk). The exact mapping is
therefore:

```
paratreet2  -m M   reproduces   old FoF  -c (M-1)          (both keep size >= M)
paratreet2  -m 3   reproduces   old FoF  DEFAULT (no -c)   (old default V=2 => size >= 3)
```

At the CORRECT offset every point agrees to within the SAME ~0.03-0.04%
partition-level boundary offset already documented between the two codebases at
the unpruned level (116 of 380,000) — well under 0.1%, so no size-definition or
counting bug: `-m 21` vs `-c 20` = 2,321 vs 2,320 (delta 1); `-m 3` vs old
default = 27,542 vs 27,554 (delta 12). The naive `-m 20` vs `-c 20` comparison
(2,432 vs 2,320, 4.8%) is ENTIRELY the `>=` vs `>` convention difference.

**This is exactly what hit the student.** They never passed `-c`, so old FoF
applied its DEFAULT threshold-2 pruning (keep `size >= 3`) and reported ~2M at
80M; paratreet2 reported the full unpruned ~23M. The reduction is old FoF's
silent default pruning, reproducible here as `-m 3`. old FoF's pruned counts
are recoverable from paratreet2's full partition by a pure size filter,
confirming the gap was pruning convention all along.

### Synthetic cross-check (`inputs/1k.tipsy`, `+p1`)

Unpruned total stays 390 for every `-m` (filter never changes the partition),
and surviving counts are monotone non-increasing in m:

```
m:        0     2    5   20   100   1000
survivors 390*  61   5    2     2      0     (*m=0 prints no surviving line)
```

### Regression

Standard fof3 small matrix (`make test`, defaults => `-m 0`): all 12 configs
`{100,1k,10k}.tipsy x {+p1,+p2,+p2 ++ppn1,+p4 ++ppn2}` PASS with identical
numbers 72 / 390 / 3549 and NO surviving line (default output byte-identical
to pre-step-5). fof1 rebuilds cleanly (shares `src/FoFPhase1.h`); the new
`runFoFComponentHistogram` threshold parameter defaults to 0 and the struct
grew only additively, so no other caller is affected. annotate/searchAlgos
untouched.

## 4. Files changed

- `examples/fof3/Main.h`: `int fof_min_component_size = 0;` + doc.
- `examples/fof3/Main.C`: `-m` getopt case (`b:c:u:m:`), usage text, config
  echo. No `release_arg` (letter is free).
- `examples/fof3/FoF3.C`: `printSurvivingLine` helper; survivor tally from
  `tips_seen` (full mode) and from the histogram struct (stats mode); prints
  the surviving line only when `m > 0`.
- `src/FoFPhase1.h`: `FoFComponentHistogram` gained `surviving_bins/
  surviving_count/surviving_max_size/min_component_size`;
  `runFoFComponentHistogram` gained an optional `int min_component_size = 0`
  and fills those from the same merged counts (no extra gather).
