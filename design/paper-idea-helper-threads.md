# Note for later: helper threads in fine-grained parallel applications

Kale's idea, scope set by him 2026-08-20. A placeholder, not a work item.
The broad subject is **helper threads created by runtimes and libraries, the
pitfalls they generate in complex or fine-grained parallel applications, and
what can be done about them** — with a literature survey, and a cross-vendor
study covering AMD, NVIDIA and possibly Intel GPUs.

Our own result is one instance of the class and would serve as the motivating
case study. Evidence for it: `stallStudies/relay40.txt` through `relay43.txt`.

## The motivating case, in one sentence

`pthread_create` gives a new thread its creator's CPU affinity mask, so a
runtime that lazily spawns a helper from inside a pinned worker welds that
helper to the worker's core — and where the worker spins rather than sleeps,
the two alternate at the scheduler timeslice, costing 22% of an application's
runtime through a mechanism invisible to every profiler that looks at the
application.

    quiet quiescence detection, 896 PEs   53-81 ms  ->  0.56 ms
    isolated runtime floor                              0.47 ms
    victim thread runqueue wait / 26 s    2.7 s     vs  0.0 ms unshared
    iteration, ppn 7                      4915.7 ms ->  3865.0 ms   -22%

## The general problem

A fine-grained parallel application pins its workers and keeps them busy or
spinning. Underneath it, several layers each create service threads of their
own, usually lazily, usually unnamed, usually without asking: GPU runtimes
(ROCm/HIP, CUDA, Level Zero), network progress engines (libfabric, UCX, MPI
progress threads), OpenMP runtimes nested inside a pinned host thread,
telemetry and I/O daemons. The application never sees them and the runtime
never accounts for them.

## The pitfalls worth cataloguing

- **Affinity inheritance.** The one we hit. The helper lands wherever its
  accidental creator happened to be pinned.
- **Spin versus sleep.** A busy-waiting worker leaves no idle runqueue
  anywhere, so a helper cannot be placed "somewhere free" — there is no such
  place. This is what makes the naive fix (widen the mask) fail: the victim
  rotates instead of disappearing.
- **Duty cycle mismatch.** A helper at 12% duty can still cost 50% of a core
  when it contends with a spinning peer, because both get timesliced.
- **Locality.** A helper placed far from the worker it serves adds latency to
  exactly the completion path it exists to shorten.
- **Invisibility.** None of this appears in application-level traces. Our
  symptom was tens of milliseconds of "nothing" at phase boundaries.
- **Amplification by synchronisation frequency.** The cost is (collectives per
  run) x (one timeslice), so fine-grained and frequently-synchronising codes
  pay most. That is the connection to the paper's framing.

## Remedies to study

Explicit placement on service cores; scoping the affinity mask around library
initialisation (what we did); library-side conventions — clear the inherited
mask, name your threads; runtime cooperation, i.e. not spinning; scheduler
policy for service threads; and OS or batch-system support for a "service
thread" placement class. Core specialisation already reserves cores on these
machines — the interesting question is why nothing routes helper threads to
them, and whether it should be automatic.

## The cross-vendor study

The same `LD_PRELOAD` interposer on `pthread_create` answers, in one run per
platform: how many helper threads each runtime creates, when, from which
thread, and with what inherited mask. Worth running on NVIDIA/CUDA and on
Intel GPUs alongside ROCm rather than assuming they behave alike. I have not
measured either, and should not guess at what they do.

## Literature survey — starting points, cited from memory and to be checked

The OS-noise line of work is the obvious neighbour: Petrini, Kerbyson and
Pakin on ASCI Q (SC'03); Ferreira, Bridges and Brightwell on kernel-level
noise injection (SC'08); Hoefler, Schneider and Lumsdaine on simulating noise
at scale (SC'10); Beckman et al. on OS interference at extreme scale. Also
worth pulling: lightweight-kernel and core-specialisation work, and anything
on dedicated cores for communication progress. The gap I think exists: that
literature treats noise as *external* — the OS interfering with the
application. Here the noise is *internal*, created by the application's own
software stack and placed by an accident of thread creation. Whether that
distinction is already made somewhere is the first thing the survey has to
settle.

## The reusable contribution

Four cheap measurement techniques, none of them specific to our stack:

- Bin idle-exit records to see protocols that emit no trace events at all.
- Trigger on a wall-clock gap in the scheduler loop, then take the verdict
  from `CLOCK_THREAD_CPUTIME_ID` and `getrusage(RUSAGE_THREAD).ru_nivcsw`. A
  wall gap alone cannot separate "descheduled" from "long call", nor from
  "blocked in a syscall" — our first version conflated them, and the write-up
  should say so.
- `/proc/<tid>/schedstat` `run_delay`: the kernel's own cumulative
  runqueue-wait accounting, which sampling cannot miss.
- `LD_PRELOAD` on `pthread_create`, logging the owning `.so` and the creator's
  affinity mask. Names the culprit in one run.

Arguably this tooling is the more transferable half of the paper and should
not be an appendix.

## Venue

Short paper or workshop if it stays a case study; the cross-vendor survey plus
the remedy comparison is what would make it a full paper.

## Where the case-study material is

    stallStudies/relay40.txt   one thread pinned to a worker's core, 8 of 8
    stallStudies/relay41.txt   the interposer names ROCm and the call site
    stallStudies/relay42.txt   the fix, -21.5%, and its ppn-14 limitation
    stallStudies/relay43.txt   the OS-reserved-core landing zone, ppn 7 and 14
    tools/interpose-pthread.c        scripts/monitor-threads.py
    scripts/relay40-wakeups.py       scripts/relay46-schedgap.py
    uploads/gpu-helper-affinity-fix.diff   the fix itself, standalone
