# MEASURED: the PE-set split works — phase 1 −390 ms, Iteration 0 −406 ms,
# and the phase-3 bill is 377 edges
# Frontier 2026-08-16. Jobs 5287404 (failed gate) and 5287418 (results).
# 4f49227 + patches/0010 + patches/0011. `bin/FoF3.2b.pesets2`,
# md5 `e2784b0b3112651c2a9072273f556b61`, TraceSummary 0, vfmadd 0.

Kale's idea: treat the outlier process's PEs as s independent sets for phase 1,
leave cross-set piece pairs out of the phaseB pool, let phase 3 find them.

**All 13 2B arms exact (424897832), and all four 10k set modes exact.**

## 1. The first attempt FAILED the 10k gate, and the reason is worth keeping

Job 5287404: 10k returned **3642 against a gold of 3549** — 93 merges lost.
The diagnostic that pinned it in one step: `leaf_visits 279` was **identical**
to the control while `same_frag` halved, 74 → 38. Same reach, fewer merges:
the pairs were never entering the traversal at all.

Cause — `FoFPhase3.h:113`, Kale's own ownership prune of 2026-08-05:

> **SkipLocalSource**: discard any pair whose source node is this process's own
> live data before descending it. Correct by phase-1 completeness: two
> particles within the linking length holding different tips are necessarily on
> different processes…

That is exactly the assumption Kale suspected, and I missed it in the analysis:
I checked the visitor's edge predicate (no ownership test), the SEEN table, and
the same_frag prune, found all three safe, and stopped one layer too early. The
prune is not in the visitor — it is a **static trait consumed by
`Traverser.h::dualSkipLocalSource`, which discards local-source pairs before
`open()` is ever called**. My "same_frag 43353 proves the walk visits these
pairs" inference was wrong.

Fix (patches/0011): a runtime veto — the prune is disabled on any process where
the split is active. All four 10k modes then exact.

The veto is deliberately COARSE: it disables the prune for **every** local
source on that process, not just cross-set ones, restoring the whole
local-versus-local certificate sweep. Section 3 shows what that costs.

## 2. VICTIM-ONLY SPLIT (node 55) — the best result of the campaign

| arm | phase1 | phaseB stage | phaseB max | phase3_walk | uf2 | unique edges | Iter0 |
|---|---|---|---|---|---|---|---|
| base (mean 2) | 3.324 | 1.322 | 1.263 | 0.303 | 0.438 | 485013 | 6421.1 |
| s=2 blocked | 3.199 | 0.950 | 0.898 | 0.292 | 0.434 | 485057 | 6279.0 |
| s=2 round-robin | 3.127 | 0.779 | 0.745 | 0.299 | 0.448 | 485103 | 6231.0 |
| s=7 | 3.107 | 0.818 | 0.785 | 0.288 | 0.444 | 485189 | 6220.9 |
| **s=14 (mean 2)** | **2.934** | **0.802** | **0.768** | **0.285** | 0.447 | 485390 | **6015.1** |

- **phase 1: 3.324 → 2.934 s, −390 ms (−11.7%)**
- **Iteration 0: 6421 → 6015 ms, −406 ms (−6.3%)**
- phaseB max 1.263 → 0.768, −39%

**And the phase-3 bill is nothing.** `phase3_walk` 0.303 → 0.285 (it went
DOWN, i.e. below noise), `uf2` +9 ms, and the edge count rises by **377** —
485,013 → 485,390 — while 74,543 piece pairs were dropped from the pool. That
is the first-witness discount in its purest form: nearly every cross-set pair
connects fragments already joined by another path, so SEEN suppresses it.

For scale: the best shedding configuration measured −49 ms on Iteration 0 and
cost 119 ms of migration. **This is 8x better and costs nothing to set up** —
no migration, no data movement, no fixed per-process step.

Two secondary readings:
- **Round-robin beats blocked at s=2** (0.745 vs 0.898 phaseB max), as
  predicted: it pushes more weight into phase 3, and phase 3 is cheap.
- **The benefit saturates by s=2 round-robin.** s=7 and s=14 are no better,
  because 0.768 is where process #2 sits — the same "level to #2" ceiling
  shedding hit. Going further needs more processes treated, not more sets.

## 3. MACHINE-WIDE — a large net loss, and it prices the sweep

| arm | phase1 | phaseB | phase3_walk | uf2 | edges | relabel entries | Iter0 |
|---|---|---|---|---|---|---|---|
| base | 3.324 | 1.322 | 0.303 | 0.438 | 485013 | 745540 | 6421.1 |
| all s=2 | 3.655 | 1.134 | **2.305** | 0.801 | 789133 | — | 9306.2 |
| all s=14 (mean 2) | **2.086** | **0.000** | **2.357** | **2.054** | 1647667 | 2449538 | **9576.4** |

Machine-wide s=14 does exactly what it promises to phase 1 — **phaseB is
GONE, 1.322 → 0.000, and phase 1 falls to 2.086 s** — and then loses 3.2 s in
phase 3:

