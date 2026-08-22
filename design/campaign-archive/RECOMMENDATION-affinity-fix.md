# What to run

Written 2026-08-20; rewritten 2026-08-21 13:55; **rewritten 2026-08-22 12:45
against the current stack.** Self-contained: you do not need any of the
investigation to apply this.

**The filename is narrower than the contents.** This started as the write-up of
one fix and is now the whole recommended configuration for both arms. The name
is kept so existing links and scp lists still work.

Everything below was measured on Frontier at 2B particles, 16 nodes, and every
arm produced EXACT results (424897832 components, max_size 185317566). Reps and
job numbers are given for every figure. The evidence trail is in
`~/software/reports/`; `RESUME.md` is the map. The previous version of this
file is kept at `reports/RECOMMENDATION-affinity-fix-2026-08-21.md.bak` — its
absolute numbers are all superseded, its reasoning is not.

## The short version

There are **four** things to get right. They are independent and they multiply.

    1. build the runtime --with-production and do NOT link tracing
    2. use the right -l for the arm:   GPU 128,  CPU-only 32
    3. GPU only: use the affinity fix at ppn 7
    4. set FOF_UF_SIZES=0

    GPU, all four right        2548 - 2606 ms   (4 reps across two jobs)
    CPU-only, 1, 2 and 4       4465 - 4580 ms   (7 reps across four jobs)

Item 1 is worth −17.8% and item 3 about −23% on the GPU arm. Item 2 is worth
−32% on the CPU-only arm and nothing on the GPU arm, where the inherited value
was already right. Item 4 is worth about −2.4% on CPU; **on GPU it is
directionally right but not established** — see section 4.

**Those headlines are RANGES ACROSS JOBS on purpose.** The same configuration
has landed at 4464.8, 4467.2, 4541.0 and 4579.5 ms on the CPU arm in four
separate allocations. That 2.6% between-job spread is as large as some of the
effects being measured, so **never compare two walls from different jobs**.
Every A/B below is a within-job pairing, which is the only kind that means
anything at this scale.

**These headlines are lower than the previous version's 2825–2882 / 4785–4812
for two reasons**, and only one of them is item 4. The prefix removal and the
sort-scan pair path landed upstream in between (relay72/73/74), which by itself
moved the GPU arm to about 2630 ms. Do not attribute the whole change to
anything in this document.

## The stack these numbers were measured on

    paratreet2  main    7263ff1
    unionfind   master  db73766
    binaries    merged/FoF3.gpu-prod-v79   d7635c30affc4be14c0da39334e72468
                merged/FoF3.cpu-prod-v79   6ccf74a0fb65408063cd667efb178a06
    built by    merged/build-v79.sh   (clean trees, full scrub, gated)

`build-v79.sh` builds both production binaries and the traced twins, checks the
refs, scrubs every generated `.decl.h`/`.def.h`, and gates on the regenerated
header and on the binary's own strings. Use it rather than building by hand.

## 1. The build. Not optional, and easy to get wrong.

Charm must be configured `--with-production`, and the application must **not**
be linked `PROJECTIONS=1`.

Without `--with-production`, charm configures `CMAKE_BUILD_TYPE=Debug` with
`-g` and **no optimization at all** for charm and reconverse. That is not
"error checking on". Worth 15.6%.

Linking `-tracemode projections` compiles trace hooks into the message path
even when tracing is never enabled at run time. phaseA is fine-grained and
message-heavy, so this is not free. Worth 7.7%.

Together they were 2117 ms of the CPU iteration, and −17.8% on the GPU ppn-7
fixed arm (3504.9 → 2881.9 ms, relay49 job 5320452).

**Build a separate traced binary if you want traces.** A `--with-production`
charm does not build `libtrace-projections.a` at all, so a production arm
cannot be linked `PROJECTIONS=1` even by accident — you get
`charmc: No such tracemode projections`. That error is a feature.

**How to tell, from the binary alone:**

    nm -C <binary> | grep -ci TraceProjections     # must be 0

An environment check cannot catch this. Gate on the artefact.

## 2. The leaf size, `-l`. The two arms want different values.

