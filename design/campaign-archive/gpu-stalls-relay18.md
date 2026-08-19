# relay18 Part A — Ritvik's own stall trace, read

Frontier, 2026-08-19. Trace `/lustre/orion/csc710/proj-shared/16node_gpu_proj_5307458`.
No job was used for this part. Nothing pushed.

---

## 0. Headline

**The 10–60 ms gaps are real, they are quantified, and they sit almost
entirely on the POLLING threads — but the reconverse-lock hypothesis does
not survive the control.** A production-runtime trace that has **neither**
`ofi_lock_mode` **nor** `m_progress_locks` shows the same gaps at the same
rate per PE-second, with the same poller concentration.

    10–60 ms overhead intervals per PE-second
      Ritvik 5307458   (his charm + reconverse + LCI fork, GPU)   0.0330
      production       (charm 90f05d8cb + reconverse main, CPU)   0.0312

And the gaps are not where the hypothesis put them: they are in the
**particle exchange at t = 3–7 s**, not in the QD-heavy uf2 bracket.

## 1. Provenance, from the `.sts` — and it changes the question

Reading the `.sts` header first was worth more than any of the analysis:

| field | value | why it matters |
|---|---|---|
| `PROCESSORS` / `SMPMODE` | 896, `7 128` | **ppn 7, 128 processes** |
| `COMMANDLINE` | `-i 1 +lci_ndevices 4 +backend_poll_thread 2 +pemap 1-7,…` | **This is the RUNBOOK shape, not his ppn-14 sweep shape** |
| `CHARMVERSION` | `v8.0.1-devel-175-g9af1de4b6` | same charm as relay17's arms |
| `TIMESTAMP` | `2026-08-19T15:36:40Z` | 11:36 EDT **today**, not yesterday |
| `+logsize` | 20000000 | ~61k events written per PE, so **no mid-run buffer flush** — the overhead measured here is not trace flushing |
| input | `/lustre/orion/csc710/scratch/rrao/…` | his scratch, not the shared copy |

**His stalls are visible at ppn 7.** They are not a property of the ppn-14
sweep shape. That is worth knowing before Part B spends an allocation
contrasting the two shapes, and it is the opposite of what the relay18
working hypothesis assumed.

## 2. What the trace actually contains

Per-PE state is decided by two flags and nothing else — `in_entry`
(BEGIN/END_PROCESSING) and `in_idle` (BEGIN/END_IDLE); anything else is
OVERHEAD. Over 896 PEs and 10.709 s:

    busy 5029.9 s   idle 4280.6 s   OVERHEAD 54.0 s   =  0.58% of PE-time

Per wall second, the overhead never exceeds 1.6%. **There is no
runtime-wide overhead pathology in this run.**

### The gaps themselves

    OVERHEAD intervals >= 10 ms      326
    in the 10-60 ms band             317   = 7.11 s = 0.076% of all PE-time
    distinct PEs involved            193 of 896

That is a faithful match to the symptom as described: a few PEs, tens of
milliseconds, everyone else idle. It is also **small** — 0.076% of the
machine's time. Whatever is costing this run, these gaps are not it.

### They are on the polling threads

`+backend_poll_thread 2` at ppn 7 makes ranks 0, 2, 4, 6 the pollers, and
`ndevices 4` maps ranks {0,1}→dev0, {2,3}→dev1, {4,5}→dev2, {6}→dev3.

| rank | device | threads on it | poller | 10–60 ms intervals | ms |
|---|---|---|---|---|---|
| 0 | 0 | 2 | **yes** | 85 | 1906 |
| 1 | 0 | 2 | no | 2 | 24 |
| 2 | 1 | 2 | **yes** | 96 | 2087 |
| 3 | 1 | 2 | no | 2 | 27 |
| 4 | 2 | 2 | **yes** | 92 | 2334 |
| 5 | 2 | 2 | no | 3 | 52 |
| 6 | 3 | **1** | **yes** | 37 | 678 |

**310 of 317 intervals are on polling threads — a 44:1 split.** And the
poller that owns its device alone (rank 6) carries about a third of what
the device-sharing pollers carry.

### What bounds them

    opened by   receive(ParticleMsg)  65.6%   flush/flushAck<FragData>  14.6%
    closed by   receive(ParticleMsg)  72.2%   flushAck<FragData>         7.9%
    97.8% end on a message ARRIVAL, and 96.1% of those arrivals are INTER-PROCESS
    resumption fan-in, 20 ms window: p50 8 distinct peer processes, p90 19, max 64

