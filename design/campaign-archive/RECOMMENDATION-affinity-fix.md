# What to run

Written 2026-08-20, rewritten 2026-08-21 13:55. Self-contained: you do not
need any of the investigation to apply this.

**The filename is now narrower than the contents.** This started as the
write-up of one fix and is now the whole recommended configuration for both
arms. The name is kept so existing links and scp lists still work.

Everything below was measured on Frontier at 2B particles, 16 nodes, and every
arm produced EXACT results (424897832 components, max_size 185317566). Reps
and job numbers are given for every figure. The evidence trail is in
`~/software/reports/`; `RESUME.md` is the map.

## The short version

There are **three** things to get right, not one. They are independent and
they multiply.

    1. build the runtime --with-production and do NOT link tracing
    2. use the right -l for the arm:   GPU 128,  CPU-only 32
    3. GPU only: use the affinity fix at ppn 7

    GPU, all three right       2825 - 2882 ms
    CPU-only, 1 and 2 right    4785 - 4812 ms

Item 1 was worth -17.8% and item 3 about -23% on the GPU arm. Item 2 is worth
-32% on the CPU-only arm and nothing on the GPU arm, where the value already
in use was already correct.

## 1. The build. This one is not optional and it is easy to get wrong.

Charm must be configured `--with-production`, and the application must **not**
be linked `PROJECTIONS=1`.

Without `--with-production`, charm configures `CMAKE_BUILD_TYPE=Debug` with
`-g` and **no optimization at all** for charm and reconverse. That is not
"error checking on". Worth 15.6%.

Linking `-tracemode projections` compiles trace hooks into the message path
even when tracing is never enabled at run time. phaseA is fine-grained and
message-heavy, so this is not free. Worth 7.7%.

Together they were 2117 ms of the CPU iteration, and -17.8% on the GPU ppn-7
fixed arm (3504.9 -> 2881.9 ms). `merged/build-runtime-merged.sh` now passes
`--with-production` permanently, with a comment saying why.

**Build a separate traced binary if you want traces.** A `--with-production`
charm does not build `libtrace-projections.a` at all, so a production arm
cannot be linked `PROJECTIONS=1` even by accident — you get
`charmc: No such tracemode projections`. That error is a feature.

**How to tell, from the binary alone:**

    nm -C <binary> | grep -ci TraceProjections     # must be 0

An environment check cannot catch this. Gate on the artefact.

## 2. The leaf size, `-l`. The two arms want different values.

The FoF3 default is **12** (`examples/fof3/Main.C`). A script that omits `-l`
runs a different problem from one that passes `-l 128`, and the two are not
comparable. Measured at ppn 7 on the production binaries:

    leaf   GPU (FoF3.gpu-prod)      CPU-only (FoF3.cpu-prod)
      12      3606.9 ms  +27.7%        5365.3 ms  +12.1%
      16           --                  5118.2     + 7.0%
      24           --                  4831.3     + 1.0%
      32      3074.3     + 8.8%        4785.3     BEST
      48           --                  4916.5     + 2.7%
      64           --                  5223.2     + 9.2%
     128      2825.0     BEST          7081.0     +48.0%
     192      3059.3     + 8.3%             --
     256      3451.5     +22.2%             --
     384      4316.7     +52.8%             --

Both curves have a genuine interior minimum, so both values are real optima
and not the edge of a sweep. **GPU: `-l 128`. CPU-only: `-l 32`.** The CPU
curve is shallow between 24 and 48 (under 3% across that span), so 32 does not
need to be hit exactly; the GPU curve is sharper.

`-l 128` came from the GPU reference run and was correct there. It was
inherited into the CPU-only scripts, where it costs 48%.

**Check what actually ran**, in both logs, before comparing any two numbers:

    Maximum number of particles per leaf: <n>

## 3. The affinity fix. GPU arm only.

**This applies to the GPU build only.** The CPU-only binary makes no HIP calls
and has no ROCm helper threads; `nm -C FoF3.cpu-prod | grep -c 'fofgpu::'` is
0. Nothing in this section is relevant to it.

