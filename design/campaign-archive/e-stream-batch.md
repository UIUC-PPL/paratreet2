# Walk / union-find overlap: the `-E` edge-stream batch sweep at 2B / 16 nodes

Job **5295956**, 2026-08-17 17:46–17:49 EDT. 16 nodes, 8 processes/node,
`+ppn 14`. Tree `ac600f0`, CLEAN (nothing uncommitted). Binary
`bin/FoF3.2b.estream`, md5 `b820ba7efcc670022168ea80011ff00b`,
TraceSummary 0, vfmadd 0, `pe_sets` present.
**10/10 arms exact (424,897,832); 10k gate exact at `-E` 0, 16 and 4096;
128 `pe_sets` lines on every arm; empty stderr.**

Every arm: `-u dist`, `FOF_PE_SETS=14 FOF_PE_SETS_MODE=1`, `FOF_S3=0`,
plus `FOF_STEALA=1 FOF_STEALA_GEO=1 FOF_PB_PARTS=16 FOF_PB_M2KEY=1
FOF_PHASEB_SLICE_MS=2`. Only `-E` varies. Two interleaved reps.

## 1. The answer in one line

**Overlap was never switched on.** `-E 4096` — the default under which
every 2B run in this campaign has been made — is indistinguishable from
`-E 0`. Turning the batch down to 512 or below buys **−0.21 s on
(phase3_walk + uf2) and −208 ms on Iteration 0 (−3.8%)**, and it is
separable: every control rep is slower than every treated rep.

## 2. The table

| arm | walk | uf2 | **walk+uf2** | gather | relabel | edges | peak_edge_buf | phase1 | phaseA | Pre-trav | Tree trav | **Iter0** |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| E0 r1    | 0.346 | 0.670 | 1.016 | 0.005 | 0.128 | 1,655,535 | **6453** | 2.113 | 2.055 | 2498.1 | 1646.2 | 5424.2 |
| E0 r2    | 0.359 | 0.692 | 1.051 | 0.004 | 0.127 | 1,655,638 | **6451** | 2.115 | 2.056 | 2500.0 | 1672.6 | 5443.6 |
| E4096 r1 | 0.362 | 0.647 | 1.009 | 0.005 | 0.128 | 1,655,598 | 4096 | 2.118 | 2.059 | 2502.2 | 1645.5 | 5421.6 |
| E4096 r2 | 0.414 | 0.654 | 1.068 | 0.004 | 0.128 | 1,655,731 | 4096 | 2.130 | 2.072 | 2515.3 | 1693.3 | 5483.5 |
| E512 r1  | 0.599 | 0.215 | 0.814 | 0.005 | 0.128 | 1,655,996 | 512 | 2.114 | 2.056 | 2497.7 | 1451.1 | 5216.9 |
| E512 r2  | 0.624 | 0.216 | 0.840 | 0.005 | 0.127 | 1,655,031 | 512 | 2.112 | 2.054 | 2497.8 | 1465.3 | 5233.7 |
| E64 r1   | 0.807 | 0.100 | 0.907 | 0.004 | 0.127 | 1,658,804 | 64 | 2.122 | 2.063 | 2505.3 | 1543.1 | 5320.9 |
| E64 r2   | 0.743 | 0.098 | 0.841 | 0.004 | 0.128 | 1,655,371 | 64 | 2.148 | 2.090 | 2533.8 | 1472.3 | 5282.5 |
| E16 r1   | 0.747 | 0.072 | 0.819 | 0.005 | 0.128 | 1,654,715 | 16 | 2.128 | 2.070 | 2512.9 | 1449.9 | 5229.8 |
| E16 r2   | 0.757 | 0.069 | 0.826 | 0.004 | 0.127 | 1,655,144 | 16 | 2.118 | 2.060 | 2503.6 | 1452.6 | 5225.0 |

Means of the two reps, seconds except Iteration 0 in ms:

| `-E` | walk | uf2 | **walk+uf2** | Δ vs E0 | Tree trav | **Iter0** | Δ vs E0 |
|---|---|---|---|---|---|---|---|
| 0    | 0.352 | 0.681 | 1.034 | — | 1659.4 | 5433.9 | — |
| 4096 | 0.388 | 0.651 | 1.038 | +0.005 | 1669.4 | 5452.5 | +18.7 |
| 512  | 0.611 | 0.215 | **0.827** | **−0.207** | 1458.2 | **5225.3** | **−208.6** |
| 64   | 0.775 | 0.099 | 0.874 | −0.160 | 1507.7 | 5301.7 | −132.2 |
| 16   | 0.752 | 0.071 | **0.823** | **−0.211** | 1451.3 | **5227.4** | **−206.5** |

## 3. Kale's prediction was right, and the reason is now measured

The prediction was that `-E 4096` would equal `-E 0` because the per-PE
edge yield never fills a 4096 batch. It does equal it: walk+uf2 1.038 vs
1.034, Iteration 0 5452.5 vs 5433.9 — both inside the rep spread.

The `-E 0` arm supplies the missing number, because with streaming off
the buffer is never cleared and `peak_edge_buf` becomes the true maximum
per-PE edge count: **6453**. Against a mean of 1,655,535 / 1792 PEs =
**924 edges per PE**. So at `-E 4096` the busiest PE in the machine fills
exactly one batch and everything else fires post-walk. The mechanism was
present, armed, and idle.

## 4. The gain appears in the shape the design doc said it must

Not as a lower walk. Between `-E 0` and `-E 16` the walk RISES 0.352 →
0.752 s (+0.40) while uf2 FALLS 0.681 → 0.071 s (−0.61). The union work
moved into the walk, where 0.21 s of it overlapped with walk work and
fetch stalls; the rest is paid either way. Combined is the only honest
column, and it drops 20%.

Everything outside phase 3 is flat, which is the check that nothing else
moved: phase1 2.112–2.148 s, phaseA 2.054–2.090, Pre-traversal
2497.7–2533.8 ms, edge_gather 0.004–0.005, relabel 0.127–0.128 across all
ten runs. The Iteration-0 saving is carried entirely by the Tree traversal
row (1659.4 → 1451.3 ms, −208 ms), which is where phase 3 lives.

## 5. Separability

Rep spread within an arm is at most 1.1% on Iteration 0 (E4096: 5421.6 /
5483.5); the other four arms are 0.09–0.72%. The control group spans
5421.6–5483.5 ms and the E512/E16 group spans 5216.9–5233.7 ms. **No
control rep overlaps any treated rep**, and the gap is 3.8% — well
outside the ~2% band.

## 6. No per-message overhead floor down to 16

The design doc predicted a floor where per-message cost overtakes the
overlap gain, and finding it was part of the point. It is not in this
range: `-E 16` (0.823) matches `-E 512` (0.827). At 16 edges per message
the machine-wide message count is ~103k, still small next to the walk's
own traffic, so the tax has not bitten yet.

`-E 64` sits 50–75 ms worse than both its neighbours, which is odd for a
monotone story. It also has the widest rep spread in the job (0.907 /
0.841 on walk+uf2, 5320.9 / 5282.5 on Iteration 0). Both of its reps beat
every control rep, so the direction is not in doubt; the ranking of 64
against 512/16 is one point of noise and should not be read as structure.
If it matters, it needs more reps, not more arms.

## 7. What this retracts

`design/walk-uf2-overlap.md` step (1) recorded a 2B A/B (job 19582087, on
Anvil, pre-split) of `-E 0` vs `-E 4096`, found walk+uf2 4.65/4.52 vs
4.67/4.67, and concluded **"performance-neutral"**. That A/B was sound
arithmetic on an experiment that did not run: at ~970k edges over ~1920
PEs the per-PE yield was ~505, so `-E 4096` never flushed mid-walk either.
It compared no-overlap against no-overlap. The null was an artifact of the
batch size, not a property of the mechanism.