**The FoF3 default is now 32** — paratreet2 `005b76f` (2026-08-21) moved it
from 12, landing the measured CPU optimum. That means **a binary built before
that commit and one built after it run different problems from the same
script**, so the printed value is the only safe source of truth.

Optima, measured at ppn 7 on production binaries (relay62 job 5321567, relay63
job 5321648). These absolute walls predate the prefix/sort landing and item 4,
so read the SHAPE, not the digits:

    leaf   GPU        CPU-only
      12   +27.7%     +12.1%
      32   + 8.8%     BEST
     128   BEST       +48.0%
     192   + 8.3%       --
     384   +52.8%       --

Both curves have a genuine interior minimum, so both values are real optima and
not the edge of a sweep. **GPU: `-l 128`. CPU-only: `-l 32`.** The CPU curve is
shallow between 24 and 48 (under 3% across that span); the GPU curve is sharper.

**Check what actually ran**, in both logs, before comparing any two numbers:

    Maximum number of particles per leaf: <n>

## 3. The affinity fix. GPU arm only.

**This applies to the GPU build only.** The CPU-only binary makes no HIP calls
and has no ROCm helper threads; `nm -C FoF3.cpu-prod-v79 | grep -c 'fofgpu::'`
is 0. Nothing in this section is relevant to it.

**Use ppn 7 with the fixed binary. Change nothing else in the job script.** At
ppn 7 the fix needs no environment variable and no Slurm flags: it finds the
SMT siblings by itself, because `+pemap` leaves all 56 of them empty.

The fix is worth **about −23%** (relay45 job 5319626, 3 reps: −23.0% on one
stack, −25.4% on the other). Single-rep measurements have given −30.3% and
−31.9%; relay45 retracted the second as noise. The uncertainty is almost
entirely in the *unfixed* arm, which ranged 4581.9 / 4722.8 / 5148.0 across
jobs, while the fixed arm is stable to under 1%. **The fixed configuration is
7–10× more reproducible than the unfixed one**, which is a second benefit worth
having.

**ppn 14 does not earn its keep on the GPU arm** (2881.9 fixed at ppn 7 against
3991.2 at ppn 14, one rep each on the old stack; ppn 14 has shown up to ~700 ms
of spread, so read the direction, not the digits).

**If you do go to ppn 13 or 14** — where every SMT sibling is itself a PE and
the automatic fallback has nowhere to put the helper — add to the job script:

    #SBATCH --core-spec=0
    #SBATCH --cpus-per-task=16
    export FOF_HELPER_CPUS=0,8,16,24,32,40,48,56

That claims the eight OS-reserved CCD-first cores (Frontier permits it:
`AllowSpecResourcesUsage = yes`) and sends the helper threads there, one core
per process, in the same L3 as that process's own PEs. Worth −17.1% at ppn 14.
Without those flags at ppn 14 the fix prints

    [fofgpu] WARNING: affinity fix DECLINED -- no CPU is free of pinned PEs ...

and the problem is back. That warning exists precisely so a silent decline
cannot be mistaken for success.

## 4. `FOF_UF_SIZES=0`. Both arms. New on 2026-08-22.

Union-find maintains a per-component `size` field, and **FoF3 never reads it**:
`collectComponentLabels` returns `componentNumber` only, `componentSize` is
written once as −1 and never read, and every size the app reports — including
`max_size` — comes from the label histogram at the end
(`depositLabelCounts` → `histogramShard` → `collectTouchedCounts`). Maintaining
it costs about 479,000 `add_size` calls per run and 30% of the message traffic
in the union-find drain.

**On CPU it is established.** Two jobs, both within-job pairings, both with
separated ranges:

    job      sizes on     sizes off                delta
    relay79  4629.9 ms    4541.0 [4538.2..4543.7]  −88.9 ms   −1.92%  (2 reps)
    relay82  4577.7 ms    4467.2 [4450.6..4491.4]  −110.5 ms  −2.41%  (3 reps)