**Use ppn 7 with the fixed binary. Change nothing else in the job script.**

At ppn 7 the fix needs no environment variable and no Slurm flags. It finds
the SMT siblings by itself, because `+pemap` leaves all 56 of them empty.

    production build, ppn 7, -l 128
      unfixed      4135.9 ms      (1 rep)
      fixed        2825 - 2882    (2+2+1 reps across three jobs)

The fix is worth **about -23%**. That is the three-rep figure (relay45:
-23.0% on one stack, -25.4% on the other). Single-rep measurements have given
-30.3% and -31.9%; relay45 retracted the second as noise. The uncertainty is
almost entirely in the *unfixed* arm, which has ranged 4581.9 / 4722.8 /
5148.0 across jobs, while the fixed arm is stable to under 1%. **The fixed
configuration is 7-10x more reproducible than the unfixed one**, which is a
second benefit worth having.

**ppn 14 does not earn its keep on the GPU arm.** With the fix working at
both, production build:

    ppn 7,  fixed       2881.9 ms
    ppn 14, fixed       3991.2 ms      +38.5%
    ppn 14, unfixed     4355.6 ms

Those ppn-14 figures are one rep each and ppn 14 has shown up to ~700 ms of
spread, so read the direction, not the digits. Nothing in the data argues for
ppn 14 on the GPU arm.

**If you do go to ppn 13 or 14** — where every SMT sibling is itself a PE and
the automatic fallback has nowhere to put the helper — then add to the job
script:

    #SBATCH --core-spec=0
    #SBATCH --cpus-per-task=16
    export FOF_HELPER_CPUS=0,8,16,24,32,40,48,56

That claims the eight OS-reserved CCD-first cores (Frontier permits it:
`AllowSpecResourcesUsage = yes`) and sends the helper threads there, one core
per process, in the same L3 as that process's own PEs. Worth -17.1% at ppn 14.

Without those flags at ppn 14 the fix will print

    [fofgpu] WARNING: affinity fix DECLINED -- no CPU is free of pinned PEs ...

and the problem is back. That warning is there precisely so a silent decline
cannot be mistaken for success.

## ppn on the CPU-only arm: it barely matters, and that is new

This reverses earlier advice. The CPU-only ppn-7-over-ppn-14 result was
recorded as 16.5-17.2%. Re-measured at 3 reps on the production build:

    p7-phys vs p14-smt,  Debug build,  -l 128     +22%     (superseded)
    p7-phys vs p14-smt,  production,   -l 128     +5.5%
    p7-phys vs p14-smt,  production,   -l  32     +0.8%    <- shipping config

The Debug build was inflating the ppn-14 penalty roughly fourfold, and the
wrong leaf size inflated what was left roughly sevenfold. What survives is
0.82% (4811.3 against 4850.8 ms, non-overlapping ranges over 3 reps): real,
and nearly worthless. **ppn 7 is still the recommendation for the CPU arm, but
if there is any other reason to prefer ppn 14 there, that choice is now open.**

Related and also superseded: a 4-process-of-14 shape on physical cores
(`p14-phys`) was measured at +2.6% against p7 when p14-smt was at +22%. At
`-l 32` that inverts — p14-smt +0.82%, p14-phys +3.15%. Anyone who wants the
mechanism must re-derive it at the right leaf size.

## The runnable configuration, both arms

GPU arm:

    binary   merged/FoF3.gpu-prod    md5 54a8de358d87b17e4d382169da88c426
    srun ... --ntasks=128 --gpus-per-node=8 \
      <gcd wrapper> $BIN -f $INPUT -d oct -u dist -c stats -l 128 -i 1 \
      +ppn 7 +pemap 1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63 \
      +lci_ndevices 4 +backend_poll_thread 2
    env: PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1

