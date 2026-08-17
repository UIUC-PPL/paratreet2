# Analysis: splitting the outlier process's PEs into sets, and letting the
# cross-set pairs fall through to phase 3
# Kale's idea, 2026-08-16. Analysed against the code at 4f49227, not the docs.

**Verdict: correct by construction, and the implementation really is one
condition. But the framing hides one serious objection — this moves work
between PHASES, not between PROCESSES — and whether it pays turns entirely on
a discount that is plausible, partly evidenced, and not yet measured.**

## 1. Correctness — checked in the code, three mechanisms, all safe

Kale's own worry was "there may be some assumption that all edges in phase 3
are cross-process". There is such an assumption **in a comment**, but not in
the code.

**(a) The edge predicate has no ownership test.** `fof/FoFPhase3.h:24-30`:

> Correctness of the edge predicate: after phase 1, any two particles within b
> of each other that hold DIFFERENT tips are necessarily owned by different
> processes (phase 1 is the complete FoF restricted to a process), **so no
> ownership test is needed**: different-tip pairs within b are exactly the
> merge edges. Same-tip pairs are skipped.

The invariant is quoted as the JUSTIFICATION for not testing ownership. The
code tests **tip difference**, nothing else. Kale's change falsifies the
premise — after it, two particles within b on the SAME process can hold
different tips — but the conclusion the code implements ("different tips within
b ⇒ emit an edge") stays exactly right. Phase 3 will emit those edges.

**This comment becomes false and must be rewritten in the same commit.** It is
precisely the kind of load-bearing comment that will be cited later to justify
some future optimisation that WOULD break.

**(b) Suppression cannot suppress an unemitted edge.** `fof/FoFPhase1.h:385`:

> SEEN is only ever set at the moment an edge is emitted (winner emits), so
> suppression can never suppress an unemitted edge.

`seen3_pairs` is populated only by phase 3's own emissions — phase 1 never
pre-loads it. So pairs omitted from phaseB cannot be silently swallowed by
case-3 suppression. Verified: the only inserters are `trySeenInsert` from the
emit path.

**(c) `same_frag` prunes exactly the pairs that need no edge.** It fires when
both sides are in the same fragment. Our omitted cross-set pairs will be in
DIFFERENT fragments (that is the whole point), so it will not fire on them.

## 2. Empirical proof the phase-3 walk already visits these pairs

From a baseline 2B run:

```
FOF3STAT prunes: negative 105372871 positive 52 suppression 4073362
                 same_frag 43353 leaf_visits 3362007
```

**`same_frag 43353` is the proof.** During the walk, tips are still
process-local (the relabel happens later), so two pieces with the SAME
fragment are necessarily on the same process. Those 43353 prunes are
same-process pairs that the phase-3 walk reached, examined, and correctly
discarded as already merged.

So the traversal needs no change at all. It is already visiting the pairs Kale
wants to hand it; today they get pruned as same-fragment, and after the change
they will differ in tip and emit an edge instead.

## 3. The data is already local — this is the cheapest phase-3 work possible

`CacheManager` is declared `nodegroup` unless `GROUP_CACHE` is defined, and it
is not defined in this build. **One cache per PROCESS, shared by all 14 PEs.**
A walk from a PE in set A against a piece owned by a PE in set B hits the
process's own cache: no request, no network, no serialisation. Of all the work
that could be pushed into phase 3, same-process pairs are the cheapest kind.

## 4. THE OBJECTION — it moves work between phases, not between processes

This is what the idea's framing does not address, and it is the thing that
decides it.

The phase-3 walk is driven by the OWNER of the target leaves: every Partition
walks its own leaves against the global tree. Both sides of a cross-set pair
belong to node 55. **So the extra phase-3 walk lands back on node 55's own
PEs.** Nothing is offloaded to another process. Contrast shedding, which
physically relocates pieces and therefore relocates their work.

So the idea does NOT reduce node 55's total work. It converts node 55's phaseB
work into node 55's phase-3 walk work. It wins if and only if:

> **the same pairs cost materially less in phase 3 than in phaseB.**

There is a real reason to expect that, and it is structural: **phase 1 must
union every particle pair; phase 3 only needs ONE witness per fragment pair.**
Once (g,f) is in SEEN, every further pair between those two fragments is
suppressed. The counters show that regime plainly — 105.4M negative prunes and
4.07M suppression prunes yield only 491,173 emitted edges for the entire
machine's cross-process boundary.

Supporting evidence, though indirect: job 5286357 pushed 10.7% of the
machine's phase-1 pair work across a process boundary and phase 3 grew by
11 ms (`phase3_walk` 0.278 → 0.289). That is the right order for a large
discount, but it is not the same experiment — there the work also changed
process, and the walk had to fetch remotely.

**The failure mode, stated plainly:** if the descent cost dominates the
witness saving, node 55 simply pays ~14 PE-s in `phase3_walk` instead of in
phaseB, `phase3_walk` goes from 0.29 s to ~1.3 s, and the whole thing is a
loss of about 0.6 s. Nothing in the current counters rules this out.

## 5. Second cost: fragment inflation into the union-find

Not shedding-like at all, and it is the cost I would watch most after (4).

```
FOF3STAT relabel_map: entries 745540 bytes 11928640
FOF3STAT time_s: ... uf2 0.427 ...          <- the LARGEST phase-3 term
```

Leaving cross-set pairs unmerged means node 55 ends phase 1 with more, smaller
fragments — every component that straddles the split is now two tips instead
of one, and each needs an edge to be rejoined. Both the tip count (745,540
machine-wide, ~5,800 per process) and the edge count (485,013 unique) grow.
uf2 is already 0.427 s. A crude bound for one process split into s=14 sets is
+10-15% on uf2, i.e. +40-65 ms — cheaper than shedding's 119 ms migration, but
not free, and it grows with the number of processes treated.

## 6. Why it is still the most promising idea on the table

Three properties that shedding does not have:

1. **No migration, no data movement.** The 59 ms fixed step + 142 ms per
   million particles (job 5286573) disappears entirely.
2. **No per-process fixed cost.** Shedding pays its 59 ms step per process
   treated, which is why it could only ever be a top-1 tool. Omitting pairs
   from a pool costs nothing per process, so it can be applied to the top 6
   (or 25) at once — and `reports/phase1-idle-structure.md` showed that the
   tail is broad, so a broad mechanism is exactly what is missing.
3. **s is a free parameter.** s=2 removes ~54% of the process's cross-PE work
   (7×7 of the 91 unordered PE pairs); s=14 removes ALL of it, i.e. phase 1 on
   that process becomes phaseA only. There is a whole sweep here, and the
   granularity floor argument does not apply because nothing is being split.

Expected magnitude if the phase-3 discount holds: node 55's `pb_max_s`
1.19 → ~0, so the phaseB stage falls to whatever #2 is (0.76), i.e. −0.43 s of
stage and ~−0.2 s of phase-1 wall after the measured 2:1 discount. Applying it
to the six processes above 0.5 s takes the stage to ~0.43 and roughly doubles
that. Against a phase-1 wall of 3.25 s, that is real.

## 7. The implementation is one condition, exactly as Kale said

`fof/FoFPhase1.h:2731`, `buildPoolSlice()`, enumerates the pool as pairs of
DISTINCT PE buckets:

```cpp
for (auto ita = nb->pe_treepieces.begin(); ita != nb->pe_treepieces.end(); ++ita) {
  auto itb = ita;
  for (++itb; itb != nb->pe_treepieces.end(); ++itb)
    for (auto& sa : ita->second)
      for (auto& sb : itb->second)
        if ((idx++ % nranks) == rank) poolPushInto(slice, sa.root, sb.root);
}
```

The filter goes in the `itb` loop, before any unit is built:

```cpp
// FOF_PE_SETS=s on FOF_PE_SETS_NODE: treat this process's PEs as s
// independent sets for phase 1. Pairs that cross a set boundary are left
// out of the pool; phase 3 discovers them, because its edge predicate is
// "different tips within b", with no ownership test (FoFPhase3.h).
if (setsHere > 1 && setOf(ita->first) != setOf(itb->first)) continue;
```

with `setOf(pe) = (pe - CkNodeFirst(CkMyNode())) % setsHere` (round-robin, to
maximise cross-set weight) or `/ (CkNodeSize/setsHere)` (blocked, to minimise
it). **Both are worth an arm** — blocked keeps SFC-adjacent pieces together and
so minimises how much lands in phase 3, round-robin maximises it. Which is
better depends on exactly the discount in §4, so the sweep measures the thing
in question.

Note it composes with S3 for free: units never entering the pool cannot be
stolen or shipped, so no stealing path needs to know about this.

## 8. What I would measure, in order

1. **s = 1 (control), 2 blocked, 2 round-robin, 7, 14 on node 55 only.**
   Read `time_s: phase1`, `phase1_stages phaseB`, and — the decisive column —
   `time_s: phase3_walk` and `uf2`. Exactness first: 424897832 every arm. If
   the count is wrong, one of the three mechanisms in §1 is not what I read.
2. If s=14 on one process wins, apply to the top 6 by `pb_max_s` and re-read.
3. Only then consider the machine-wide version (every process, phase 1 =
   phaseA only), which is the logical endpoint and a genuinely different
   design point rather than a tuning knob.

The first job is ~6 arms x 17 s and needs no traced build.
