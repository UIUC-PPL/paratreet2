# Targeted shedding at 2B / 16 nodes — the prize is real, phase 3 does not
# bill for it, and the missing piece is SELECTION

Frontier, 2026-08-16. Job **5286357**, paratreet2 **4f49227** (branch
`phaseab-campaign`), feature landed at fb43f06.

Binary `~/software/bin/FoF3.2b.shed`, md5 `e1310be9cf57b3d8739f444bd98d72d9`,
built by a FULL clean rebuild of the stack (`scripts/rebuild-prod.sh`, which
does `make clean` in htram, unionfind/prefixLib, unionfind, paratreet2/{src,
fof,examples/fof3}) against production charm. Build checks, all four required:

```
nm -C  bin/FoF3.2b.shed | grep -ci TraceSummary       = 0   (production)
strings bin/FoF3.2b.shed | grep -c 'Targeted shedding' = 1   (feature present)
strings bin/FoF3.2b.shed | grep -c 'SHED plan'         = 1   (feature present)
objdump -d bin/FoF3.2b.shed | grep -c vfmadd           = 0   (no FMA; count is exact)
```

Exact srun line for every 2B arm (`$k`=0 means the two shed vars are not set
at all, so the feature is off):

```
FOF_SHED_NODE=55 FOF_SHED_COUNT=$k \
srun -N 16 --ntasks-per-node=8 -t 8:00 --mpi=cray_shasta --network=job_vni \
     --unbuffered --cpu-bind=none --distribution=block:block \
     $HOME/software/bin/FoF3.2b.shed -f \
     /lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024 \
     -d oct -u serial +traceoff +ppn 14 \
     +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
     +lci_ndevices 7 +backend_poll_thread 2
```

Base env on EVERY arm, as specified:
`FOF_STEALA=1 FOF_STEALA_GEO=1 FOF_PB_PARTS=16 FOF_PB_M2KEY=1
FOF_PHASEB_SLICE_MS=2 FOF_S3=1`, `-u serial`.

No `FOF_S3_XNODE` arms, no `PARATREET_PREBUILD_LB`. Nothing pushed.

Arms: probe-base (the victim finder), then base / shed4 / shed12 / shed30,
interleaved, **3 reps each**. The task asked for two; a 2B arm costs ~17 s
here and relay8 established that n=2 cannot clear the control spread, so the
third rep is nearly free insurance. All per-rep numbers are below, so the
first two alone can be read if preferred.

---

## 0. EXACTNESS FIRST

**`FOF3STAT components: 424897832` on all 13 2B arms.** 10k gate 3549 on both
of its legs. Zero mismatches, zero non-zero exit codes.

## 1. STEP 1 — the victim, measured rather than assumed

The probe baseline ran first and the shed arms took their victim from its own
`FOF3STAT load_model:` lines. Largest `pb_sum_s` of the 128 processes:

```
FOF3STAT load_model: node 55 pieces 403 n 15483519 m2_self 6.32747e+10
  m2_intra 7.48505e+10 m2_cross 6.01555e+10 self 1.33647e+08 pair 1.02496e+25
  pa_sum_s 20.719 pa_max_s 1.786 pb_sum_s 14.609 pb_max_s 1.251
```

Ranked `pb_sum_s`, probe arm:

| rank | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| node | **55** | 54 | 87 | 83 | 80 | 107 | 67 | 43 | 66 | 4 |
| pb_sum_s | **14.61** | 9.56 | 9.43 | 6.98 | 6.55 | 6.46 | 5.00 | 4.99 | 4.76 | 4.73 |

**Confirmed: node 55, for this build too.** #1 → #2 gap **1.528x**, so the
ceiling of levelling #1 to #2 is **−34.5%** of the `pb_sum_s` max — within a
point of the −33.4% the design quotes from relay5, and reproduced here as a
fourth independent draw. Median `pb_sum_s` is 1.57, so node 55 is 9.3x the
median, again matching the standing figure.

Node 55 is the same shape as before: **403 pieces against a median of ~512**,
with the same particle count as everyone else (1.55e7). Fewer, larger pieces.

## 2. The prize: phaseB_s max

`FOF3STAT balance: ... phaseB_s min/avg/max` over 1792 PEs. The max is what
sets the phase.

| arm | rep1 | rep2 | rep3 | mean | vs base |
|---|---|---|---|---|---|
| base | 1.242 | 1.177 | 1.346 | **1.254** (with probe 1.251, n=4) | — |
| shed4 | 1.268 | 1.210 | 1.274 | **1.251** | −0.3% |
| shed12 | 1.166 | 1.215 | 1.110 | **1.164** | **−7.2%** |
| shed30 | 1.301 | **0.788** | 1.067 | **1.052** | **−16.1%** |

Base spread is 1.177–1.346 (14.4% of the low end), so read those means against
that, not against zero.

- **shed4 does nothing.** As expected.
- **shed12 is a small, consistent improvement** — 2 of its 3 reps sit below the
  best base rep. It is not separable from the control spread at n=3
  (t≈1.8, p≈0.12); it is directionally clean.
- **shed30 has the best mean and the worst spread.** One rep is the best
  phaseB max the campaign has ever recorded, one is indistinguishable from
  base, one is in between. §5 explains exactly why, and the explanation is
  not noise.

**shed30-r2 at 0.788 s is a new campaign best** — the previous best was
1.23–1.32 s and the granularity floor is 0.25 s. Against the base mean that is
**−37.2%**, i.e. it slightly EXCEEDED the −34.5% ceiling computed for this run
(S3 stealing redistributes on top of the migration, so the two compose).

The full min/avg/max rows, for the record:

| arm | min | avg | max |
|---|---|---|---|
| probe-base | 0.002 | 0.148 | 1.251 |
| base-r1 | 0.002 | 0.149 | 1.242 |
| base-r2 | 0.000 | 0.148 | 1.177 |
| base-r3 | 0.000 | 0.149 | 1.346 |
| shed4-r1 | 0.000 | 0.149 | 1.268 |
| shed4-r2 | 0.004 | 0.148 | 1.210 |
| shed4-r3 | 0.004 | 0.150 | 1.274 |
| shed12-r1 | 0.000 | 0.148 | 1.166 |
| shed12-r2 | 0.002 | 0.150 | 1.215 |
| shed12-r3 | 0.000 | 0.148 | 1.110 |
| shed30-r1 | 0.002 | 0.149 | 1.301 |
| shed30-r2 | 0.005 | 0.146 | **0.788** |
| shed30-r3 | 0.002 | 0.149 | 1.067 |

The avg is flat at 0.148 everywhere — this is entirely a tail effect, which is
what it was supposed to be.

## 3. Both SHED lines fired on every shed arm

| arm | SHED plan | Targeted shedding |
|---|---|---|
| shed4-r1 | 4 of 63946 elements migrating | 19.255 ms |
| shed4-r2 | 4 of 63946 | 67.343 ms |
| shed4-r3 | 4 of 63946 | 36.631 ms |
| shed12-r1 | 12 of 63946 | 189.127 ms |
| shed12-r2 | 12 of 63946 | 168.787 ms |
| shed12-r3 | 12 of 63946 | 66.390 ms |
| shed30-r1 | 30 of 63946 | 353.342 ms |
| shed30-r2 | 30 of 63946 | 321.293 ms |
| shed30-r3 | 30 of 63946 | 146.774 ms |
| 10k gate | 2 of 330 | 0.376 ms |

Requested count moved every time, and the piece counts confirm it end to end:
node 55 goes 403 → 399 (k=4) → 391 (k=12) → 373 (k=30), and node 56 goes
388 → 392 → 400 → 418. The migration lands.

**The 10k `shed=on` leg was added deliberately**: fb43f06's reconverse gate was
inconclusive because the env never reached the ranks through `lcrun`. It
reaches them through `srun` — proven before any 2B time was spent.

Migration cost scales with k as expected (mean 41 / 141 / 274 ms for k=4/12/30)
but is noisy by a factor of 2–3 at fixed k. **At k=30 the 274 ms mean is the
same order as the 202 ms mean phaseB saving.** That matters for §6.

## 4. PHASE 3 — the open question, and the answer is favourable

`FOF3STAT time_s: uf2_setup phase3_walk edge_gather uf2 relabel`, seconds:

| arm | uf2_setup | phase3_walk | edge_gather | uf2 | relabel | P3 total |
|---|---|---|---|---|---|---|
| probe-base | 0.000 | 0.339 | 0.009 | 0.441 | 0.150 | 0.939 |
| base-r1 | 0.001 | 0.278 | 0.009 | 0.436 | 0.149 | 0.873 |
| base-r2 | 0.000 | 0.280 | 0.009 | 0.437 | 0.147 | 0.873 |
| base-r3 | 0.000 | 0.289 | 0.009 | 0.446 | 0.148 | 0.892 |
| shed4-r1 | 0.000 | 0.294 | 0.009 | 0.426 | 0.151 | 0.880 |
| shed4-r2 | 0.000 | 0.287 | 0.005 | 0.433 | 0.149 | 0.874 |
| shed4-r3 | 0.000 | 0.279 | 0.009 | 0.443 | 0.153 | 0.884 |
| shed12-r1 | 0.000 | 0.283 | 0.009 | 0.437 | 0.151 | 0.880 |
| shed12-r2 | 0.001 | 0.285 | 0.010 | 0.438 | 0.149 | 0.883 |
| shed12-r3 | 0.000 | 0.374 | 0.009 | 0.442 | 0.150 | 0.975 |
| shed30-r1 | 0.001 | 0.295 | 0.010 | 0.450 | 0.152 | 0.908 |
| shed30-r2 | 0.000 | 0.289 | 0.009 | 0.438 | 0.148 | **0.884** |
| shed30-r3 | 0.001 | 0.304 | 0.010 | 0.437 | 0.149 | 0.901 |

Base P3 total ranges 0.873–0.892 over the three base reps (0.939 on the probe,
which is the first 2B arm of the job and reads cold — treat it as such).

**The arm that moved the most work, shed30-r2, has a phase-3 total of 0.884 —
inside the base range.** That is the answer to the question nobody had
measured: at this k, on this victim, the phase-1 → phase-3 locality conversion
costs less than phase 3's own run-to-run spread.

That is not an argument from a null. The conversion is directly visible in the
m2 accounting, summed over all 128 processes (`m2_intra + m2_cross` is the
phase-1 pair work; pairs that leave phase 1 become phase-3 cache-walk work):

| arm | machine-wide phase-1 pair m2 | converted to phase 3 | P3 total | phaseB max |
|---|---|---|---|---|
| base-r1 | 3.835e11 | — | 0.873 | 1.242 |
| base-r2 | 3.834e11 | −0.0% | 0.873 | 1.177 |
| base-r3 | 3.830e11 | −0.1% | 0.892 | 1.346 |
| shed4-r1 | 3.758e11 | −2.0% | 0.880 | 1.268 |
| shed4-r2 | 3.835e11 | 0.0% | 0.874 | 1.210 |
| shed4-r3 | 3.823e11 | −0.3% | 0.884 | 1.274 |
| shed12-r1 | 3.826e11 | −0.2% | 0.880 | 1.166 |
| shed12-r2 | 3.840e11 | +0.1% | 0.883 | 1.215 |
| shed12-r3 | 3.788e11 | −1.2% | 0.975 | 1.110 |
| shed30-r1 | 3.826e11 | −0.2% | 0.908 | 1.301 |
| shed30-r2 | **3.426e11** | **−10.7%** | **0.884** | **0.788** |
| shed30-r3 | 3.758e11 | −2.0% | 0.901 | 1.067 |