So: a polling PE finishes handling a particle message, sits 10–60 ms
**without going idle**, and is released by a remote particle message, after
which it exchanges with a median of 8 distinct peer processes. The
fan-in half of the old LCI idle-stall precondition is met (51.4% of
resumptions have K ≥ 8).

### Where in the run

| t | intervals | ms | what the machine is doing |
|---|---|---|---|
| 3–4 s | 72 | 1817 | `flush<FragData>` — htram particle send |
| 4–5 s | 96 | 2221 | `receive(ParticleMsg)` + `flushAck` |
| 5–6 s | 65 | 1559 | `receive(ParticleMsg)` + `flushAck` |
| 6–7 s | 28 | 752 | `buildTree` |
| 8–9 s | 43 | 538 | `goDown` / `requestNodes` — phase-3 walk |
| 9–10 s | 10 | 161 | `histogramShard` / `applyUF2Labels` — **the uf2 bracket** |

**261 of 317 are in the particle exchange (t = 3–7 s). Ten are in the uf2
bracket.** The working hypothesis put them in the QD-heavy uf2 bracket.
They are not there.

## 3. The control, and why the lock hypothesis does not survive it

`reconverse` main (`a1207a8`), which the production stack uses, sets
**neither** `ofi_lock_mode` **nor** `m_progress_locks` — those are the +67
lines relay17 identified as the only message-path difference. My own
production traces on scratch use the same `+backend_poll_thread 2`
arrangement, so the poller/co-tenant structure is identical. Running the
same analysis on `proj2b_parbuild5` (charm 90f05d8cb, 183 PEs traced of
1792, ppn 14 / ndev 7, CPU arm):

| | Ritvik 5307458 | production baseline |
|---|---|---|
| locks in the progress path | **both** | **neither** |
| overhead, % of PE-time | 0.58% | 1.06% |
| 10–60 ms intervals per PE-second | **0.0330** | **0.0312** |
| poller : co-tenant, by interval count | 310 : 7 | 111 : 26 |
| opened by `receive(ParticleMsg)` | 65.6% | 43.1% |

**The rate is the same and the poller concentration is present in both.**
A defect that exists only in his reconverse cannot produce a gap rate that
the runtime without it matches to 6%.

What the comparison *does* establish is that **long overhead intervals are
a property of the polling thread**, on either runtime — the PE that calls
`progress()` is the one that spends tens of milliseconds inside the
runtime without registering idle. That is worth telling the Charm team
regardless of Ritvik's stalls.

### Caveats, stated plainly

- The baseline is **not a matched A/B**: different paratreet version, CPU
  arm not GPU, `-u serial` not `-u dist`, ppn 14 not 7, and 183 of 1792
  PEs traced rather than all 896. It controls the *runtime's polling
  structure*, which is what the hypothesis is about, and nothing more.
- Tracing coverage differs in the direction that *favours* the hypothesis
  and it still fails: Ritvik's run traces every PE, which is more tracing
  work everywhere, and his overhead fraction is nonetheless the **lower**
  of the two.
- Rank 6's lighter load is consistent with device sharing, but device 3
  also carries half the traffic of the others, and the production shape
  (7 devices for 14 threads) has no unshared device to compare against.
  This is suggestive, not settled.

## 4. Two analysis errors I made and caught before reporting

Both produced confident, wrong, publishable-looking numbers. Recording
them because the second one would have been very hard to spot downstream.

1. **Sorting the event list by `(time, kind)`.** reconverse emits
   `END_IDLE` and `BEGIN_IDLE` in the *same microsecond* — the scheduler
   leaves the idle condition to poll and re-enters it. Sorting puts `I`
   before `J`, inverting the pair, so the genuine idle period that follows
   is reclassified as an overhead gap. This invented a population of
   "woke from idle → 10–60 ms of nothing → idle again" gaps that appeared
   **41× more common on Ritvik's runtime than on production** and was
   entirely an artifact. State records are time-monotonic in file order
   (verified on 24 files); only CREATION records interleave, and they
   carry a send time, not a state.
2. **Treating `END_PACK`/`END_UNPACK` as a return to OVERHEAD.** Pack and
   unpack nest *inside* an entry method. Ending the overhead state on them
   reclassified ordinary execution as overhead and produced "91.7% of gaps
   open at the end of a pack" — a statement about my state machine, not
   about the run.

