# The pure-LCI idle-stall reproducer, run on Frontier

Jobs **5303876** (2-node smoke), **5303887** (16-node sweep, 16 arms),
**5303902** (long-silence arms), 2026-08-18. Companion to
`reports/stall-attrib.md` (relay13) and `reports/relay14.txt`.

## 0. Why this run exists, and the correction that prompted it

Relay13 established that switching the keep-alive ring off changes nothing
in FoF, even though FoF still goes 2.4 s silent per process and then
resumes against 48-108 peer processes per thread. It offered two candidate
explanations. **One of them was wrong, and the notes already said so:**

* Job 19624352 ran the reproducer at **1 device/rank and at 15
  devices/rank**, and both stalled (K=8: 0.070% / 0.210% over 10 ms;
  K=31: 0.620% / 0.430%), with the explicit conclusion "per-thread devices
  are not a workaround". Device fan-out was never a live candidate. That
  is withdrawn in `stall-attrib.md` and `relay13.txt`.

Reading those tables again surfaced the thing that reframes the whole
question. **Every measurement of this bug -- jobs 19608513 through
19661625 -- was made on Anvil, over Mellanox InfiniBand HDR.**
`charm-notes/machines/anvil.md`: "Network: Mellanox InfiniBand HDR, one HCA
per node". `fof/FoF.C:13` opens the workaround's comment with "On
reconverse/LCI over InfiniBand". And the bug was localised "below the
progress API ... (libfabric/IBV provider)".

Frontier is Slingshot/CXI through libfabric's cxi provider. So the prior
question is not which remedy holds the stall off here. It is whether the
bug is present on this fabric at all.

## 1. The harness

`lci_multipeer.cpp` did not exist on Frontier -- it was written on Anvil
and never copied. It is rebuilt here from the description in
`charm_best_practices.md` (2026-08-02, job 19624352), matching it point by
point, and extended with two arms Anvil never ran.

* **Pure LCI.** `nm -C` reports **0 converse/Cmi symbols**. It links the
  same `liblci.so` the production FoF binary links
  (`~/software/charm/lib/liblci.so`, LCI 8.0.1), and the same
  `libfabric.so.1`.
* **The app's shape, not the benchmark's.** 128 ranks x 14 threads x 7
  devices, with reconverse's exact mapping
  `device_id = thread_id / ceil(nthreads/ndevices)`, so 2 threads share a
  device exactly as `+ppn 14 +lci_ndevices 7` produces.
* **The measurement.** Rank 0 thread 0 pings round-robin over K distinct
  remote ranks, **one message outstanding**, 8-byte active messages; every
  other thread on every rank spins on `lci::progress()` for the whole run,
  which is what a reconverse PE does in its idle loop.
* **The silence.** After a barrier, every rank goes quiet for
  `--quiet` seconds before rank 0 starts measuring. There is deliberately
  **no barrier after the silence** -- a barrier is itself traffic and
  would destroy the condition under test.
* **New arm 1, `--warm traffic`:** a heavy all-to-all phase before the
  silence, then FoF's own 2.4 s silence. This is the warm-connections
  hypothesis from relay13 item 16(a).
* **New arm 2, `--ambient MS`:** a keep-alive ring, one 8-byte message per
  rank to its ring neighbour every MS ms, from one thread -- the
  `FOF_KEEPALIVE` analogue, as the workaround control.

Each arm is a **separate `srun`**. Running them in one process would let an
earlier arm's traffic warm the connections for a later one, which is the
variable under test.

## 2. Result: the bug is not present on this stack

Job 5303887, 10,000 round trips per arm, two interleaved reps.
**Anvil reference in the last column.**

