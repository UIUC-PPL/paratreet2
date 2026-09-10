# Handoff: clusterFinding state as of 2026-09-10

Written at the close of a long laptop session (2026-08-24 → 09-10), ahead
of an account switch. Everything a successor session needs to resume is
either in this file or linked from it. Companion machine/runtime facts
live in `~/software/charm-notes/` (shared repo, pushed).

## 1. Everything committed and pushed — nothing is waiting on this session

| repo | branch | tip | state |
|---|---|---|---|
| paratreet2 | main | `6cfa5c6` | clean, synced |
| unionfind | master | `7794336` | clean, synced |
| charm-notes | main | `292cba6` | clean, synced |
| recharm/charm | reconverse-specific-build | `50dbead86` | clean, 2 behind origin |
| recharm/charm/reconverse | main | `f78dff1` | clean, **5 ahead / 10 behind origin** |

The five local reconverse commits (`CMK_RECONVERSE` discriminator,
`CmiMkdir`, `CsdEnqueue`/`CsdEnqueueLifo`, `CpdSetInitializeMemory`,
`tests/abort_peer`) are ALSO carried on topic branches
(`record-replay-prereqs`, `charm-config-discriminator`) and on a safety
branch `local-work-2026-09-10`. They are deliberately UNPUSHED — the
standing protocol is push to branches, never to `main` of a shared repo.
Pushing them is a decision for Kale, not a chore left undone.

## 2. What was LOST — read this before assuming an artifact exists

This session's scratchpad (`/private/tmp/claude-501/…/52dcc6a2-…/`) was
cleared. Three deliverables existed only there and are **gone**; a
filesystem-wide search on 09-10 confirms no copy survives:

1. **`hapi-helper-affinity.patch`** (527 added lines, 3 files) — a
   drafted port of paratreet2's GPU helper-thread affinity fix into
   Charm++'s HAPI (`src/arch/cuda/hybridAPI`). Six latching RAII scopes
   (`hapiInit`, `hapiCreateStreams`, `hapiEnqueue`, `hapiAddCallback`,
   `hapiPoolMalloc`, plus an exported `User` scope); zone derivation
   generalized for no-SMT machines (Grace); knobs `HAPI_HELPER_CPUS` /
   `HAPI_NO_AFFINITY_FIX`; header-only, no build-system change. It
   applied cleanly to `reconverse-specific-build` and, with `-C1`, to
   `hapi_portable_reconverse{,_support}`. **Regenerable** — the source
   model is committed (`fof/gpu/FoFDevice.cpp:942-1123`) and the design
   is summarized here and in
   `charm-notes/reconverse-core-interference-monitor.md`.
2. **`hapi-helper-affinity-notes.md`** — the HAPI init-flow map with
   file:line, placement rationale, and reviewer questions. Its
   conclusions survive in the charm-notes design note.
3. **`stall-study-migration-inventory.md`** (1108 lines) — the full
   artifact-by-artifact audit. **Its conclusions were distilled before
   loss** into `charm-notes/reconverse-core-interference-monitor.md`
   (commit `3bd8165`) and the `stall-study-migration` memory; the
   1108-line detail is gone.

Lesson for successors: **scratchpad output is not storage.** Anything an
agent produces that matters must be committed the same day.

## 3. Where the durable knowledge now lives

- `design/campaign-report-2026-08.md` — the closed optimization campaign
  (what shipped, what was refuted, measurement rules, parked items).
- `design/debris-audit-2026-08.md` — dead-code audit + dispositions;
  pre-campaign machinery deliberately left, listed for a future pass.
- `projections-tools/` (tracked since `6cfa5c6`) — 30 files: all the
  campaign's Projections analysis tooling, its README/tooling map, and
  `charm-instrumentation.diff` (the uncommitted charm patch: QD
  visibility + off-core detector with `ru_nivcsw` discrimination).
  Previously existed only in an unversioned laptop folder.
- `~/software/charm-notes/reconverse-core-interference-monitor.md` —
  design note for an opt-in runtime core-interference monitor, corrected
  2026-09-01.
- `~/software/charm-notes/machines/{mac,anvil,frontier}.md` — machine
  profiles. Only `mac.md` auto-loads into sessions; point sessions at
  the others explicitly.

## 4. Open items, in rough priority order

**Superseded in part, 2026-09-10: the agenda now lives in GitHub issues.**
paratreet2 #3 (affinity fix silent no-op — item 1 below), #4 (`loadCache`
anti-scaling), #5 (phase-3 walk imbalance); charm #3972 (GPU helper-thread
affinity inheritance), #3973 (`hapi_memory_daemon` spin loops); reconverse
#216 (`CmiOnCore` stub). Going forward, file agenda items as issues rather
than adding them to documents. Items 2, 3 and 5 below are not yet issues
because they are pending other people's reports rather than actionable
work.

