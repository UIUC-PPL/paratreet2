# Projections trace-analysis tools from the FoF campaigns (2026-08)

**Tracked copy, added 2026-09-01.** These files lived only in the
unversioned laptop folder `~/software/clusterFinding/projections-tools/`
until a migration audit found 16 of the 17 tool scripts, this README, and
`charm-instrumentation.diff` had no version-controlled copy anywhere. The
partial duplicate under `design/campaign-archive/stall-residue/tools/`
(stall-study subset only) predates this and is left in place. Treat THIS
directory as the canonical copy.

Everything the 2026-08 campaigns (phase-1 load balancing, then the GPU
stall investigation, relays 13–46) built for analyzing Charm++
Projections traces, gathered for a future session whose goal is
incorporating some of this into the Projections tool itself — most
likely as one combined analyzer for VERY LOW UTILIZATION PHASES.
Everything is Python 3 with no third-party dependencies, plus one C
file; trace paths are positional arguments. All of it was validated
against real 896–1792-PE traces on Frontier.

Provenance: `tools/` merges the stall-campaign residue (also archived
in paratreet2 `design/campaign-archive/stall-residue/`, whose
SUMMARY.md is the narrative these tools come from) with the three
trace tools from the earlier relays. `charm-instrumentation.diff` is
the one diagnostic charm patch kept (see below).

## Read first

`tools/RECORD-FORMATS-AND-TOOLS.md` — the stall campaign's own tool
guide: per-tool descriptions, the VERIFIED record layouts (field
orders checked against binaries, not the docs), and the clock-sync
caveat (cross-PE timestamps are safe at the millisecond scale, not
the microsecond scale).

## The non-negotiable base layer

**`relay18_state.py`** — the one correct per-PE busy/idle/overhead
state machine. Everything else builds on it. It exists because two
naive readings each published a false result:

1. **File order, never (time, kind) order**: `BEGIN_IDLE`/`END_IDLE`
   frequently share a microsecond (the scheduler re-enters idle around
   a poll); sorting inverts the pairs and fabricates phantom overhead.
2. **PACK/UNPACK nest inside entry methods** — they are sub-intervals,
   not state transitions.

And one artifact every downstream count must exclude: each PE's
interval from first record to first `BEGIN_IDLE` is trace startup —
exactly one per PE, and it can be IN the measured band on one arm and
out of it on another (cold input read), silently changing A/B
populations. `relay18-band.py` shows the `open_entry == -1` exclusion.

## Inventory by layer

### Projections-trace layer (input: .log.gz + .sts)

| tool | question it answers |
|---|---|
| `relay18_state.py` | per-PE busy/idle/overhead intervals (the base) |
| `relay18-band.py` | how many overhead intervals in a duration band, artifact-excluded |
| `relay32-dump.py` | "what is in this time window?" — all PEs, all records named, entries resolved |
| `relay38-pe.py` | the same for ONE PE, unfiltered (when one PE differs from its neighbors) |
| `relay40-wakeups.py` | protocols that emit NO trace events (QD, CcdRaiseCondition) revealed as idle-exit waves |
| `relay39-qd.py` | every QD episode: post/fire/drain/settle + machine-wide busy inside it (no instrumentation needed) |
| `relay45-ramp.py` | how a broadcast actually spreads (per-process re-multicast records; (source,event) matching COLLIDES) |
| `relay46-schedgap.py` | census of user events / bracketed pairs (type 100); reads what charm-instrumentation.diff writes |
| `projlog_tool.py` | headless full-trace reader: per-CALL durations + MESSAGE LENGTH (regress cost against bytes) |
| `proj-quiet-scan.py` | inter-process quiet windows: is a phase actually silent on the wire? |
| `proj-resume-fanin.py` | distinct-peer fan-in at the resumption after a quiet window |
| `relay75-ufattrib.py` | entry-method-group attribution across the WHOLE run + automatic low-utilization drain detection (found the 334 ms union-find drain the uf2 bracket cannot see) |
| `relay75-chain.py` | burst/wait structure per PE inside a window (one-chain-vs-many; needs ALL PE logs — a 96-PE sample inverted the answer once) |

### OS layer (below Projections — where the helper-thread bug lived)

| tool | question it answers |
|---|---|
| `monitor-threads.py` | every thread's name, CPU, affinity, and cumulative `/proc/<tid>/schedstat` — `run_delay` (runqueue wait) cannot be missed by sampling |
| `relay43-threadmap.py` | which CPUs host more than one thread (matched 8/8 doubled cores to 8/8 victim PEs) |
| `interpose-pthread.c` + `relay44-pcreate.py` | who creates threads, from where, with what inherited affinity (LD_PRELOAD shim + log reader) |

### Instrumentation (charm patch)