**shed30-r2 pushed 10.7% of the machine's entire phase-1 pair work across the
process boundary into phase 3, and phase 3 did not move above its own noise,
while phaseB's critical path fell 0.447 s** (phase1_stages phaseB 1.269 →
0.822). On the same arm, phase3_walk went 0.278 → 0.289, i.e. +0.011 s bought
−0.447 s.

The reason the conversion is so cheap is mechanical and worth stating, because
it is a property of the crude forced rule that turns out to be protective:
**all k pieces go to the SAME destination process**, so the pairs among the
moved pieces stay in phase 1 — they just execute on node 56 instead of node 55.
Only the pairs between moved pieces and the pieces left behind convert. The
full accounting on shed30-r2, in m2:

| | m2_intra (phaseA pairs) | m2_cross (phaseB pairs) | total phase-1 pair work |
|---|---|---|---|
| node 55 lost | −3.29e10 | −4.51e10 | **−7.80e10** |
| node 56 gained | +0.03e10 | +3.73e10 | **+3.76e10** |
| net → phase 3 | | | **−4.04e10 (52%)** |

So **48% of the work that left node 55 was re-created as phase-1 work on node
56** rather than converting. Machine-wide `m2_self` is bit-identical between the
two arms (3.708e11), which confirms the pieces simply moved and nothing was
recomputed.

**Counter-check against reading noise as mechanism:** the single largest
phase-3 reading in the whole job is shed12-r3 at 0.975 (phase3_walk 0.374) —
and that arm converted only 1.2% of the pair work. The biggest phase-3 number
came from an arm that barely moved anything, which is what phase-3 noise looks
like.

**Verdict on the specific question asked: NO, this is not an arm that improves
phaseB by inflating phase 3.** Phase 3 is flat.

## 5. WHY shed30 has a 0.788 / 1.067 / 1.301 spread — it is the SELECTION

This is the most important finding in the job, and it is not noise.

The forced arm picks its pieces like this (`src/TreePiece.h:114`):

```cpp
static std::atomic<int> taken{0};
const int slot = taken.fetch_add(1);
if (slot < k) { ... shed_dest_ = CkNodeFirst(dn) + (slot % CkNodeSize(dn)); }
```

**The first k elements to execute `shedDecide` win**, in the runtime's
broadcast delivery order across the process's 14 PEs. That is effectively a
RANDOM SAMPLE of 30 of node 55's 403 pieces, not the heaviest 30. The
destination is `dn = (victim + 1) % CkNumNodes()` — always process 56 — which
is also arbitrary and, note, on a DIFFERENT physical node (block 6 is
processes 48–55, so 56 is in block 7).

That randomness is the whole spread. Node 55's own `m2_cross` — its cross-PE
pair work, which is what phaseB executes — across the three identical k=30
reps:

| arm | n55 m2_cross | vs base | n55 pb_sum_s | n55 pb_max_s | n56 pb_max_s | global phaseB max |
|---|---|---|---|---|---|---|
| base (mean of 3) | 6.006e10 | — | 14.59 | 1.255 | 0.300 | 1.255 |
| shed30-r1 | 5.964e10 | **−0.8%** | 15.41 | 1.301 | 0.482 | 1.301 |
| shed30-r2 | 1.502e10 | **−75.0%** | 7.48 | 0.664 | **0.788** | **0.788** |
| shed30-r3 | 4.816e10 | **−19.9%** | 11.80 | 1.067 | 0.464 | 1.067 |

Same k, same victim, same destination, three runs: the sampled 30 pieces
carried 0.8%, 75%, and 20% of the victim's pair work respectively. **The
outcome tracks the m2 moved, not the count moved, and not the particles
moved** — the particle mass shifted was near-identical in all three reps
(1.336M / 1.194M / 1.397M particles). That is the design doc's own point,
measured again from the other side: *any model that reduces to particle count
cannot work here.*

A random 7.4% of the pieces sometimes catching 75% of the work is exactly what
the cost probe's concentration finding predicts (top 1% of pairs hold 60–79.5%
of the time). The work is concentrated in a handful of pieces; hitting them is
currently luck.

**Where the phase actually lands.** In all 13 arms the global `phaseB_s` max
equals the worst PE of either node 55 or node 56 — node 55 sets the phase in
12 of 13, and in shed30-r2, where the shed worked, **node 56 becomes the new
worst at 9.04 s** and sets it. That is precisely the predicted behaviour:
levelling #1 lands you at roughly #2 (base #2 = node 54 at ~9.6 s) and no
further.