1. **LIVE BUG, undecided.** The GPU affinity fix declines SILENTLY when a
   PE's mask is not exactly one CPU (`fof/gpu/FoFDevice.cpp:1053`,
   `CPU_COUNT(&save) != 1 → return`). That is precisely the
   multi-process shape `charm-notes/machines/frontier.md` now
   recommends (`--cpu-bind=cores`, no charm affinity flag, 7-core
   masks), so **a fix measured at −22% (16 nodes) to −63% (7168 PEs) is
   probably inactive in the recommended configuration, unwarned.**
   Correct trigger: "the helper's inheritable mask overlaps cores
   carrying PEs", not `COUNT == 1`. Kale has not yet ruled on changing
   a measured fix's trigger. Any HAPI/runtime port must not inherit the
   same assumption.
2. **Frontier session tasks, sent 2026-09-01, no report yet**: (a) does
   the helper-thread pathology reproduce on
   `reviewed-with-reconverse` + latest reconverse; (b) **salvage
   `~/software/stallStudies/` — relays 19–43, which exist ONLY on
   Frontier**, prioritising relay43 (thread-map) and relay44
   (pthread-interposer) output. That output is the only primary
   evidence naming the offending threads, and the planned
   helper-thread paper rests on it.
3. **NAMD on Vista (TACC, Grace-Hopper), diagnosis pending**: 1-process
   run 30% slower in GPU mode on reconverse, at par CPU-only. Ten-minute
   test: check whether CUDA driver threads (`cuda-EvtHandlr` etc.) carry
   a single-core mask equal to a worker PE's core, plus climbing
   `nonvoluntary_ctxt_switches`. If the build has `SHRINKEXPAND` on, the
   `hapi_memory_daemon` is the prime suspect instead (see §5).
   Also worth asking: how does NAMD learn a kernel finished — host
   functions/stream callbacks, or event-record-and-poll?
4. **Four proposed PRs** for the `reviewed-with-reconverse` migration
   (from the lost inventory, titles preserved): (1) QD visibility in
   Projections — ~60 lines, `qdbench` as test, **applies to charm
   `origin/main` today so it need not wait for the migration**; (2) the
   off-core detector — ~140 lines, needs renaming out of `FOF_*` and
   moving out of `initQd()`; (3) A2/A3 branch upstreaming — blocked on
   one 2B lock A/B; (4) the `+interferencecheck` monitor — ~350 lines,
   never prototyped.
5. Regenerate the HAPI patch if/when the Vista diagnosis confirms.

## 5. Two findings that a successor should not have to re-derive

**The GPU completion mechanism is NOT an escape.** HAPI has two paths
behind `hapiAddCallback()` (`hapi_impl.cpp:1879`): with
`HAPI_CUDA_CALLBACK` defined it calls `hapiLaunchHostFunc`, serviced by a
driver-spawned helper thread; undefined, it calls `recordEvent()` and
polls via `hapiPollEvents` on `CcdSCHEDLOOP` (registered
`hapi_impl.cpp:217-220`), spawning no callback thread. **The macro is
defined nowhere** — not in CMake, configure, arch headers, or paratreet2
— so FoF already ran the polling path and had the helper-thread problem
anyway. GPU runtimes create their INTERNAL threads (HSA queue servicing,
signal/interrupt handlers) on the first device call regardless of how
completions are delivered. Switching mechanisms can only add a thread
class, never remove one.

**The shrink/expand memory daemon is a separate interference source.**
`src/arch/cuda/hybridAPI/hapi_memory_daemon.cpp` (278 lines), a
standalone executable built only under `SHRINKEXPAND`
(`CMakeLists.txt:819`), which also replaces the normal device-init path
(`hapi_impl.cpp:210-215`). Charm does not fork it — it creates FIFOs and
waits on `/tmp/daemon_ready_<device>` — so **its affinity comes from the
launcher**, and the remedy is launcher-side pinning, not an in-process
scope. Two defects found by inspection, unmeasured: the main loop reads
a FIFO opened `O_NONBLOCK` then set blocking, so with no writer attached
`read()` returns 0 immediately and the `usleep(1000)` "spin guard"
becomes a permanent **1 kHz wakeup loop**; and the `EINTR` branch
`continue`s with no sleep at all. It also holds its own CUDA context
(daemon line 134), spawning helper threads that inherit the *daemon's*
mask; one daemon per device.
