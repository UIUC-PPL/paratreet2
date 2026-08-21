# What to run, and what the fix is

Written 2026-08-20 for Kale to carry. Self-contained: you do not need any of
the investigation to apply this.

Everything below was measured on Frontier at 2B particles, 16 nodes, and every
arm produced EXACT results (424897832 components, max_size 185317566). The
full evidence trail -- the reports, the diagnostic instruments, the traces --
was moved out of the way into `~/software/stallStudies/`; start at
`relay40.txt` if you ever want it. Nothing there is needed to deploy the fix.

This folder holds three files and nothing else:

    RECOMMENDATION-affinity-fix.md    this page
    gpu-helper-affinity-fix.diff      the fix, standalone, one file changed
    paper-idea-helper-threads.md      why this may be worth writing up

## First, the acronyms, since I have been careless with them

- **CCD — Core Complex Die.** Frontier's node is one AMD EPYC 7A53 with 64
  cores. Those cores are packaged in 8 groups of 8, and each group is a CCD
  with its own shared L3 cache. Slurm reports them as `Sockets=8`. Cores 0-7
  are CCD 0, cores 8-15 are CCD 1, and so on. "CCD-first core" means the first
  core of each of those groups: 0, 8, 16, 24, 32, 40, 48, 56. Those are the
  eight Frontier reserves for the operating system.
- **SMT — Simultaneous Multi-Threading.** Each physical core presents two
  logical CPUs. On Frontier core `c` and core `c+64` are the same physical
  core: 1 and 65, 2 and 66, and so on. They are called siblings.
- **PE — Processing Element.** One Charm++ worker thread. `ppn` is how many of
  them per process; `+pemap` is the list of CPUs they are pinned to.
- **QD — quiescence detection.** `CkWaitQD` / `CkStartQD`: the runtime waits
  until no message is in flight anywhere.

## The recommendation

**Use ppn 7 with the fixed binary. Change nothing else in the job script.**

    gpu-stalls/FoF3.up.aff2      md5 de88154380d9e3fb696f23fa45924b71

At ppn 7 the fix needs no environment variable and no Slurm flags. It finds
the SMT siblings by itself, because `+pemap` leaves all 56 of them empty.

    ppn 7, unfixed      4915.7 - 5007.5 ms
    ppn 7, fixed             3859.1 - 3865.0 ms      about -22%

**ppn 14 only if it earns its keep, and it does not here.** Even with the fix
working at both, ppn 7 beats ppn 14 by 27%:

    ppn 14, unfixed     5926.7 ms
    ppn 14, fixed       4912.4 ms
    ppn 7,  fixed       3865.0 ms

Caveat on that comparison: 1792 PEs is a different decomposition from 896, so
it is not purely a question of SMT. But nothing in the data argues for ppn 14
on performance grounds.

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

## What the fix actually is

About 135 lines in `paratreet2/fof/gpu/FoFDevice.cpp` — one file, nothing
else. Apply `gpu-helper-affinity-fix.diff` from this folder:

    cd <paratreet2> && git apply gpu-helper-affinity-fix.diff

**It applies to the current merged code as-is; no regeneration needed.**
Checked 2026-08-20 in a separate clone: `fof/gpu/FoFDevice.cpp` is
BYTE-IDENTICAL on `main` (6c0a568), on `gpu-phase1` (ae687f7) and on the base
this was developed against (a491d27) — md5 d178162bb1a9929ee93753a49e7ec9d8 —
even though `main` is 189 commits and 17,260 insertions ahead. `git apply
--check` passes on both branches, and the patched file compiles clean with
hipcc against merged `main`.

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

`paper-idea-helper-threads.md`, next to this file, is the separate note. Its
scope is broader than this fix: helper threads generally, the pitfalls they
create in fine-grained parallel codes, remedies, a literature survey, and a
cross-vendor study covering NVIDIA and possibly Intel as well as AMD.