- `phase3_walk` 0.29 → 2.36 s (**+2.07 s**). This is the local-versus-local
  certificate sweep that the ownership prune was added to remove, now measured:
  it costs about 2.1 s machine-wide. The coarse veto is paying for all of it,
  including the same-set pairs it did not need to walk.
- `uf2` 0.44 → 2.05 s (**+1.61 s**) and the relabel map 745,540 → 2,449,538
  entries (3.3x). This is the fragment inflation the analysis flagged as the
  second cost, and at s=14 machine-wide it is large.
- unique edges 485,013 → 1,647,667 (3.4x).

So the failure mode named in the analysis is real, and it is the coarse veto
plus fragment inflation, not the idea. **Narrowing the veto to cross-set pairs
only is the obvious next step** — it would remove most of the +2.07 s while
keeping the phase-1 gain, and it is what makes a top-N version viable.

Note also `all-s14` phase1 = 2.086 s with phaseB at ZERO. That is phaseA alone,
and it confirms the phase-1 floor from `phase1-idle-structure.md`: phaseA is a
19-process plateau that this idea does not touch.

## 4. PHASE-B STEALING: keep it. It is worth 1.8 s.

| arm | phase1 | phaseB stage |
|---|---|---|
| base, S3 on | 3.324 | 1.322 |
| base, **S3 off** | **5.140** | **3.338** |
| victim s=14, S3 on | 2.934 | 0.802 |
| victim s=14, **S3 off** | 3.494 | 1.726 |
| machine-wide s=14, S3 on | 2.086 | 0.000 |
| machine-wide s=14, S3 off | 2.083 | 0.001 |

- **S3 stealing is worth 1.82 s on the baseline** (5.140 → 3.324). It is not a
  marginal mechanism and should not be turned off.
- The victim split recovers most of what stealing does when stealing is absent
  (5.140 → 3.494) — they are partial substitutes — but **S3 is still worth
  560 ms on top of the split** (3.494 → 2.934). Use both.
- Only in the machine-wide variant does stealing become free to remove (2.086
  vs 2.083), and only because phaseB is empty. That is Kale's "turn off all
  phaseB steals" endpoint: reachable, but currently at a 3.2 s net cost.

## 5. Where this leaves things

**Adopt now:** victim-only split, `FOF_PE_SETS=2 FOF_PE_SETS_MODE=1
FOF_PE_SETS_NODE=<measured worst>`, with S3 left on. −390 ms of phase 1 and
−406 ms of Iteration 0, exact, zero setup cost. s=2 round-robin is enough;
s=14 is no better.

**Do next, in order:**
1. **Narrow the veto** to "source and target in different sets" rather than
   "any local source". §3 prices what the coarse version wastes: up to 2.07 s
   of walk. This is the single highest-value follow-up in the campaign now.
2. Then re-run the machine-wide and top-N arms. With the narrow veto, the
   walk cost should scale with cross-set pairs only, and the top-6 version
   becomes the natural target — `phase1-idle-structure.md` showed the tail is
   broad, and this mechanism has no per-process cost.
3. Fragment inflation (uf2, relabel_map) remains the second cost and is
   unavoidable in principle; at victim-only scale it is +9 ms, at machine-wide
   s=14 it is +1.6 s. It bounds how far a top-N version can go and should be
   tracked on every arm.

---

# PART 2: the narrow veto and TOP-N. Jobs 5287618 (failed gate) and 5287653.
# `bin/FoF3.2b.pesets4`, md5 `2bd50769105e32d38cc803bd31eb904f`, patches/0012.
# **All 18 2B arms exact.**

## 6. Clarification: what "machine-wide" meant

`FOF_PE_SETS_NODE` unset — the split applied on **all 128 processes**, each
splitting its own PEs into s sets. "Victim-only" applied it on node 55 alone.
Part 2 adds `FOF_PE_SETS_NODES`, a comma list, so the top N predicted outliers
can be treated together — which is the whole point of a mechanism with no
per-process cost.

## 7. The narrow veto failed once first, for a reason worth recording

First attempt (job 5287618) returned 3555 / 3562 / 3557 at 10k against a gold
of 3549 — a handful of lost merges, where the coarse veto had been exact.

My bug: I took the TARGET's set from `CkMyPe()`. Under `FOF_STEALA` a piece's
phase-1 set is its **claimed** PE, which need not be the PE it resides on, so
the target's set was sometimes wrong and the prune fired on a genuine cross-set
pair. Fixed by looking BOTH sides up in the piece→set table. The index spaces
are the same — the mirrored-pairs prune a few lines below already compares
`node->tp_index` with `tp.thisIndex` directly.

## 8. The narrow veto removes ~2.0 s of walk, exactly as predicted

`phase3_walk`, machine-wide s=14: **2.357 s (coarse) → 0.389 s (narrow)**.
The local-versus-local certificate sweep is no longer being paid for pairs that
never needed walking. Part 1 §3 predicted this and it is the whole difference.

## 9. Results (means of 2 reps; base = 3.267 s phase1, 6364 ms Iter0)