The inference built on that null goes with it. The doc concluded that the
uf2 wall "sits in what runs AFTER all unions — find_components' own serial
latency structure ... which no amount of union overlap can touch by
design." That is now measurable directly: with streaming at `-E 16` the
uf2 bracket — remainder flush + QD + `find_components` — is **0.071 s**.
So find_components' serial structure is ~0.07 s, not 0.65 s. The other
0.6 s of the old uf2 bracket was union injection and its cascades, and it
IS overlappable. The doc's diagnosis was drawn from an untested null and
is wrong at these edge volumes.

The verdict line "KEPT with default -E 4096: zero measured cost" is
accurate but reads as harmless when it is not — 4096 is not a conservative
default, it is an off switch.

## 8. What is not answered here

This is 16 nodes only. The campaign's recommended operating point is **64
nodes** (2771 ms Iteration 0, the best absolute result), and that is where
this matters most in relative terms: at 64 nodes uf2 is 0.723 s of a
2771 ms iteration — **26%**, against 12% here. If the same 0.6 s of the
uf2 bracket turns out to be injection rather than find_components at that
scale, the effect on the headline number is several times larger than the
3.8% measured here. Nothing in the mechanism predicts it should fail at 64
nodes, but nothing measured says it holds either, and the default decision
should not be taken on 16-node data.

A second open item: the edge count is 3.4x inflated by the split, so the
per-PE yield of 924 is itself a post-split number. On an unsplit run
(491,173 edges = 274/PE) the batch would need to be smaller still for
overlap to trigger. Any `-E` default has to be stated as a default for the
split configuration.

## 9. Recommendation — SUPERSEDED, see section 11

The 16-node data alone supported changing the default to **512**. The
64-node job (5296408) refutes that. Section 11 has the confirmed
recommendation; this section is kept so the reasoning that turned out to
be wrong is visible.

> Change the default from 4096 to 512. 512 and 16 measure the same, and
> 512 is the conservative end of the tie: it bounds `peak_edge_buf` to a
> useful memory figure, keeps the message count 32x lower than 16, and
> leaves margin before the overhead floor that this sweep did not find.

No code change is required — `-E` already exists and the default lives in
`examples/fof3/Main.h:82`. Nothing was pushed and the tree is clean.

---

## 10. 64 nodes: the operating point. Job 5296408, 8/8 exact

Arms `-E 0 / 4096 / 512 / 16`, two interleaved reps, same configuration,
same binary. 512 `pe_sets` lines per arm. `-E 64` was dropped as
uninformative.

| arm | walk | uf2 | **walk+uf2** | edges | peak_edge_buf | phase1 | Tree trav | **Iter0** |
|---|---|---|---|---|---|---|---|---|
| E0 r1    | 0.251 | 0.745 | 0.996 | 2,904,361 | **3351** | 0.554 | 1437.0 | 2791.3 |
| E0 r2    | 0.219 | 0.724 | 0.943 | 2,904,708 | **3465** | 0.548 | 1391.4 | 2728.2 |
| E4096 r1 | 0.289 | 0.694 | 0.983 | 2,904,756 | **3304** | 0.552 | 1418.3 | 2773.5 |
| E4096 r2 | 0.212 | 0.723 | 0.935 | 2,904,731 | **3465** | 0.555 | 1376.6 | 2754.1 |
| E512 r1  | 0.501 | 0.555 | 1.056 | 2,906,525 | 512 | 0.554 | 1497.1 | 2840.9 |
| E512 r2  | 0.515 | 0.564 | 1.079 | 2,902,815 | 512 | 0.556 | 1514.2 | 2887.5 |
| E16 r1   | 0.876 | 0.079 | 0.955 | 2,903,811 | 16 | 0.557 | 1384.4 | 2764.8 |
| E16 r2   | 0.860 | 0.074 | 0.934 | 2,905,912 | 16 | 0.546 | 1372.7 | 2708.5 |

