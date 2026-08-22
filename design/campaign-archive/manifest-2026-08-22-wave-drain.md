# uploads/ — overnight, 2026-08-22

Eleven files. Two rounds: the attribution ladder you asked for (relay74) and
then the autonomous exploration of the union-find drain (relay75/76).

## START HERE: relay75.txt — the drain, and the wave's real problem

**Your 300 ms is 334 ms, and it survives the prefix removal.** Measured with
entry-method attribution across the whole run, not from the uf2 bracket
(which by construction cannot see it — your correction was the key that
unlocked this):

    stack / arm                 drain   union-find    util    active PEs
    relay65 cpu-best (old)      334 ms   1053.7 PE-ms  0.93%     37.3
    relay75 cpu-off (current)   348 ms   1196.0        0.94%     35.3
    relay75 gpu-off (current)   274 ms    603.3        0.89%     32.6

**The decisive number is the ratio, not the 334 ms.** Total union-find CPU
across the entire run is 4603 PE-ms — 5.1 ms per PE. The drain is ~1200
PE-ms stretched over 348 ms of wall, a factor of ~100. It is a pointer-
chasing dependence chain, not a load problem. Rebalancing cannot touch it;
shortening chains is the only lever — so the wave is aimed at the right
structure.

## The wave: three findings, one of them a correction of mine

**1. It makes the drain LONGER.** 348 ms → 409 ms with `FOF_WAVE=1`, and the
wall agrees (+57.4 ms over two reps).

**2. I over-read the banner, and it matters.** I said it "found almost
nothing — element 0 rewrote 256 parents". Wrong: in `unionFindLib.C`,
`rewrote` counts only element 0 AND only vertices whose parent is on the
same element; every remote parent goes out through `wave_need_root_batch`
uncounted. Measured from the trace the wave does a full pass —
**414,543 calls, 1.02 s of CPU across 122 PEs**. It is not idle. It costs
1022 PE-ms and returns nothing.

**3. It is not just mistimed — and this is the result I did not expect.**
The wave fires 294 ms into a 409 ms drain (72% through, 100% of its CPU
inside the drain window). So I tested the position hypothesis with no code
change: `-E 0` moves the entire cascade behind the barrier where the wave
fires, deepening `uf2` from 0.042 s to 0.348 s.

    E16-off  4597.6 ms   uf2 0.042      E0-off  4701.7 ms   uf2 0.348
    E16-w1   4655.0      uf2 0.090      E0-w1   4730.9      uf2 0.385
      wave  +57.4 ms                      wave  +29.2 ms
    (streaming itself is worth -104.1 ms; hedge at -E 0 costs +107.7 ms)

**With an eight-fold deeper bracket sitting directly in front of it, the wave
still costs time and still rewrites the same ~259 parents.** Moving the work
to the wave did not give the wave more to do.

The explanation that fits both: the barrier is after edges are *submitted*
but before the QD *processes* them. At `-E 16` the cascade already ran during
the walk (forest settled and shallow); at `-E 0` it has not run yet (forest
trivial). **Either way the wave sees a shallow forest — there is no moment at
that barrier when the forest is both deep and stable.**

## Where that leaves it

The target is worth having: 348 ms of 4700 (7.4%) on CPU, 274 ms of 2670
(10.3%) on GPU, at under 1% utilisation. But v1 cannot reach it by
retiming — it would have to fire *during* the cascade, which is exactly
where there is no barrier and why v1 chose the one it did. **Keep the
default OFF**; both modes are correct (every arm EXACT, components line
bitwise identical) so the knob costs nothing meanwhile.

**One chain or many? Answered — many, and it needed no job.** Reading all 896
Projections logs: **128 PEs (14% of the machine) are each engaged for ~310-348
ms of the 348 ms drain, all still working at the end**, each ~97% idle waiting
on round trips. So there is no single critical chain to target — a *global*
mechanism is the right shape after all, which is what the wave is. What it
needs is a trigger that fires while those 128 cascades are still deep.

(I had first read this from only the first 96 PE logs and concluded the
opposite — "one PE engaged 305 of 334 ms". The heavy participants are the
high-numbered PEs, which that sample missed. Corrected in relay75.txt §2.)

## relay74.txt — the attribution ladder (asked for, answered)

The histogram row's gain is the **sort+scan pair path**, not the prefix
removal — the prefix removal has the wrong sign on that row (+0.0325 s).
The entire −6.4% relay72 reported is the sort path. relay72 and relay73
carry correction headers.

## Files

    relay75.txt                     the drain and the wave  <- START HERE
    relay74.txt                     the attribution ladder
    relay72.txt, relay73.txt        with correction headers
    relay75-ufattrib.py             entry-method attribution + drain finder
    relay75-chain.py                Projections burst/wait structure
    relay74/75/76 sbatch, build-ladder.sh

Traces on Frontier (1.4 GB each, 896 PEs, Projections + sumDetail):
    /lustre/orion/csc710/scratch/lvkale/s3ab/5324831/traces/{cpu-off,cpu-wave1,gpu-off}

Nothing pushed. paratreet2 e2a877e, unionfind 2ad91a8, both clean.
