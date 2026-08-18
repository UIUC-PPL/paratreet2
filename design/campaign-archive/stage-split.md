# phaseA stage split — SELF vs CROSS (relay12)

Jobs **5301010** (16 nodes, 128 processes) and **5301011** (64 nodes, 512
processes), 2026-08-18. Tree `61685b7` on `phaseab-campaign`, CLEAN.
Binary `bin/FoF3.2b.stagesplit`, md5 `dbad388ed959e475e285ca4e67baf298`,
production charm (TraceSummary 0, vfmadd 0). Config `-u dist`,
`FOF_PE_SETS=14 FOF_PE_SETS_MODE=1`, `FOF_S3=0`, `+ppn 14`.

**All 8 2B arms exact (424,897,832); both 10k gates exact (3549); stderr
empty on both jobs. `phaseA_stages` lines: 128 and 512 — full coverage,
no stale build.**

---

## 0. The -E default confirmation (part a)

The `default` arm passes **no `-E` flag at all**, so it exercises the value
shipped in `Main.h` after `f5ac39f`. It behaves as intended:
`peak_edge_buf` is exactly **16** on every default arm (the batch cap), and
**6452** (16 n) / **3465** (64 n) with `-E 0`. The old 4096 default was
above both, which is why it never fired.

| | 16 nodes | | 64 nodes | |
|---|---|---|---|---|
| | default (`-E 16`) | `-E 0` | default (`-E 16`) | `-E 0` |
| phase3_walk | 0.739 | 0.348 / 0.351 | 0.870 / 0.875 | 0.223 / 0.290 |
| uf2 | 0.070 | 0.664 / 0.622 | 0.076 / 0.079 | 0.699 / 0.716 |
| walk+uf2 | **0.809** | **1.012 / 0.973** | **0.946 / 0.954** | **0.922 / 1.006** |
| Iteration 0 (ms) | **5159.2** | 5387.1 / 5337.4 | 2758.2 / 2786.1 | 2691.7 / 3010.0 |

- **16 nodes: −203 ms** against the `-E 0` mean (5362.2), a fourth
  independent reproduction of the relay11 result (−197, −208 ms).
- **64 nodes: inside the noise.** default mean 2772.1 vs `-E 0` mean 2850.9;
  the `-E 0` reps span 318 ms on their own. Consistent with relay11's
  −0.8%. The overlap win stays a 16-node phenomenon.
- **Caveat, stated plainly:** `default-r1` at 16 nodes was a cold-cache
  outlier (Iteration 0 **7084.2** ms, Pre-traversal 4290 ms vs ~2450 ms on
  every later arm, `upwardPass` 2.187 s vs 0.353 s). It is excluded, which
  leaves the 16-node comparison at **one** clean default rep against two
  `-E 0` reps. The walk/uf2 signature is unambiguous regardless, and three
  prior jobs already carry the magnitude.

---

## 1. Work split: the SELF stage is nearly all of phaseA

Summed PE-seconds over all processes, Iteration 0:

| | 16 nodes (128 proc) | 64 nodes (512 proc) |
|---|---|---|
| self | 1755.2 s (**94.6%**) | 1573.7 s (**91.9%**) |
| cross | 100.8 s (5.4%) | 138.6 s (8.1%) |
| **self : cross** | **17.4 : 1** | **11.3 : 1** |
| per-process ratio (median) | 19.1 | 12.5 |
| per-process ratio (min–max) | 5.2 – 38.6 | 1.2 – 86.8 |

The laptop 1M figure of ~10:1 is **not** what 2B shows at 16 nodes — it is
17.4:1 there. The cross share *grows* with node count (5.4% → 8.1%), so
the laptop number is closer to the 64-node behaviour. Reps agree to three
digits (17.41 / 17.61 at 16 n; 11.33 / 11.35 at 64 n).

## 2. But the SKEW lives in the CROSS stage

Within-process skew of a stage = `stage_max / (stage_sum / 14)`.

| | 16 nodes | 64 nodes |
|---|---|---|
| SELF stage | median **1.101**, p90 1.185, max 1.261 | median **1.086**, p90 1.158, max 1.310 |
| CROSS stage | median **2.776**, p90 4.244, max 5.692 | median **2.646**, p90 4.375, max 7.403 |
| TOTAL phaseA (`load_model`) | median 1.086, max 1.233 | median 1.101, max 1.474 |

The self stage — the part any PE may take — is already almost flat.
The owner-bound cross stage is skewed by a factor of ~2.7, up to 7.4.

Accounting check: `(self_sum + cross_sum) / pa_sum_s` = **0.90** median at
both scales. About 10% of phaseA is outside the two instrumented stages;
it is treated as spreading evenly below.

---