| arm | mean | p50 | p90 | p99 | max | <=8 us | >1 ms | >10 ms | Anvil >10 ms |
|---|---|---|---|---|---|---|---|---|---|
| k1-r1  | 6.58 us | 5.63 | 6.32 | 34.86 | 78.4 us | 95.1% | 0 | **0** | 0.000% |
| k1-r2  | 6.55 | 5.61 | 6.42 | 34.73 | 294.0 | 94.6% | 0 | **0** | |
| k8-r1  | 6.46 | 5.77 | 6.55 | 34.62 | 286.7 | 96.7% | 0 | **0** | **0.070%** |
| k8-r2  | 6.53 | 5.89 | 6.64 | 34.69 | 97.0 | 96.7% | 0 | **0** | |
| k16-r1 | 6.67 | 6.00 | 6.89 | 32.95 | 80.1 | 95.6% | 0 | **0** | |
| k16-r2 | 6.77 | 6.00 | 7.18 | 34.63 | 260.3 | 93.6% | 0 | **0** | |
| k31-r1 | 7.08 | 6.27 | 7.29 | 35.05 | 275.6 | 93.8% | 0 | **0** | **0.620%** |
| k31-r2 | 7.05 | 6.37 | 7.32 | 32.58 | 81.1 | 93.4% | 0 | **0** | |
| k64-r1 | 7.51 | 6.63 | 8.20 | 35.67 | 83.5 | 89.1% | 0 | **0** | not run |
| k64-r2 | 7.30 | 6.45 | 7.90 | 34.82 | 79.9 | 90.5% | 0 | **0** | |
| k16w-r1 (traffic warm) | 8.50 | 6.44 | 7.36 | 43.21 | 562.3 | 94.6% | 0 | **0** | not run |
| k16w-r2 | 7.87 | 6.42 | 7.37 | 36.25 | 519.0 | 94.7% | 0 | **0** | |
| k64w-r1 (traffic warm) | 10.50 | 7.07 | 8.16 | 45.94 | 580.1 | 88.2% | 0 | **0** | not run |
| k64w-r2 | 9.68 | 7.14 | 8.32 | 41.54 | 577.9 | 86.3% | 0 | **0** | |
| k16ring-r1 (100 ms ring) | 6.62 | 5.93 | 6.81 | **23.09** | 81.6 | 95.6% | 0 | **0** | 0.000% |
| k16ring-r2 | 6.61 | 5.94 | 6.99 | **23.27** | 82.1 | 94.0% | 0 | **0** | |

**160,000 samples. Not one sample over 1 ms, in any arm.** The worst single
round trip in the entire job is 580 us. On Anvil, K=31 put 62 samples per
10,000 over **10 ms**.

How strong the null is: with 0 events in 20,000 trials (2 reps), the 95%
upper bound on the stall rate is 3/20,000 = **0.015%**.

| K | Anvil rate >10 ms | Frontier 95% upper bound | ratio |
|---|---|---|---|
| 8 | 0.070% | < 0.015% | at least **4.7x** below |
| 31 | 0.620% | < 0.015% | at least **41x** below |

And the healthy-mode statistics differ by more than the tail. Anvil's
degraded K=16 control had a **89.1 us mean**; its recovered state was
24-27 us. Frontier's mean is **6.5-7.5 us** with 86-97% of samples in the
2-8 us mode at every K. This fabric is not in the degraded regime, and
there is no regime to recover from.

## 3. What the K dependence does look like here

There is a peer-count effect on this stack, and it is real but tiny:
mean 6.58 us at K=1 rising monotonically to 7.51 us at K=64, and the
fraction in the fast mode falling 95.1% -> 89.1%. That is a **14% mean
increase across a 64x peer-count increase**, entirely inside the
microsecond band. On Anvil the same axis moved the mean by 3.5x and put
0.6% of samples past 10 ms.

## 4. The two new arms

**Warm connections (relay13 item 16a) is not the mechanism, and the
question it was raised to answer has dissolved.** Heavy all-to-all traffic
before the silence makes the mean *worse*, not better (8.5-10.5 us against
6.7-7.5 us for the same K after a quiet warm-up; max 520-580 us against
80-280 us) -- residual traffic still draining while the measurement runs.
Since neither arm stalls, there is nothing for warm connections to
suppress. The hypothesis is neither confirmed nor needed.

**The keep-alive ring does something measurable here, and it is not what
it was built for.** With the 100 ms ring on, p99 falls from 32.95/34.63 us
to **23.09/23.27 us**, consistently in both reps -- about 12 us off the
tail. The mean does not move (6.62 vs 6.67 us). So on Slingshot the ring
buys roughly 12 microseconds of p99 on a microsecond-scale operation. It
is not removing 10-25 ms stalls, because there are none to remove.

## 5. What this settles

1. **The LCI idle-stall does not reproduce on Frontier's Slingshot/CXI
   stack**, at any peer count from 1 to 64, after silences of 5 s (and see
   section 6 for 15/30/60 s), with or without a keep-alive ring.
2. **So the relay13 question has a different answer than either candidate
   on the table.** Neither the ring nor the polling configuration deserves
   the credit for FoF's clean phase boundaries here. The credit belongs to
   the fabric and provider -- Slingshot/CXI, libfabric, LCI 8.0.1 -- and
   the bug is an InfiniBand-path behaviour that this machine's path does
   not have.