**On GPU it is NOT established, and I am saying so rather than quoting the
better of two jobs.** relay80 measured −2.61% with separated ranges at 2 reps;
relay83 re-measured at 2 more and got −1.04% with the ranges overlapping:

    job      sizes on                  sizes off                 delta
    relay80  2630.9 [2630.9..2630.9]   2562.2 [2547.9..2576.5]   −2.61%  SEPARATED
    relay83  2625.4 [2592.7..2658.1]   2598.2 [2590.7..2605.7]   −1.04%  OVERLAPS
    POOLED   2628.1 [2592.7..2658.1]   2580.2 [2547.9..2605.7]   −1.83%  OVERLAPS

relay80's separation rested on a sizes-on pair whose two reps landed 0.012 ms
apart, which is a warning sign rather than a precision claim. At n=4 only the
direction survives. **Set the knob on the GPU arm anyway** — it is free, it is
EXACT in every arm measured, and the direction has never reversed — but do not
quote a GPU percentage.

relay79 job 5326437, relay80 job 5326867, relay82 job 5326927, relay83 job
5327046. The traced pairs (relay81 job 5326926) show the union-find drain
itself shortens — 311 → 255 ms on CPU, 248 → 181 ms on GPU — and that this is
not an artefact of how the drain is defined
(`scripts/relay81-findonly.py` re-measures with `add_size` excluded and the
lengths are identical).

**The library default stays 1 and that is deliberate**, because `unionFindLib`
is shared: a client that does read sizes must not silently get zeros.
`prune_components` aborts under `FOF_UF_SIZES=0` rather than returning wrong
answers. **Set it to 0 for FoF3 runs; do not change the library default.**

## The runnable configuration, both arms

GPU arm:

    binary   merged/FoF3.gpu-prod-v79   md5 d7635c30affc4be14c0da39334e72468
    srun ... --ntasks=128 --gpus-per-node=8 \
      <gcd wrapper> $BIN -f $INPUT -d oct -u dist -c stats -l 128 -i 1 \
      +ppn 7 +pemap 1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63 \
      +lci_ndevices 4 +backend_poll_thread 2
    env: PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1 FOF_UF_SIZES=0

CPU-only arm:

    binary   merged/FoF3.cpu-prod-v79   md5 6ccf74a0fb65408063cd667efb178a06
    srun ... --ntasks=128 \
      $BIN -f $INPUT -d oct -u dist -c stats -l 32 -i 1 \
      +ppn 7 +pemap 1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63 \
      +lci_ndevices 7 +backend_poll_thread 1
    env: FOF_UF_SIZES=0; PARATREET_DEVICE_TREE and FOF_GPU_PHASE1 UNSET

Leave `FOF_WAVE` and `FOF_WAVE_MS` unset. The compression wave is **parked**:
tested at three trigger points, benefit capped at ~260 parent rewrites at any
of them, and +71% at `FOF_WAVE_MS=25` (relay77). It is correct at every gate
and kept only as a validation mechanism.

Both arms use the same `+pemap`: 56 physical cores, no SMT siblings, skipping
the CCD-first cores 0/8/16/…/56.

**On the poller settings.** The rule that matters is that EVERY DEVICE MUST GET
A POLLER: reconverse assigns thread `t` to device `t / ceil(ppn/ndev)`, and the
pollers are the threads at each `backend_poll_thread` stride. Both lines above
satisfy it — check it arithmetically rather than trusting a product formula,
because `ndev 4 / poll 2` at ppn 7 works while `ndev 7 / poll 2` at ppn 7
leaves devices 1, 3 and 5 unpolled. Getting it wrong hangs at cache-manager
init.

## ppn on the CPU-only arm: it barely matters

This reverses advice older than 2026-08-21. The CPU-only ppn-7-over-ppn-14
result was once recorded as 16.5–17.2%. Re-measured at 3 reps on the production
build (relay64 job 5321787):

    p7-phys vs p14-smt,  Debug build,  -l 128     +22%     (superseded)
    p7-phys vs p14-smt,  production,   -l 128     +5.5%
    p7-phys vs p14-smt,  production,   -l  32     +0.8%    <- shipping config