The surviving numbers all come from `scripts/relay18_state.py`, whose
model is written down in its docstring and which was validated by hand
against a raw record dump for one PE.

## 5. What this retargets for Part B

1. **His stalls occur at ppn 7**, so arm 2 (the recommended shape) is not
   a "clean control" — expect it to show gaps too. Both arms are now
   positive tests, which is a better experiment than the brief assumed.
2. **Trace the particle exchange, not just the uf2 bracket.** The gaps
   live at t = 3–7 s in `flush`/`receive(ParticleMsg)`/`flushAck`. A
   readout aimed at uf2 would find ten intervals and miss 261.
3. **The discriminator arm should isolate the poller, not the lock.** The
   lock is already contradicted. The open question the trace raises is why
   a polling thread sits 10–60 ms inside the runtime without going idle,
   on *both* runtimes.
4. **A matched control is worth an arm.** Everything above compares two
   runs that differ in five ways at once. Part B can fix that by running
   the same binary and shape on his runtime and on production.

---

# relay18 Part B — the poller sweep, traced, at his own shape

Job **5310158**, 16 nodes, 2B, GPU replace arm, 2026-08-19 17:45:02–17:46:56.
Script `sbatch/relay18-pollersweep-16n.sbatch`. Nothing pushed.

## 6. What was run

Part A established that his stalls happen at **ppn 7**, so every arm here is
ppn 7 with his byte-identical `+pemap`, `-i 1 -c stats -l 128`, on
**`FoF3.up`** — his charm `9af1de4b6` / reconverse `397864f` with **upstream
LCI `b8069dd`**, the go-forward build. All 896 PEs traced, `+logsize
20000000` so there is no mid-run buffer flush, `+traceroot` into a per-arm
directory on scratch, `FOF_KEEPALIVE_MS=10`. Two reps each, interleaved.

| arm | ndevices | poll_thread | pollers/proc | threads/device | domains/node |
|---|---|---|---|---|---|
| **M** | 4 | 2 | 4 (ranks 0,2,4,6) | 2 (rank 6: 1) | 32 — **mimic of 5307458** |
| **L** | 7 | 1 | 7 (all) | 1 | 56 — minimum sharing |
| **H** | 1 | 7 | 1 (rank 0) | 7 | 8 — maximum sharing |

**All six arms exact** — 424,897,832 components / 185,317,566 max_size — and
each wrote 896 `log.gz` plus its `.sts`, ~1.2 GB per arm.

## 7. The three shapes are indistinguishable in wall time

| | M r1 | L r1 | H r1 | M r2 | L r2 | H r2 |
|---|---|---|---|---|---|---|
| iteration 0 | 4.867 s | 4.883 s | 4.966 s | 5.014 s | 4.799 s | 4.762 s |
| UNACCOUNTED | 22.4% | 22.5% | 22.8% | 23.3% | 22.3% | 22.7% |
| phase1 | 0.585 | 0.582 | 0.584 | 0.584 | 0.585 | 0.578 |
| uf2 | 1.136 | 1.088 | 1.120 | 1.132 | 1.096 | 1.092 |

Going from seven threads sharing one libfabric domain to every thread owning
its own costs nothing and saves nothing. The spread across all six runs
(4.76–5.01 s) is smaller than the spread between the two reps of a single
arm. **Whatever the 10–60 ms gaps are, they are not paying for themselves in
wall time, in either direction.**

## 8. One artifact had to be removed first

Every PE's trace opens with one OVERHEAD interval running from its first
record to its first `BEGIN_IDLE`. That is trace startup, not a stall. In five
of the six arms it lands inside the 10–60 ms band and contributes **exactly
896 intervals** — one per PE, all at t = 0–1 s, all opened by "start of
trace" and closed by "the PE went IDLE". Left in, it is 62–85% of the band
population and it swamps the real signal. It does not appear in arm M rep 1,
or in Ritvik's own 5307458, because in those runs the input read was cold and
that first interval ran past 60 ms, out of the band.

`open_entry == -1` identifies it. Everything below excludes it —
`scripts/relay18-band.py`, over the same validated `relay18_state.py`.