3. **The keep-alive ring is dead weight on Frontier.** It costs 160
   messages/s job-wide, it is invisible in FoF's timings (relay13: -12 ms,
   inside noise), and its measured benefit here is 12 us of p99 in a
   microbenchmark. Keep it if FoF is ever run on Anvil or another
   InfiniBand machine, where it is load-bearing; it does nothing on this
   one.
4. **`fof/FoF.C:13-33` should say so.** The comment currently presents the
   ring as the fix for FoF's uf2-bracket stalls without naming the fabric
   it was measured on. One sentence -- that the stall is an InfiniBand-path
   behaviour, not reproducible on Slingshot/CXI as of 2026-08-18, jobs
   5303887/5303902 -- would stop the next person inheriting the wrong model.
   Not written: that is a code change and none was asked for.

## 6. Long-silence arms

Job 5303902. Anvil's onset needed about 1 s of quiet, and the sweep above
used Anvil's 5 s. If the cxi provider had a slower-onset version of the same
behaviour, a 5 s window would miss it. It does not:

| arm | silence | mean | p99 | max | <=8 us | >1 ms | >10 ms |
|---|---|---|---|---|---|---|---|
| k16-q15 | 15 s | 6.94 us | 34.96 | 103.0 us | 94.3% | 0 | **0** |
| k16-q30 | 30 s | 6.96 | 24.96 | 83.4 | 93.8% | 0 | **0** |
| k16-q60 | 60 s | 6.93 | 27.36 | 290.6 | 94.8% | 0 | **0** |
| k64-q15 | 15 s | 7.39 | 40.26 | 265.7 | 90.9% | 0 | **0** |
| k64-q30 | 30 s | 7.41 | 36.48 | 273.7 | 89.8% | 0 | **0** |
| k64-q60 | 60 s | 7.41 | 40.01 | 144.1 | 90.7% | 0 | **0** |

The mean is flat to two decimal places across a **12x range of idle time**
(6.93-6.96 us at K=16, 7.39-7.41 us at K=64). Idle duration has no effect
on this stack, which is the cleanest possible statement of the negative
result: the variable the whole trigger model is built on does nothing here.

**Running total across both jobs: 220,000 round trips, 0 over 1 ms.**

## 7. Caveats, stated plainly

* **This is a rewritten harness, not the Anvil binary.** `lci_multipeer.cpp`
  was never copied to Frontier, so a null result could in principle be a
  harness that fails to exercise the bug. Three things argue it is faithful:
  it matches the documented structure point by point (one thread, one
  message outstanding, round-robin over K peers, all other threads spinning
  on progress, silence before measurement); it reproduces the healthy
  behaviour the notes describe (a 2-8 us mode holding 86-97% of samples);
  and it shows the same qualitative K dependence, just three orders of
  magnitude smaller. What would settle it beyond argument is running this
  same file on Anvil and recovering the 0.62% at K=31. That is one small
  job on a machine I do not have in this session.
* **Scale differs from Anvil.** Anvil ran 32 ranks x 15 threads; this runs
  128 ranks x 14 threads x 7 devices, chosen to mirror FoF's own shape.
  Anvil showed the stall at both 1 and 15 devices per rank, so 7 is not a
  special case, and 128 ranks is a larger fan-out than the 32 that stalled.
* **libfabric moved.** BUILDS.md records the charm build against libfabric
  1.20.1; the stack now resolves **2.3.1** at run time
  (`/opt/cray/libfabric/2.3.1/lib64/libfabric.so.1`), with 1.15.0.0,
  1.15.2.0 and 1.20.1 also installed. Both this reproducer and the FoF
  binary load 2.3.1. Nothing here depends on which of them is "the" build
  version, but a provider-level behaviour is exactly the kind of thing a
  provider upgrade changes, and the note in BUILDS.md is now stale.
* **This says nothing about Anvil.** The ring remains load-bearing there on
  the evidence of jobs 19624352 and 19661625, which are not in question.

## 8. Files

```
lci-repro/lci_multipeer.cpp          the reproducer (pure LCI, 0 converse symbols)
lci-repro/build.sh                   builds it against the production charm's LCI
sbatch/lci-repro-smoke.sbatch        2-node bootstrap gate
sbatch/lci-repro-sweep-16n.sbatch    the 16-arm sweep (job 5303887)
sbatch/lci-repro-longquiet-16n.sbatch  the 15/30/60 s silences (job 5303902)
reports/lci-repro-frontier.md        this report
reports/relay14.txt                  the relay
```

Raw logs: `/lustre/orion/csc710/scratch/lvkale/s3ab/5303887` and `/5303902`
(and `/5303876` for the smoke).