`charm-instrumentation.diff` — two files in charm: makes QD visible in
Projections (its messages are Converse handlers, invisible otherwise)
and adds an off-core detector (bracketed user events around scheduler
gaps). Independent of any fix; candidate for upstreaming to charm on
its own merits.

Known superseded: `relay18-analyse.py` (an earlier band counter that
does NOT exclude the startup artifact; its raw numbers are inflated) is
deliberately not in this folder.

## Sketch: the combined low-utilization-phase analyzer

The campaign converged on a de-facto pipeline; a Projections-integrated
tool would run it as one pass over a selected time window:

1. **Busy-time-per-PE summary first** (the methodological lesson: low
   utilization is imbalance or a critical path BEFORE it is
   communication — this campaign's split was 88% straggler tail, 12%
   dependence chain, 0% communication, and thirteen rounds were spent
   on communication before anyone summed busy time). Output: the
   straggler tail vs dependence chain vs genuinely-idle decomposition.
2. **State-machine pass** (relay18_state semantics) → overhead/idle
   intervals per PE, startup artifact excluded, duration-banded.
3. **Gap attribution** for each interval over a threshold: what entry
   opened/closed it, whether it closed on a message arrival, source PE
   and process of the closer, fan-in at resumption (quiet-scan +
   resume-fanin logic).
4. **Sub-Projections view** for windows with no entry methods: idle-
   exit wave detection (wakeups logic) to surface QD and other
   Converse-level protocols; with the charm instrumentation applied,
   QD episodes and scheduler gaps read directly (relay39/46 logic).
5. **Off-core hypothesis check**: intervals concentrated on specific
   PEs in scheduler-timeslice-sized bands (16–24 ms on Linux default
   HZ) suggest a descheduled PE — the signature that OS-layer tools
   then confirm (they are outside a trace tool's scope, but the
   analyzer can NAME the suspicion and the victim PEs).
6. Every claim carries its window bounds ("if you cut a window, say so
   in the output and check what lies just outside it" — a 366 ms miss
   hid behind an anchored window once).

The pieces map cleanly: 1 is a trivial reduction; 2–3 are
relay18_state + band + quiet-scan/fanin; 4 is wakeups + qd; 5 is the
band histogram shape; 6 is discipline the existing tools already
follow. The main integration work is a shared window/selection model
and doing it in one pass instead of five scripts.

## Test traces (`traces/` — LOCAL copies, on this laptop)

Real 2B Frontier data the tools were validated against, copied to
`~/software/clusterFinding/traces/frontier/`; the links resolve
locally, so the integration session can run entirely on the laptop.

| link | what it is |
|---|---|
| `gpu-ppn7-ritvik-5307458` | GPU replace arm, ppn 7 / ndev 4 / poll 2, 896 PEs, -i 1 (897 files: 896 log.gz + .sts) — the trace whose 10–60 ms gaps started the stall investigation (relay18 Part A). THE canonical low-utilization test input: contains the poller-overhead gaps, the QD episodes, and the pre-fix helper-thread victim signature |
| `gpu-pollersweep-M-arm-r2` | job 5310158 arm M rep 2 (ppn 7 / ndev 4 / poll 2, 899 files) — same shape as Ritvik's on the go-forward build with the ring at 10 ms: the A/B companion |
| `cpu-E16-5296573` | CPU arm, ppn 14, full-machine E16 trace — the stall-census baseline (relay13), for contrast with the GPU arms |
| `lvkale-traces-durable` | Frontier-absolute link (dangles locally): the proj-shared stash, incl. the E4096 companion. The remaining 5 poller-sweep arms live only on Frontier scratch (job 5310158) — purgeable; fetch if ever needed |

**Better traces exist on Frontier (2026-08-21, job 5322240,
`/lustre/orion/csc710/scratch/lvkale/s3ab/5322240/traces/{gpu-best,cpu-best}`):
the first PRODUCTION-runtime trace sets** (optimized charm with
TRACING=1 — orthogonal to --with-production in buildcmake), at the
final recommended configurations (GPU ppn 7 / -l 128 / fix on; CPU
ppn 7 / -l 32), ~775/864 MB apparent each, tracing cost ~0-3%. The
LOCAL sets above are Debug-runtime traces — still valid for tool
development and for anything that cited them, but superseded for any
question about the recommended configurations. Fetch 5322240's sets
(scratch, purgeable) if representative traces matter.

## Caveats that transfer

- Cross-PE timestamps: millisecond-scale trust only.
- Reconverse traces specifically exhibit the same-microsecond
  IDLE-pair behavior heavily; classic charm less so, but file-order
  reading is correct on both.
- `monitor-threads.py` caches `Cpus_allowed_list` at first sight
  (pre-pinning masks for early threads); `psr` is sampled fresh and is
  the placement ground truth.
- Everything was validated on Projections VERSION 11.0 traces
  (v8.0.1-devel charm) — re-verify record layouts against other
  versions before trusting field orders.