## 3. The gating measurement `phasea-reassignment.md` §3 asked for

That section said the recorded skew is global and cannot separate
within-process from cross-process, and set a stopping rule: *if within ≈ 1,
STOP, the item becomes cross-process placement.* The instrument now
answers it.

Decomposition of the phaseA critical path — the process whose `pa_max_s`
equals the phaseA wall time:

| | 16 nodes (proc 57) | 64 nodes (proc 164) |
|---|---|---|
| phaseA wall = its `pa_max` | 2.004 s | 0.522 s |
| its own floor `pa_sum/14` | 1.634 s | 0.414 s |
| **within-process skew** | **1.226** | **1.262** |
| mean floor over all processes | 1.147 s | 0.265 s |
| **ACROSS**-process excess | 0.486 s (**57%**) | 0.149 s (**58%**) |
| **WITHIN**-process excess | 0.370 s (**43%**) | 0.108 s (**42%**) |
| across-process imbalance max/mean | 1.426 | 1.602 |

**The 1.28 on record is confirmed — at the critical process** (1.226 and
1.262), not as a typical value. The *median* process has within-skew of
only 1.086–1.101. Both matter, and they are different numbers: rebalancing
is worth doing only where phaseA is actually set.

So within-skew is **not** ≈ 1, and the stopping rule does not fire. But
**57–58% of the excess is across-process and no within-process scheme can
touch it.**

---

## 4. What the scheme can actually reach — the barrier is NOT the binding constraint

**This section was rewritten 2026-08-18 11:43 after Kale challenged the
"no barrier" model. The original version recommended building the scheme
barrier-free on the strength of a −7.1% figure. That recommendation is
withdrawn; the figure is real but it measures something else. See §4.4.**

### 4.1 Why cross work is owner-bound (the actual mechanism)

It is not a barrier. `phaseAAdmit` (`fof/FoFPhase1.h:2004`) appends each
claimed piece to a **PE-local flat `uf_parent` array** at its own offset;
`certRep(a, sa)` / `connectedRep(a, sa)` index into that array. A cross pair
`(i,j)` can only be walked by a PE that holds **both** pieces in its own
`uf_parent`. That is an addressing constraint, not a synchronisation one.

### 4.2 Kale's incremental variant does work

Cross pair `(i,j)` becomes available the moment the later of `i`, `j`
finishes its self walk; each pair is then enumerated exactly once, when its
second endpoint lands. Suppression survives, because the memo that makes
`cross(i,j)` cheap is built by `self(i)` and `self(j)` — both done by
construction. **So no barrier is structurally required.** The current claim
path takes the simpler route (`fof/FoFPhase1.h:2114`): self runs at claim
time, the cross double loop runs over the realized set afterwards.

### 4.3 But self and cross cannot be scheduled independently

Claiming a piece **both assigns it and self-walks it**. A PE holding `p`
pieces owes `p(p−1)/2` cross pairs. Taking one more piece to fill idle time
adds one self walk **and `p−1` cross pairs**. You cannot shift self work
without shifting cross work with it — the unit of movement is a piece, and it
carries both stages.

The original model, `max(pa_sum/14, cross_max)`, assumed the two were
independently schedulable. They are not, so it does not describe any
self/cross stage-splitting scheme.

### 4.4 What the numbers actually measure

| new phaseA wall | 16 nodes | 64 nodes | what it is |
|---|---|---|---|
| today | 2.004 s | 0.522 s | |
| stage split **with** barrier | 1.922 s (**−82 ms, −1.6%**) | 0.536 s (**+14 ms, a LOSS**) | value of separating the stages — **still valid** |
| perfect within-process **assignment** | 1.636 s (**−368 ms, −7.1%**) | 0.424 s (**−98 ms, −3.5%**) | **not** the barrier-free stage split; the ceiling of a better **claim policy** |
| perfect everywhere | 1.147 s (−857 ms) | 0.265 s (−9.2%) | across-process too |

The −368 / −98 ms row is `max(pa_sum/14, cross_max)` per process, and
`cross_max` never binds — **0 of 128 and 0 of 512 processes** have `cross_max`
above their own floor (median 0.117x the floor at 16 nodes, 0.169x at 64).
So it is exactly the per-process floor: **the ceiling of perfect assignment
of pieces to PEs within a process.** Same number, different mechanism — and
the mechanism is what tells you what to build.

### 4.5 The self stage is already balanced, by machinery that is already on

Both jobs ran `FOF_STEALA=1` / `FOF_STEALA_GEO=1`, so the claim pool was
live. It is doing real work:

| | processes | own claims | foreign claims | foreign share | PEs stealing (of 14) |
|---|---|---|---|---|---|
| 16 nodes | 128 | 56,125 | 7,821 | **12.2%** | mean 10.7 (6–13) |
| 64 nodes | 512 | 212,578 | 31,268 | **12.8%** | mean 10.5 (5–13) |

**The 1.09–1.10 self skew is the residual *after* stealing.** There is no
large untapped self imbalance for a self-stealing scheme to harvest — the
existing claim pool already took it. That, not the barrier, is why the stage
split is worth so little.

### 4.6 What would actually move the number

The claim pool picks own pieces first, then nearest-centroid among unclaimed
(`fof/FoFPhase1.h:2089`). It balances *pieces taken*; it does not price the
**marginal cross cost** of taking one, which is `p−1` new pairs against the
`p` already held. A claim priority that charges a piece its marginal cross
cost is a small, local, testable change, and §4.4 says the headroom is
−368 ms at 16 nodes and −98 ms at 64.

Unmeasured but worth noting: total cross work is **convex** in pieces-per-PE
(`sum p_i(p_i−1)/2`), so equalising piece counts should *reduce* total cross
work, not merely redistribute it. The instrument reports per-process sums and
maxima only, not per-PE vectors, so this could not be checked here. **A
per-PE (pieces, self, cross) dump is the one thing missing** to settle it.

## 5. Where the plateau's excess sits

Top 19 processes by `pa_max_s` (relay10 item 50c) against the rest:

| 16 nodes | pa_max | self_sum | cross_sum | self_max | cross_max | s:c |
|---|---|---|---|---|---|---|
| top-19 | 1.732 | 18.458 | 1.381 | 1.470 | 0.338 | 13.4 |
| rest (109) | 1.188 | 12.886 | 0.684 | 1.021 | 0.145 | 18.8 |

| 64 nodes | pa_max | self_sum | cross_sum | self_max | cross_max | s:c |
|---|---|---|---|---|---|---|
| top-19 | 0.487 | 4.575 | 0.566 | 0.346 | 0.148 | 8.1 |
| rest (493) | 0.293 | 3.016 | 0.260 | 0.235 | 0.053 | 11.6 |

Excess of the plateau over the rest, split by stage:

| | self | cross | proportional would be |
|---|---|---|---|
| 16 nodes | +5.57 s (**88.9%** of excess, +43% relative) | +0.70 s (**11.1%**, **+102%** relative) | self 95.0% / cross 5.0% |
| 64 nodes | +1.56 s (**83.6%** of excess, +52% relative) | +0.31 s (**16.4%**, **+118%** relative) | self 92.1% / cross 7.9% |

Two readings, both true:

1. **In absolute seconds the plateau's excess is mostly self work** — 84–89%
   of it. A scheme that rebalances self does address the bulk of what makes
   these processes slow.
2. **But cross is over-represented in the excess by about 2x** — 11.1% of
   the excess against a 5.0% work share, 16.4% against 7.9%. In relative
   terms plateau processes have **roughly double the cross work** of the
   rest (+102%, +118%) while having only ~40–50% more self work. Cross
   volume is the sharper *identifier* of a plateau process, even though it
   is not the bulk of its excess.

This is consistent with §4: the cross stage is small, skewed, concentrated
on exactly the processes that set the critical path, and owner-bound.

---

## 6. Bottom line

- The shipped `-E 16` default is confirmed correct and firing (`peak_edge_buf`
  16 vs 6452). −203 ms at 16 nodes, neutral at 64.
- phaseA is **94.6% / 91.9% self work**, but the self stage is nearly
  balanced (1.09–1.10) while the small cross stage is skewed 2.7x.
- Within-process skew at the critical process is **1.226 / 1.262** — the
  1.28 on record is real, and the `phasea-reassignment.md` §3 stopping rule
  does not fire.
- **57–58% of the phaseA excess is across-process** and unreachable by any
  within-process scheme. That caps the item at −7.1% of Iteration 0 at 16
  nodes and −3.5% at 64, even executed perfectly.
- **The self/cross stage split is worth −1.6% at 16 nodes and a small loss
  at 64, and removing its barrier does not rescue it** — self and cross move
  together because a piece carries both. The self stage is *already* balanced
  by the live claim pool (12.2% / 12.8% foreign claims); 1.09–1.10 is the
  residual after stealing, not an untapped reserve.
- The within-process headroom that does exist — **−368 ms (16 n) / −98 ms
  (64 n)** — belongs to **better assignment**, i.e. a claim priority that
  prices a piece's marginal cross cost. Nothing structurally blocks it:
  `cross_max` is below its process floor at 0 of 128 and 0 of 512 processes.
- The larger remaining lever is still **across-process placement**, worth the
  other 57–58% (max/mean 1.43 at 16 nodes, 1.60 at 64, and growing with
  scale).