## 9. THE RESULT — it is the polling thread, and it is not device sharing

    arm             band  band_ms  per PE-s | pollers  on pollers  co-tenants
    M-p7d4-poll2-r1  485   9130.1   0.0228  | 4/proc     442 (91%)      43
    M-p7d4-poll2-r2  396   7687.5   0.0384  | 4/proc     349 (88%)      47
    L-p7d7-poll1-r1  525  11731.0   0.0499  | 7/proc     525 (100%)      0
    L-p7d7-poll1-r2  539  12077.8   0.0515  | 7/proc     539 (100%)      0
    H-p7d1-poll7-r1  179   2696.0   0.0170  | 1/proc     112 (63%)      67
    H-p7d1-poll7-r2  161   2380.0   0.0157  | 1/proc      84 (52%)      77

**The prediction written into the script was that sharing would order them
L < M < H. The measured order is H < M < L — the exact reverse.** The arm
with every thread on its own private device has the MOST 10–60 ms gaps; the
arm with seven threads crammed onto one device has the fewest, by a factor
of three.

What the count actually tracks is the **number of polling threads**:

| pollers per process | 1 (H) | 4 (M) | 7 (L) |
|---|---|---|---|
| band intervals, rep 2 | 161 | 396 | 539 |

And normalising by the polling PEs themselves makes it flat:

| | polling PEs | band on pollers | **per polling PE-second** |
|---|---|---|---|
| H r1 / r2 | 128 | 112 / 84 | **0.0743 / 0.0573** |
| M r1 / r2 | 512 | 442 / 349 | 0.0363* / **0.0592** |
| L r1 / r2 | 896 | 525 / 539 | **0.0499 / 0.0515** |
| **his 5307458** | 512 | 310 | **0.0565** |

\* M r1's span is 23.75 s because its input read was cold; ~13 s of that
carries no traffic at all, so its denominator is inflated. Over its active
window it is ~0.078.

**A polling PE accumulates 10–60 ms overhead intervals at about 0.05–0.07 per
second no matter how many devices exist, no matter how many threads share
one, and no matter which of the three shapes it is in — and Ritvik's own run
sits at 0.0565, inside that range.** A non-polling PE runs at roughly a sixth
of that (M: 0.011, H: 0.009 per co-tenant PE-second).

## 10. What this settles

1. **The positive test passes.** His symptom reproduces on the go-forward
   build — upstream LCI, no fork — at his own shape, at his own rate
   (0.0592 against his 0.0565 per polling PE-second), with the same poller
   concentration and the same closers (`receive(ParticleMsg)` 33%,
   `addCache` 16%, `requestNodes` 10–14%; 87–95% of the arrival-closed ones
   are inter-process). Nothing about the gaps is specific to his LCI fork,
   which relay17 had already exonerated by two independent routes.
2. **It is not device sharing.** Three shapes spanning 1, 2 and 7 threads per
   domain, and the gap rate per polling thread does not move. Combined with
   Part A's control — production reconverse, which takes neither
   `ofi_lock_mode` nor `m_progress_locks`, gives the same rate — the gaps are
   not about contention for the device at all.
3. **It is a property of being the polling thread.** The PE that calls
   `progress()` spends tens of milliseconds inside the runtime without
   registering idle. Every shape shows it; the total simply scales with how
   many PEs are given that job.
4. **It costs nothing measurable.** 2.4–12.1 s of PE-time out of ~10,100 s,
   and the three shapes finish within 5% of each other. This is a
   *reporting* problem — it makes a few PEs look stalled in projections —
   far more than a performance problem.

## 11. What to change, and what not to

- **If the goal is to stop seeing the symptom in projections**, run
  `+lci_ndevices 1 +backend_poll_thread 7` at ppn 7. It cuts the band
  population by 3.4x and confines it to one PE per process, at no cost in
  wall time. That is a cosmetic fix, and it should be described as one.
- **Do not** chase device counts for performance. M, L and H are the same
  speed to within run-to-run noise.
- **The real question for the Charm team** is the one Part A raised and this
  job confirms across three shapes: why does the PE that calls `progress()`
  sit 10–60 ms inside the runtime without entering the idle state? That is
  reconverse scheduler behaviour on both his branch and main, not an
  application or an LCI question.

## 12. What I did not do

- **No CPU discriminator arm.** The relay18 brief's third arm (mainline CPU
  FoF3 on his charm/reconverse + upstream LCI) was dropped by decision, in
  favour of spending the allocation on the poller sweep. It does not exist as
  a binary. Part A's production comparison stands as the runtime control, with
  its stated caveat that it is not a matched A/B.
- **No ppn-14 arm.** Part A showed his stalls are at ppn 7; relay17 item 9
  already covers the ppn-14 uf2 penalty.
- **Nothing pushed**, and no file in any of Ritvik's repositories changed.