| arm | phase1 | Δ | phaseB | phase3_walk | uf2 | unique edges | Iter0 | Δ |
|---|---|---|---|---|---|---|---|---|
| base | 3.267 | — | 1.309 | 0.287 | 0.448 | 485013 | 6364.0 | — |
| victim s=2 rr | 3.048 | −219 | 0.833 | 0.323 | 0.446 | 485062 | 6177.3 | −187 |
| victim s=14 | 2.880 | −387 | 0.766 | 0.349 | 0.441 | 485374 | 6036.7 | −327 |
| **top2 s=2 rr** | 2.708 | −559 | 0.807 | 0.374 | 0.449 | 485065 | 5893.4 | −471 |
| **top6 s=2 rr** | **2.690** | **−577** | 0.487 | 0.340 | 0.464 | 504024 | **5873.8** | **−490** |
| **top6 s=14** | **2.639** | **−628** | 0.500 | 0.340 | 0.476 | 512837 | **5829.7** | **−534** |
| all s=2 rr | 2.412 | −855 | 0.445 | 0.336 | **1.461** | 1260794 | 7063.7 | **+700** |
| all s=14 | 2.110 | −1157 | 0.001 | 0.389 | **2.025** | 1647442 | 7612.8 | **+1249** |

**TOP-6 IS THE ANSWER. `phase1 −628 ms (−19.2%)`, `Iteration 0 −534 ms
(−8.4%)`**, exact, with a phase-3 bill of +53 ms of walk, +28 ms of uf2 and
+5.7% edges.

Going top-1 → top-2 → top-6 improves Iteration 0 from −327 to −471 to −534 ms.
That is the broad-tail thesis of `phase1-idle-structure.md` confirmed
operationally: the mechanism that can treat many processes beats the one that
could only ever treat one. Shedding's best was −49 ms.

## 10. Machine-wide is STILL a loss, and now we know exactly why: uf2

With the walk term fixed, the entire remaining penalty is fragment inflation
into the union-find:

- all s=14: `uf2` 0.448 → 2.025 (**+1.58 s**), edges 3.4x, and that alone turns
  a −1157 ms phase-1 win into a +1249 ms Iteration-0 loss.
- all s=2: `uf2` +1.01 s, edges 2.6x, +700 ms.
- top6 s=14: `uf2` +0.028 s, edges +5.7%.

So the cost is not the walk and not the traffic — **it is the union-find, and
it scales with how many processes are split, not with how finely.** top6 s=14
splits six processes fourteen ways and pays 28 ms; all-s2 splits 128 processes
two ways and pays 1.01 s. That is the governing constraint for any future
version, and it is the second cost flagged in the original analysis.

## 11. KALE'S PAYOFF ARM: with top-6 split, phaseB stealing is FREE TO REMOVE

| arm | phase1 | phaseB | Iter0 |
|---|---|---|---|
| base, S3 on | 3.267 | 1.309 | 6364.0 |
| base, **S3 off** | 5.087 | 3.321 | 8292.3 |
| top6 s=2 rr, S3 on | 2.690 | 0.487 | 5873.8 |
| top6 s=2 rr, **S3 off** | **2.687** | 0.887 | 5861.0 |

On the baseline, S3 stealing is worth **1.82 s**. With the top-6 split it is
worth **3 ms** — phase1 2.687 against 2.690, and Iteration 0 5861 against 5874,
both inside the rep spread.

**The split has fully substituted for phaseB work stealing.** All of it, within
and across physical nodes. That is the outcome Kale was aiming at, and it says
the S3 machinery — the coordinator, the grant sizing, the POD wire format, the
parallel rebuild, the cross-node protocol — can be switched off in this
configuration with no measurable loss. Note it is a substitution, not a
refutation: S3 is still worth 1.82 s when the split is absent.

## 12. Recommended configuration

```
FOF_PE_SETS=14 FOF_PE_SETS_MODE=0 FOF_PE_SETS_NODES=<top 6 by pb_sum_s>
FOF_S3=0
```

Iteration 0 −534 ms (−8.4%), phase 1 −628 ms (−19.2%), exact, no migration, no
stealing machinery. `FOF_PE_SETS=2 FOF_PE_SETS_MODE=1` on the same six is
within noise of it and cheaper in edges (+3.9% vs +5.7%), so prefer s=2 if the
union-find is under pressure.

## 13. What is left

1. **The union-find is now the binding cost** (§10). Anything that widens the
   split past ~6 processes needs uf2 to scale better, or it will be eaten.
   Kale's note that phase-3 edges go to the root in serial mode and that
   communication dominates in parallel mode is the thread to pull here.
2. **The victim list is still hand-supplied.** relay5 established that
   `m2_cross` ranks the true worst process #1 in 3/3 reps and gets 4 of the
   top 5; wiring that in makes this self-configuring, and it is now worth doing
   because the payoff is −534 ms rather than −49 ms.
3. **phaseA's 19-process plateau (0.87 s) is untouched** and, with phaseB down
   to 0.50 s on the top-6 arm, it is now most of what remains in phase 1.