The Debug build was inflating the ppn-14 penalty roughly fourfold, and the
wrong leaf size inflated what was left roughly sevenfold. What survives is
0.82%, non-overlapping over 3 reps: real, and nearly worthless. **ppn 7 is
still the recommendation for the CPU arm, but if there is any other reason to
prefer ppn 14 there, that choice is now open.**

## The acronyms, since I have been careless with them

- **CCD — Core Complex Die.** Frontier's node is one AMD EPYC 7A53 with 64
  cores, packaged in 8 groups of 8, each group a CCD with its own shared L3.
  Slurm reports them as `Sockets=8`. Cores 0–7 are CCD 0, 8–15 are CCD 1, and
  so on. "CCD-first core" means 0, 8, 16, 24, 32, 40, 48, 56 — the eight
  Frontier reserves for the operating system.
- **SMT — Simultaneous Multi-Threading.** Each physical core presents two
  logical CPUs. Core `c` and core `c+64` are the same physical core. They are
  called siblings.
- **PE — Processing Element.** One Charm++ worker thread. `ppn` is how many per
  process; `+pemap` is the list of CPUs they are pinned to.
- **QD — quiescence detection.** `CkWaitQD` / `CkStartQD`: the runtime waits
  until no message is in flight anywhere.

## What the affinity fix actually is

About 135 lines in `paratreet2/fof/gpu/FoFDevice.cpp` — one file, nothing else.
It is upstream on `main` as commit 7c28411.

**The problem it solves.** ROCm creates its helper threads lazily, on the first
real HIP work, from whichever thread is calling. `pthread_create` gives a new
thread its creator's CPU affinity mask. Under `+pemap` that caller is a PE
pinned to exactly one core, so the helper is born welded to that core. The two
then alternate at the scheduler timeslice: the victim PE accumulated 2.7
seconds of runqueue wait in a 26-second run, while every PE with its core to
itself accumulated 0.0 ms. Because a Charm PE with nothing to do spins rather
than sleeps, there was never an idle moment for the helper to use politely.

**What the scope does.** On entry it widens the calling thread's affinity to a
set of CPUs that no PE is pinned to, so the helpers inherit that instead. On
exit it restores the original single-core mask immediately. The CPU set is
either named explicitly by `FOF_HELPER_CPUS`, or derived at runtime: walk
`/proc/self/task`, take every thread pinned to exactly one CPU, add that CPU's
SMT sibling from sysfs, then subtract every pinned CPU so a PE core can never
come back. If no safe set exists it declines and warns.

**Switches.**

    FOF_HELPER_CPUS=<cpu-list>   name the landing zone, e.g. 0,8,16,24,32,40,48,56
    (unset)                      derive it from the SMT siblings
    FOF_NO_AFFINITY_FIX=1        disable entirely, for A/B

## Why it was worth chasing

The symptom was 53–81 ms of quiescence detection per phase boundary on a
machine that was doing nothing at all, six times a run. That is now 0.56 ms,
against an isolated runtime benchmark floor of 0.47 ms. The QD saving alone was
only part of it: the victim PE was absent half of every second all run, so
every phase that had to reach all 896 PEs was paying.

`paper-idea-helper-threads.md` is the separate note, with a broader scope:
helper threads generally, the pitfalls they create in fine-grained parallel
codes, remedies, a literature survey, and a cross-vendor study covering NVIDIA
and possibly Intel as well as AMD.

## Where the numbers come from

    −17.8% build              relay49, job 5320452
    about −23% affinity fix   relay45, job 5319626 (3 reps)
    leaf curves               relay62 job 5321567, relay63 job 5321648
    CPU ppn re-measurement    relay64, job 5321787 (3 reps)
    prefix removal + sort     relay72/73/74, jobs 5324637 / 5324710 / 5324786
    FOF_UF_SIZES=0  CPU        relay79 job 5326437 (2 reps),
                               relay82 job 5326927 (3 reps)
    FOF_UF_SIZES=0  GPU        relay80 job 5326867 + relay83 job 5327046
                               (4 reps, ranges overlap -- direction only)
    drain attribution         relay81, job 5326926 (four traced arms)
    wave parked               relay77, jobs 5325960 / 5325970