Also worth recording: node 56 is a good destination *by luck* — it starts at
`pb_sum_s` 4.13 against the 1.57 median, so it has room, but it is not the
idlest process on the machine. And at k=30 it absorbs enough to become the
straggler itself, so the single-destination rule saturates at roughly
(#2 − destination's own load) ≈ 5 s of pb_sum. Beyond that, k cannot help;
a second destination is required.

## 6. The wall rows — the gain does not yet reach Iteration 0

| arm | Pre-traversal (ms) | Iteration 0 (ms) |
|---|---|---|
| probe-base | 3669.4 | 6412.6 |
| base-r1 | 3728.9 | 6425.9 |
| base-r2 | 3812.1 | 6490.6 |
| base-r3 | 3861.7 | 6561.8 |
| shed4-r1 | 3638.8 | 6343.8 |
| shed4-r2 | 3689.4 | 6447.9 |
| shed4-r3 | 3785.6 | 6509.9 |
| shed12-r1 | 3704.1 | 6580.3 |
| shed12-r2 | 3644.5 | 6503.1 |
| shed12-r3 | 3537.9 | 6385.3 |
| shed30-r1 | 3765.3 | 6835.6 |
| shed30-r2 | 3581.1 | 6584.4 |
| shed30-r3 | 3594.4 | 6443.8 |

Means: base 3768.0 / 6472.7; shed4 3704.6 / 6433.9; shed12 3628.8 / 6489.6;
shed30 3646.9 / 6621.3.

**Say this plainly: no arm improves Iteration 0.** shed30 is +2.3% on the
mean. Two reasons, both arithmetic rather than mysterious:

1. phaseB max is 1.25 s of a 6.47 s Iteration 0 — 19%. Even a perfect phaseB
   fix caps the Iteration-0 gain at ~7%.
2. The migration is charged to the same iteration: 274 ms mean at k=30 against
   a 202 ms mean phaseB saving. At k=30 the current implementation spends more
   on moving than it recovers, on average.

Even shed30-r2, the arm that saved 0.447 s of phaseB, paid 0.321 s to migrate
and finished Iteration 0 at 6584 ms against a 6473 ms base mean. Iteration 0
ranges 6344–6836 across the job (7.8%), so no arm's effect is separable there
anyway.

`phase1_stages` (reset / register / phaseA / phaseB / merge / relabel) confirms
the effect is confined to the phaseB stage — phaseA is flat at 2.03–2.12 on
every arm, merge and relabel are flat:

| arm | phaseA | phaseB | merge | relabel |
|---|---|---|---|---|
| base (mean of 3) | 2.047 | 1.292 | 0.013 | 0.118 |
| shed4 (mean) | 2.075 | 1.288 | 0.018 | 0.118 |
| shed12 (mean) | 2.071 | 1.187 | 0.013 | 0.119 |
| shed30 (mean) | 2.073 | 1.103 | 0.013 | 0.118 |
| shed30-r2 | 2.043 | **0.822** | 0.013 | 0.117 |

## 7. What the spec expected vs what I found

| expectation | finding |
|---|---|
| victim is node 55 | **Confirmed**, measured not assumed: pb_sum_s 14.61, #2 node 54 at 9.56, gap 1.528x, ceiling −34.5% |
| shed4 does little | **Confirmed**, −0.3% and inside the noise |
| shed12–30 cut the victim's phaseB max toward #2 and no further | **Confirmed where it fires.** shed30-r2 took the max to 0.788 and node 56 became the new worst at 9.04, next to base #2 = 9.6. But it fires only 1 rep in 3 at k=30 |
| the open question is the phase-3 cost | **Answered, and favourably.** 10.7% of machine-wide phase-1 pair work converted, phase 3 stayed inside its base range |
| — (not anticipated) | **The forced arm samples pieces at random, not by load.** That, not noise, is the shed30 spread: the sampled 30 carried 0.8% / 75% / 20% of the victim's m2_cross in three identical reps |
| — (not anticipated) | **Sending all k pieces to ONE destination is protective**, not merely crude: ~83% of the displaced work stayed in phase 1 because the moved pieces keep each other's pairs. It also saturates — at k=30 the destination becomes the new straggler |
| — (not anticipated) | **The migration is charged to Iteration 0** and at k=30 costs (274 ms) more than the phaseB saving it buys (202 ms). No arm improves Iteration 0 |

## 8. What this licenses next

The measurement that was blocking model-driven selection is now done, and it
came out in favour of building it:

1. **The prize is real and reachable.** 0.788 s against a 1.254 s base and a
   0.25 s granularity floor, with the count exact.
2. **The locality conversion is not the obstacle.** It was the stated risk
   since the v2 design; at k=30 it is below phase-3 noise.
3. **The obstacle is selection.** The outcome is a clean function of the m2
   moved, and the current rule moves a random sample. relay5 already showed
   the m2_cross model ranks the true worst process #1 in 3/3 reps; what this
   job shows is that the same kind of ranking is now needed one level down,
   **over the pieces within the victim**, to choose which 30 of 403 to move.
4. **Two implementation notes follow directly from the data**, and both are
   small:
   - Cap or split the destination. At k=30 node 56 becomes the new worst, so
     one destination saturates at ~5 s of absorbed pb_sum. Two or three
     destinations, chosen by their own measured load, would extend the range —
     at some cost to the 48% in-phase-1 preservation measured in §4, which
     should be re-measured rather than assumed to hold.
   - The migration cost (274 ms at k=30) is currently the tax that eats the
     win at the Iteration-0 level. It scales with pieces moved, so a ranker
     that gets 75% of the work into 8–10 pieces instead of 30 would cut that
     cost by ~3x AND make the result deterministic.

Anti-recommendation, stated plainly: **do not adopt shedding on these numbers
as an end-to-end speedup.** It moves phaseB's tail, which is what it was
designed to do, and it does not yet move Iteration 0.

## 9. Provenance and artefacts

- job 5286357, `/lustre/orion/csc710/scratch/lvkale/s3ab/5286357/`
- tree `4f49227`, clean (only the `utility` submodule shows in the divergence
  guard). `patches/0009` (the cross-node protocol work) was REVERTED off the
  tree before this build, so these numbers are pristine-branch numbers. That
  patch is unchanged in `~/software/patches/`.
- 13 2B logs + 2 10k logs in the job directory
- all 128 `load_model:` lines from base-r1 and shed30-r1:
  `reports/load_model-shed-base-r1.txt`, `reports/load_model-shed-shed30-r1.txt`
- sbatch: `~/software/sbatch/shed-2b-16n.sbatch`
- analysis: `~/software/scripts/shedtable.sh`

---

# ADDENDUM (Kale, 2026-08-16 17:2x): phase-1 accounting, migration price,
# and what is worth tracing

Three questions: read the impact on **phase 1** rather than on phaseB, since
phaseA and phaseB are not barrier-separated; confirm the migration price;
and how to migrate less while keeping the benefit.

## A1. The phase-1 wall, paired within each rep

`FOF3STAT time_s: phase1` is the barrier-to-barrier phase-1 total and is the
honest basis. Paired against the base arm of the SAME rep:

| rep | base phase1 | shed4 | shed12 | shed30 |
|---|---|---|---|---|
| r1 | 3.328 | 3.249 (**−0.079**) | 3.316 (**−0.012**) | 3.355 (**+0.027**) |
| r2 | 3.426 | 3.302 (**−0.124**) | 3.249 (**−0.177**) | 3.193 (**−0.233**) |
| r3 | 3.477 | 3.399 (**−0.078**) | 3.151 (**−0.326**) | 3.208 (**−0.269**) |
| **paired mean** | — | **−94 ms** | **−172 ms** | **−158 ms** |

Base phase1 spread across the three reps is 149 ms (3.328–3.477), so the
shed12/shed30 gains are of order one spread and the shed4 gain is smaller than
one spread but the most consistent (−79/−124/−78, all three negative).

**The phaseB result is discounted roughly 2:1 on its way to the phase-1 wall,
exactly as you predicted.** shed30-r2 cut the phaseB stage by 447 ms
(1.269 → 0.822 max-over-processes) but the phase-1 wall by only 233 ms paired.
With no barrier between phaseA and phaseB, half of the freed phaseB tail was
hiding under other processes' phaseA and buys nothing. **Any future shedding
result should be quoted on `time_s: phase1`, not on `balance: phaseB_s max`.**
The −37.2% headline in §2 is a real measurement of the tail, but the
corresponding phase-1 number is −6.8%.

## A2. The price — 274 ms is right, and the budget closes

`Targeted shedding:` mean over 3 reps: **41 ms (k=4), 141 ms (k=12), 274 ms
(k=30)**. It sits between decomposition and the tree build, so it is charged to
Iteration 0 but is NOT inside phase 1 — no double counting.

| arm | phase-1 gain | migration price | predicted Iter0 delta | **observed** Iter0 delta |
|---|---|---|---|---|
| shed4 | −94 ms | +41 ms | −53 ms | **−59 ms** |
| shed12 | −172 ms | +141 ms | −31 ms | **−3 ms** |
| shed30 | −158 ms | +274 ms | **+116 ms** | **+129 ms** |

Predicted = gain + price; observed = paired Iteration-0 difference. The three
rows agree to within 30 ms, so the budget is closed and there is no missing
term — the tree build, pre-traversal and traversal rows are all flat
(tree build 988–1003 ms on every one of the 13 arms).

**So the k sweep inverts once the price is paid.** shed4 and shed12 are net
positive; **shed30 is net negative** — its extra gross gain over shed12 is
nil (−158 vs −172 ms) while it costs 133 ms more to move. The useful k on
these numbers is **around 4–12, not 30**, which is the opposite of what the
phaseB-max column alone suggests.

**One-off cost, recurring gain.** `taken` is a function-static that never
resets, so iterations 1+ migrate nothing: the price is paid once and the gain
repeats every iteration. These runs are a single iteration, so everything is
charged to iteration 0. On a 10-iteration host run, shed30 would be net
positive from iteration 2 onward. Worth stating in any writeup, and worth one
`num_iterations 2` arm to verify that the second iteration really is free.

## A3. Reducing the migration cost

Ordered by (value / effort), and the first one is free.

**1. Measure the fixed cost first — a `shed0` arm, zero code change.**
`TreePiece::shedDecide` gates on `k > 0` but `Driver::run` gates on
`getenv("FOF_SHED_NODE")` alone. So **`FOF_SHED_NODE=55 FOF_SHED_COUNT=0` runs
the entire machinery — broadcast to all 63946 elements, concat reduction,
`shedPlan`, `CkWaitQD` — and migrates nothing.** It will print
`SHED plan: 0 of 63946 elements migrating` and a `Targeted shedding:` time that
is PURE FIXED COST. Subtract it from the k=4/12/30 numbers and the per-piece
cost falls out. One 17 s arm answers what a trace would otherwise be needed
for, and it should be run before anything is traced.
The existing data hints the fixed part is large: k=4 was as low as 19 ms but
k=30 as high as 353 ms, and the run-to-run spread at fixed k is 2–3x, which is
not what a data-volume-dominated cost looks like.

**2. Collapse the reduction payload — one line.** Every one of the 63946
elements contributes 8 bytes to a `CkReduction::concat`, so **500 KB is
gathered through the reduction tree on every shed run to carry 240 useful bytes
at k=30**. Non-movers can contribute zero length:

```cpp
int pair[2] = {this->thisIndex, shed_dest_};
this->contribute(shed_dest_ >= 0 ? sizeof(pair) : 0, pair,
                 CkReduction::concat, cb);
```

Every element still contributes, so the reduction still proves the broadcast
was delivered everywhere — the property the fix at fb43f06 depends on is
untouched. `shedPlan` then sees `n` = movers rather than 63946, so its printf
and loop want the obvious adjustment. If step 1 says the fixed cost dominates,
this is the first thing to try.

**3. Rank the pieces — the one change that improves both terms at once.**
Cost scales with PARTICLES moved; benefit scales with m2 moved. So the
selection metric is not m2 but **m2_cross(piece) / n(piece) — work moved per
byte moved.** §5 measured the two coming apart completely: three k=30 reps
moved near-identical particle mass (1.34M / 1.19M / 1.40M) for 0.8% / 75% /
20% of the victim's pair work. A ranked rule that reaches shed30-r2's 75% with
8–10 pieces instead of 30 cuts the per-piece part of the price ~3x, moves the
useful k down into the 4–12 band that A2 says is the profitable one, and
removes the lottery. Everything needed is already computed: `pieceM2` and the
per-piece (n, box) are in hand at `FoFPhase1.h:1616`.

**4. Split the destination — only after 1–3.** One destination both saturates
(§5: node 56 becomes the new worst at k=30) and serialises the receive side.
Two or three destinations chosen by measured load would extend the range, at
some cost to the 48% in-phase-1 preservation measured in §4. Note this trades
against 3: fewer, better-chosen pieces may make it unnecessary.

**5. Overlap the migration with the tree build — invasive, keep in reserve.**
The 274 ms is dead time enforced by a global `CkWaitQD`; only the moved pieces
and their destinations actually need to wait. A per-piece dependency instead of
a global barrier would hide most of it. Worth doing only if 1–3 leave a large
residue.

Not recommended for this purpose: the shadow-copy / data-plane variant from the
design doc. It saves the RETURN trip (labels only, ~12x), and we are not paying
a return trip — the forward cost, which is what A2 prices, is unchanged.

## A4. Is any configuration worth projections / sumDetail?

**Split the answer, because the two questions have opposite verdicts.**

**YES for the migration cost — and it is the cheap capture.** `+sumDetail`,
full machine, base vs shed30 (add shed4 for the k-scaling): 257 MB and 3585
files per run, already measured on 2026-08-16. It decomposes the 274 ms into
broadcast delivery / concat reduction / pup + element migration / QD drain,
which is precisely what decides between ideas 2, 4 and 5 above. Entry points to
request in ONE `sumd_tool.py --ep` invocation: `shedDecide`, `migrateTo`,
`shedPlan`, the reduction path, and the array element pup/unpack.
**But run the free `shed0` arm (A3.1) first** — if it shows the cost is
overwhelmingly fixed, idea 2 is a one-line fix and the trace may be unnecessary.

**NO, not yet, for the phaseB/phase-1 win — and the reason is stronger than the
usual perturbation caveat.** The selection rule IS the delivery order
(`taken.fetch_add` on first-come order across the process's 14 PEs). **Tracing
changes the delivery order, so a traced shed30 run moves DIFFERENT pieces than
the untraced one.** It is not a perturbed version of the same experiment, it is
a different experiment — and with a 0.8% / 75% / 20% spread on the outcome, the
traced run is a fresh draw from that lottery. This is the same failure that
wasted job 5282141, where the traced binary sent node 55's work to a different
block than the untraced one, in a sharper form.
**Make the selection deterministic first (A3.3, or even just "lowest k
indices" as a stopgap), then tracing becomes meaningful.** After that, a
projections capture is worth it and can be a SUBSET, not all 1792 PEs: process
55 owns PEs 770–783 and process 56 owns PEs 784–797, so 28 PEs plus a control
block is ~75–150 MB against 2.6 GB for the full machine. The destination is
deterministic (`victim+1`), so unlike the cross-node case both ends are known
in advance and the subset can be chosen safely.

All-PE projections are not worth it for this question: the machine-wide effect
this would show is phase 3, and §4 established phase 3 is flat.

---

# ADDENDUM B: the migration price is NOT what I said it was
# Job 5286540, same binary `FoF3.2b.shed`, 25 arms, all exact

## B0. Why a concat reduction at all — the question, answered

`Driver::shedPlan` has to issue the migrations, because issuing them from
inside the decision broadcast is exactly what crashed (fb43f06: the source PE
is still walking its local element list). So the plan has to travel from the
elements to the Driver, and across a chare array the only gather is a
reduction. `concat` specifically, rather than `sum`/`max`, because the payload
is a variable-length LIST of records — one per candidate — not a scalar.

The same reduction does double duty as the barrier: it can only complete after
every element has been delivered the broadcast, which is the property that
makes migrating afterwards safe. That is why every element contributes, and it
is why the old code had all 63946 of them contribute 8 bytes each.

So the answer to "why concat" is: the Driver needs a list, and the reduction
that carries it is also the barrier. What was wrong was the DENOMINATOR, not
the mechanism — non-candidates had no reason to send bytes.

## B1. The fixed cost is 1.3 ms. My "collapse the payload" idea was wrong.

`FOF_SHED_COUNT=0` with `FOF_SHED_NODE` set runs the whole machinery and
migrates nothing. Measured, three reps:

```
SHED plan: 0 of 63946 elements migrating
Targeted shedding: 1.323 / 1.325 / 1.246 ms
```

**The broadcast to 63946 elements, the ~500 KB concat reduction, `shedPlan`
and `CkWaitQD` together cost 1.3 ms.** The payload was never the problem.
Collapsing 500 KB to 6 KB — item 2 of the previous addendum, which I put second
on the list — is worth about one millisecond. I was wrong to rank it there;
it is hygiene for larger machines, not a speedup. It is in patches/0010 only
because it falls out of moving the choice to the Driver at no extra cost.

The shed0 arms also confirm the machinery is otherwise inert: phase-1 wall
3.310/3.327/3.359 against a base of 3.194/3.341/3.264, i.e. inside the base
spread, and the victim keeps all 403 pieces and its full `m2_cross`.

## B2. And the per-piece cost does not behave like data movement either

`Targeted shedding:` over the sweep, 3 reps each:

| k | mean ms | reps |
|---|---|---|
| 0 | **1.3** | 1.32 / 1.33 / 1.25 |
| 1 | 21.9 | 8.9 / 39.8 / 16.9 |
| 2 | 30.0 | 15.4 / 37.0 / 37.7 |
| 4 | 63.7 | 24.5 / 57.9 / 108.8 |
| 8 | 64.7 | 71.3 / 59.3 / 63.6 |
| 16 | **212.2** | 184.2 / 262.8 / 189.6 |
| 30 | **103.5** | 93.8 / 106.9 / 109.7 |

**k=16 reproducibly costs about twice k=30**, in all three reps, in one
allocation, with the same binary. Moving fewer, smaller-in-total pieces costs
more. That cannot be a bytes-moved cost, so the linear "≈11 ms per piece" law
the first four rows suggest is not the mechanism.

Two further facts against a data-volume explanation:
- The same k=30 configuration cost 147–353 ms in job 5286357 and 94–110 ms
  here — a 3x difference between allocations, same binary, same k.
- The spread at fixed k is 2–4x (k=4: 24.5 / 57.9 / 108.8 ms).

**Working hypothesis, stated as a hypothesis:** the cost is dominated by
`CkWaitQD` convergence rather than by the migration traffic. Quiescence
detection has to observe the whole machine go quiet, and how long that takes
depends on how the migration messages interleave with everything else in
flight — which would be neither monotonic in k nor reproducible between
allocations. I have not proved this and the counters cannot.

**If that hypothesis holds, it changes the answer to "how do we migrate less".**
Reducing the number of migrations would not help much, and the lever becomes
replacing the global `CkWaitQD` with a targeted completion test — count the
migrations acknowledged and proceed, rather than waiting for machine-wide
quiescence. That is item 5 of the previous addendum promoted from reserve to
primary.

**The discriminating measurement is already running** (job 5286573): with
ranked selection, mode 0 and mode 2 move very different particle masses for the
SAME k. If `Targeted shedding:` tracks particles moved, the cost is data-bound;
if it is flat across modes at fixed k, it is QD-bound. That settles it without
a trace.

## B3. What the k sweep says about the benefit, on this allocation

| arm | phase1_s | phaseB max | n55 pieces | n55 m2_cross |
|---|---|---|---|---|
| base (mean of 3) | 3.266 | 1.223 | 403 | 6.014e10 |
| shed0 | 3.332 | 1.207 | 403 | 6.015e10 |
| shed1 | 3.283 | 1.205 | 402 | 5.973e10 |
| shed2 | 3.274 | 1.238 | 401 | 6.011e10 |
| shed4 | 3.237 | 1.169 | 399 | 5.672e10 |
| shed8 | 3.198 | 1.189 | 395 | 5.941e10 |
| shed16 | 3.211 | 1.170 | 387 | 6.002e10 |
| shed30 | 3.270 | 1.063 | 373 | 5.540e10 |

This allocation drew badly: the random selection moved almost no work at k=16
(`m2_cross` 6.002e10 against a base of 6.014e10 — 0.2%) and only 4–13% at
k=30, so there is no 75% event anywhere in these 25 arms. That is the lottery
again, and it is the reason the phase-1 column barely moves. It is also, on its
own, an argument for ranked selection: two independent 3-rep draws at k=30 gave
−0.8/−75/−19.9% and −13/−4/−7% of the victim's pair work.

---

# ADDENDUM C: RANKED selection works, and it is deterministic
# Job 5286573, `bin/FoF3.2b.shedrank` (4f49227 + patches/0010), 30 arms, all exact

## C1. Determinism — the property that was missing

`particles_moved`, three reps of every arm:

| arm | rep1 | rep2 | rep3 |
|---|---|---|---|
| r0k4 | 180517 | 180517 | 180517 |
| r1k4 | 191533 | 191533 | 191533 |
| r2k4 | 191533 | 191533 | 191533 |
| r3k4 | 46465 | 46465 | 46465 |
| r2k1 | 61055 | 61055 | 61055 |
| r2k2 | 109121 | 109121 | 109121 |
| r2k8 | 324683 | 324683 | 324683 |
| r1k12 | 720556 | 720556 | 720556 |
| r2k12 | 417119 | 417119 | 417119 |

Identical, every arm, every rep — and the resulting `m2_cross` agrees to 3
significant figures too (e.g. r2k4: 3.77367 / 3.77161 / 3.77331e10). **The
lottery is gone.** A traced run will now move the same pieces as an untraced
one, which is what makes any future projections capture legitimate.

## C2. Ranking finds the work — 4 pieces carry 37% of it

At k=4, all three deterministic rules against the blind control:

| mode | rule | particles moved | victim m2_cross drop |
|---|---|---|---|
| 0 | `-index` (low end of the SFC run) — CONTROL | 180517 | **−0.0 to −0.4%** |
| 1 | `n²/V^(4/3)` (work per piece) | 191533 | **−37.3%** |
| 2 | `n/V^(4/3)` (work per particle) | 191533 | **−37.3%** |
| 3 | `1/V^(2/3)` (sqrt term, per particle) | 46465 | −3.9% |

**Four pieces — 191533 particles, 1.2% of the victim's 15.5M — move 37.3% of
its cross-PE pair work.** The old random rule needed 30 pieces and 1.2–1.4M
particles to reach anywhere between 0.8% and 75%. Modes 1 and 2 select the
same four pieces at k=4; they diverge at larger k.

Mode 2's whole depth series, deterministic:

| k | particles | m2_cross drop | drop per Mparticle |
|---|---|---|---|
| 1 | 61055 | −16.2% | 265 |
| 2 | 109121 | −23.2% | 213 |
| 4 | 191533 | −37.3% | 195 |
| 8 | 324683 | −47.6% | 147 |
| 12 | 417119 | −56.1% | 135 |

**A CORRECTION TO THE DESIGN DOC.** The v2 sketch says "pieces at either END of
a run have most of their neighbours in the adjacent process already... Prefer
end pieces when their load is comparable." Mode 0 IS that rule, and it moved
180517 particles for **zero** work. Their load is not comparable — end pieces
are locality-cheap and load-empty, so the two criteria point in opposite
directions and locality must not be the tiebreak.

Mode 3 is also refuted: `1/V^(2/3)` selects the smallest-volume pieces
regardless of size and finds almost nothing (−3.9%). The sqrt compression that
helped ACROSS processes does not transfer to ranking WITHIN one.

## C3. The migration cost model, now that selection is deterministic

Regressing `Targeted shedding:` on particles moved over the nine ranked arms
(R² = 0.93):

```
cost ≈ 59 ms  +  142 ms per million particles moved      (if anything moves)
cost  =  1.3 ms                                          (if nothing moves)
```

So B2's hypothesis was HALF right. There is a step of ~59 ms the moment any
element migrates — consistent with quiescence/location settling, since it does
not depend on how much moves — and on top of it a genuine data term of 142 ms
per million particles (~700 MB/s, a plausible transfer rate). Both are real;
neither is the 500 KB reduction payload.

**The practical consequence is sharp: below ~400k particles the 59 ms step
dominates, so shaving k is nearly pointless.** Cutting the price means moving
fewer PARTICLES for the same work — which is exactly what mode 2 does, and at
k=12 it moves 42% fewer particles than mode 1 (417k vs 721k) for 56% vs 64% of
the work.

## C4. The full budget — and the first net-positive configuration

Means of 3 reps. "predicted" = phase-1 delta + migration cost.

| arm | phase1 | Δ phase1 | shed ms | predicted ΔIter0 | **observed ΔIter0** |
|---|---|---|---|---|---|
| base | 3.249 | — | — | — | 6341.3 ms |
| r0k4 | 3.287 | +38 | 66.0 | +104 | **+109** |
| r3k4 | 3.218 | −31 | 60.6 | +30 | **+32** |
| r2k1 | 3.501 | +252 | 75.6 | +328 | **+369** |
| r2k2 | 3.604 | +355 | 76.2 | +431 | **+429** |
| r1k4 | 3.449 | +200 | 92.5 | +293 | **+297** |
| r2k4 | 3.450 | +201 | 92.7 | +294 | **+345** |
| r2k8 | 3.249 | 0 | 102.5 | +103 | **+126** |
| r1k12 | 3.109 | −140 | 161.6 | +22 | **+20** |
| **r2k12** | **3.062** | **−187** | **118.7** | **−68** | **−49** |

The budget closes on every row. **`FOF_SHED_RANK=2 FOF_SHED_COUNT=12` is the
first configuration in this campaign that is net positive on Iteration 0:
−49 ms, reproducible across three reps, deterministic, and exact.** It is
modest — 0.8% of Iteration 0 — but it is a real win rather than a tail
measurement, and mode 2 beats mode 1 on every axis at k=12 (cost 119 vs 162 ms,
phase 1 3.062 vs 3.109, Iter0 6292 vs 6361), which is the per-particle metric
earning its keep.

## C5. THE ANOMALY: small k with concentrated work HURTS phase 1

This is the one thing here I cannot explain, and it is systematic rather than
noise. Phase-1 wall delta against base, by k, mode 2:

| k | 1 | 2 | 4 | 8 | 12 |
|---|---|---|---|---|---|
| Δ phase1 | **+252** | **+355** | **+201** | 0 | **−187** ms |

Monotone in k above k=2, consistent in sign across all three reps of every arm
(r2k4: 3.441 / 3.473 / 3.436, every one above the highest base rep 3.336;
r2k12: 3.031 / 2.977 / 3.179, all at or below the lowest base rep 3.165).

What makes it strange is that every counter we have moves the RIGHT way in
those same arms:

| arm | phaseA | phaseB stage | n55 pb_sum | n56 pb_sum | phaseB max |
|---|---|---|---|---|---|
| base | 2.016 | 1.263 | 14.18 | 3.68 | 1.189 |
| r2k2 | 2.017 | **1.132** | **11.38** | 4.59 | **1.121** |
| r2k4 | 2.017 | **1.173** | **12.51** | 6.33 | **1.091** |
| r2k8 | 2.024 | **1.068** | **11.23** | 7.62 | **1.043** |
| r2k12 | 2.034 | **1.160** | **12.85** | 7.78 | 1.137 |

phaseA is flat, the phaseB stage falls, the victim's load falls, the
destination stays below the victim, and the phaseB max improves — yet the
phase-1 WALL rises by 355 ms at k=2. The gap is therefore not in any stage's
own work; it is in the ALIGNMENT between processes, which is precisely the
quantity none of these counters measure. r0k4, which migrates 4 pieces but
moves no work, sits at +38 ms, so it is not "any migration hurts" — it is
specific to relocating a small amount of highly concentrated work.

**This is now the right thing to trace, and it is the first time a trace has
been justified here.** The prerequisite is satisfied: selection is
deterministic (C1), so a traced run moves the same pieces as the untraced one.
A projections capture over phase 1 for base / r2k2 / r2k12 on the victim's and
destination's PEs (770–783 and 784–797) plus a control block — ~28 PEs,
75–150 MB — should show directly what the machine is waiting on. `+sumDetail`
would NOT answer this one: the question is about ordering between processes,
not about where time goes inside an entry method.

Until it is explained, the honest recommendation is to use k=12 with mode 2 and
to treat small-k shedding as measured-harmful rather than merely unhelpful.