Means:

| `-E` | walk | uf2 | walk+uf2 | Iter0 | Δ Iter0 |
|---|---|---|---|---|---|
| 0    | 0.235 | 0.734 | 0.969 | 2759.7 | — |
| 4096 | 0.251 | 0.708 | 0.959 | 2763.8 | +4.1 (+0.1%) |
| 512  | 0.508 | 0.559 | 1.067 | 2864.2 | **+104.5 (+3.8%)** |
| 16   | 0.868 | 0.076 | 0.945 | 2736.7 | −23.1 (−0.8%) |

**The 16-node win does not scale.** Three things happen at once:

1. `-E 4096` equals `-E 0` again, more emphatically: `peak_edge_buf` is
   3304–3465, **below the 4096 threshold**, so the default did not fire on
   a single PE in the machine.
2. `-E 512` is **worse than doing nothing**, by +104 ms (+3.8%). Both its
   reps are slower than every other rep in the job, against a within-arm
   spread of ~60 ms, so the direction is real even if the magnitude is
   only just outside noise.
3. `-E 16` moves the work exactly as it did at 16 nodes — walk 0.235 →
   0.868 (+0.633), uf2 0.734 → 0.076 (−0.658) — but the two now cancel.
   Net −0.025 s on walk+uf2, −23 ms on Iteration 0, **inside** the 60 ms
   rep spread. Not separable.

The mechanism still works; what has gone is the slack it was hiding in. At
16 nodes the walk was 0.352 s and could absorb 0.21 s of union work for
free. At 64 nodes the walk is 0.235 s — it shrank with the node count —
while total edges grew 1.66M → 2.90M. There is more union work and less
walk to hide it under, so moving the work in is close to a pure transfer.

## 10a. Kale's premise, checked against the scaling job

He argued the default should be 16 because per-PE edge yield probably
falls with node count. Job 5288946 already carried the answer, in a field
nobody had read for this purpose — `peak_edge_buf` on the default `-E
4096`:

| nodes | PEs | edges | **per PE** | max per PE (`peak_edge_buf`) | 4096 fires? |
|---|---|---|---|---|---|
| 16  | 1792  | 1,655,535 | 924 | 6453 | once, on one PE |
| 64  | 7168  | 2,903,750 | 405 | 3465 | **never** |
| 128 | 14336 | 3,815,309 | 266 | 2307 | **never** |

Total edges rise with node count and per-PE yield falls, exactly as he
said. **The default has never fired at 64 or 128 nodes in any run this
campaign has made.** A 512 batch is also above the per-PE mean at 64 and
128 nodes, so 512 is not a scale-safe choice either — it fires only on
above-average PEs, and it measured as a regression where it does fire.

## 11. Recommendation, confirmed

**Change the default from 4096 to 16** (`examples/fof3/Main.h:82`).

The case is that 16 is the only value measured to be never worse:

| | 16 nodes | 64 nodes |
|---|---|---|
| `-E 4096` (today's default) | baseline | baseline |
| `-E 512` | −208 ms (−3.8%) | **+104 ms (+3.8%)** |
| `-E 16` | **−208 ms (−3.8%)** | −23 ms (−0.8%, within noise) |

16 is also the only value whose behaviour does not depend on scale: per-PE
yield is 266–924 across 16–128 nodes, so a 16-edge batch fires everywhere
at every size, while 512 and 4096 fire on a shrinking subset as you scale
out. A default that quietly turns itself off as the machine grows is the
trap this whole investigation uncovered, and 512 repeats it one scale
later.

Two honest caveats to state alongside it:

- **The win is a 16-node win.** At the operating point it is −0.8% and
  inside the rep spread. Adopting 16 is justified by "free at 64 nodes,
  real at 16", not by a gain at scale.
- **128 nodes is untested** for `-E`. Per-PE yield there is 266, so 16
  still fires everywhere, but whether the transfer stays neutral or turns
  negative as the walk shrinks further is not known. 128 nodes is already
  a regression for other reasons (relay10 item 52), so this is low
  priority, but it is a gap.

## 11a. Full-machine projections. Job 5296573, 4/4 exact

Requested as the campaign's one full-projections record. All 1792 PEs, no
`+traceprocessors`. Binary `bin/FoF3.2b.estream.proj` from the clean
`ac600f0` tree against `tracedcharm`: TraceProjections 475, TraceSummary 0,
`pe_sets` 1, vfmadd 0, md5 `99061e4861b4401ed74c4707d3b7df9e`. The tree was
relinked back to production charm afterwards and
`bin/FoF3.2b.estream.restored` verifies all-zero — **the tree is not left
traced.** Script: `scripts/build-estream-proj.sh`.

Untraced controls ran in the same job so the traced numbers can be checked:

| run | walk | uf2 | walk+uf2 | peak | phase1 | Tree trav | Iter0 |
|---|---|---|---|---|---|---|---|
| ctrl-E4096 (untraced) | 0.363 | 0.697 | 1.060 | 4096 | 2.072 | 1695.2 | 5437.2 |
| ctrl-E16 (untraced)   | 0.782 | 0.071 | 0.853 | 16 | 2.096 | 1483.6 | 5240.2 |
| proj-E4096 (traced)   | 0.379 | 0.732 | 1.111 | 4096 | 2.065 | 1739.7 | 5476.7 |
| proj-E16 (traced)     | 0.882 | 0.080 | 0.962 | 16 | 2.068 | 1591.9 | 5317.9 |

Two things to carry into the viewer:

- **The controls reproduce the result a third time**, independently of job
  5295956: −197 ms, uf2 0.697 → 0.071. Three separate jobs now agree.
- **Tracing costs more on the `-E 16` arm than on `-E 4096`** (+1.5% vs
  +0.7% on Iteration 0), which is expected — streaming creates ~103k extra
  messages to record. So the traced pair shows −159 ms where the untraced
  pair shows −197 ms. Read the traces for the SHAPE of the overlap, and
  take the magnitude from the untraced runs.

Both sets: 1794 files, 2.7 GB each, `.sts` included.

## 12. What section 8's worry got right and wrong

Section 8 (written before the 64-node data) predicted the effect might be
**several times larger** at 64 nodes, because uf2 is 26% of the iteration
there against 12% at 16 nodes. That reasoning was wrong in a way worth
recording: it treated uf2's share as the size of the prize, when the prize
is actually bounded by how much idle time the *walk* has to absorb it.
uf2's share grew and the walk's slack shrank, and the second effect won.

The corrected rule of thumb for anything else in this campaign that
proposes to overlap X with Y: the ceiling is Y's idle time, not X's cost.

## Files

- sbatch: `~/software/sbatch/e-stream-2b-16n.sbatch` (16 nodes),
  `~/software/sbatch/e-stream-2b-64n.sbatch` (64 nodes),
  `~/software/sbatch/e-stream-proj-2b-16n.sbatch` (full-machine projections)
- logs: `/lustre/orion/csc710/scratch/lvkale/s3ab/5295956/` (16n),
  `.../5296408/` (64n), `.../5296573/` (projections)
- job output: `.../e-stream-2b-16n-5295956.out`,
  `.../e-stream-2b-64n-5296408.out`, `.../e-stream-proj-2b-16n-5296573.out`
- table tool: `~/software/scripts/etable.sh`
- binaries: `bin/FoF3.2b.estream` (production, md5 `b820ba7e…`),
  `bin/FoF3.2b.estream.proj` (projections, 475 syms, md5 `99061e48…`),
  `bin/FoF3.2b.estream.restored` (production relink after tracing,
  md5 `6f70301c…`)
- traced build script: `~/software/scripts/build-estream-proj.sh`