CPU-only arm:

    binary   merged/FoF3.cpu-prod    md5 b558cc059f71e43a2cf4afd791b6fef3
    srun ... --ntasks=128 \
      $BIN -f $INPUT -d oct -u dist -c stats -l 32 -i 1 \
      +ppn 7 +pemap 1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63 \
      +lci_ndevices 7 +backend_poll_thread 1
    env: PARATREET_DEVICE_TREE and FOF_GPU_PHASE1 UNSET

Both use the same `+pemap`: 56 physical cores, no SMT siblings, skipping the
CCD-first cores 0/8/16/.../56.

**On the poller settings.** The rule that matters is that EVERY DEVICE MUST
GET A POLLER: reconverse assigns thread `t` to device `t / ceil(ppn/ndev)`,
and the pollers are the threads at each `backend_poll_thread` stride. Both
lines above satisfy it — check it arithmetically rather than trusting a
product formula, because `ndev 4 / poll 2` at ppn 7 works while
`ndev 7 / poll 2` at ppn 7 leaves devices 1, 3 and 5 unpolled. Getting it
wrong hangs at cache-manager init.

At ppn 7 the two lines above are interchangeable on performance: `ndev 7 /
poll 1` gave 4811.3 ms and `ndev 4 / poll 2` gave 4794.4 ms over 3 reps each,
with overlapping ranges.

Build scripts: `merged/build-runtime-merged.sh` then `merged/build-gpu-prod.sh`
(GPU) or `merged/build-two-cpu.sh` (CPU-only).

## The acronyms, since I have been careless with them

- **CCD — Core Complex Die.** Frontier's node is one AMD EPYC 7A53 with 64
  cores, packaged in 8 groups of 8, each group a CCD with its own shared L3.
  Slurm reports them as `Sockets=8`. Cores 0-7 are CCD 0, 8-15 are CCD 1, and
  so on. "CCD-first core" means 0, 8, 16, 24, 32, 40, 48, 56 — the eight
  Frontier reserves for the operating system.
- **SMT — Simultaneous Multi-Threading.** Each physical core presents two
  logical CPUs. Core `c` and core `c+64` are the same physical core: 1 and 65,
  2 and 66, and so on. They are called siblings.
- **PE — Processing Element.** One Charm++ worker thread. `ppn` is how many per
  process; `+pemap` is the list of CPUs they are pinned to.
- **QD — quiescence detection.** `CkWaitQD` / `CkStartQD`: the runtime waits
  until no message is in flight anywhere.

## What the affinity fix actually is

About 135 lines in `paratreet2/fof/gpu/FoFDevice.cpp` — one file, nothing
else. It is upstream on `main` as commit 7c28411. The standalone diff is
`gpu-helper-affinity-fix.diff`:

    cd <paratreet2> && git apply gpu-helper-affinity-fix.diff

It is an RAII scope at the top of `fofgpu::Device::runPhase1Impl`. (The
cumulative patch `0020` in `stallStudies/` contains the same change plus the
diagnostic instrumentation used to find the problem; you do not want that one
unless you are reproducing the measurements.)

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

The symptom was 53-81 ms of quiescence detection per phase boundary on a
machine that was doing nothing at all, six times a run. That is now 0.56 ms,
against an isolated runtime benchmark floor of 0.47 ms. The QD saving alone
was only part of it: the victim PE was absent half of every second all run, so
every phase that had to reach all 896 PEs was paying.

`paper-idea-helper-threads.md` is the separate note. Its scope is broader than
this fix: helper threads generally, the pitfalls they create in fine-grained
parallel codes, remedies, a literature survey, and a cross-vendor study
covering NVIDIA and possibly Intel as well as AMD.

## Where the numbers come from

    -17.8% build             relay49, job 5320452
    about -23% affinity fix  relay45, job 5319626 (3 reps), relay42/43
    leaf curves              relay62 job 5321567, relay63 job 5321648
    CPU ppn re-measurement   relay64, job 5321787 (3 reps)
    2825 / 2852 / 2881.9     relay62 / relay63 / relay49
    4785 / 4794 / 4811       relay63 / relay64 F / relay64 A
