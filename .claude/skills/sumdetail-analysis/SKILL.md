---
name: sumdetail-analysis
description: Read Charm++ Projections traces headlessly — especially +sumDetail (.sumd/.sts) — with the bundled parser; use for straggler anatomy, entry-method costs, fan-out checks, and protocol-lifetime questions on FoF/paratreet2 runs. Also covers reading Projections screenshots (images) and what full .log.gz traces are.
---

# Reading Projections sum-detail traces without the GUI

You have no Projections GUI on a cluster login node, and you don't
need one: `.sumd` files are plain text and the bundled
`sumd_tool.py` (this directory; stdlib only) answers the questions the
GUI answers, in tables. It was validated 2026-08-13 against
Projections' own Time Profile of the same trace (sumd2b_slice_v3, the
2B S3 v2 run) and found the s3Shipment serial-rebuild defect
(design/phaseab-balancing.md section 31).

## The files

A `+sumDetail` run with `+traceroot DIR` leaves in DIR:
- `<prefix>.sts` — the dictionary. `ENTRY CHARE <id> "<name>" ...`
  maps EP ids to entry-method names. EP IDS CHANGE BETWEEN BINARIES:
  always resolve names through the .sts of THAT run, never reuse ids.
- `<prefix>.<pe>.sumd` — one per worker PE, 3 lines:
  1. `ver:7.1 cpu:PE/NPES numIntervals:M numEPs:E intervalSize:1.0e-03`
  2. `ExeTimePerEPperInterval <RLE>` — busy per-mil per 1 ms interval
  3. `EPCallTimePerInterval <RLE>` — calls begun per interval
  RLE: `a+b` = value a repeated b times, `a` = once; stream is
  EP-MAJOR (EP0's M intervals, then EP1's M, ...). Value v on line 2 =
  the PE spent v/1000 of that interval in that EP. Time axis = ms
  since program start (matches wall clock of stdout timestamps).
- `<prefix>.<pe>.sum` — coarser single-histogram format; ignore when
  .sumd exists.
- Full event traces (projections mode) are DIFFERENT files
  (`.log.gz` per PE, event records); this tool does not read them.

Trace PEs are WORKER PEs only: Frontier 2B = 14/process, Anvil 2B =
15/process. process = pe // ppn. Pass the right --ppn.

## The tool

Run from the trace directory (or --dir). --ep takes a case-insensitive
regex over entry names, repeatable; each regex is one aggregated group.
Escape '(' in regexes (`'s3Shipment\('`).

```
python3 sumd_tool.py totals    --ep 'phaseBChained' --ep 's3Shipment\('
    # busy seconds, call count, ms/call, PEs active — the first look
python3 sumd_tool.py straggler --ep 'phaseBChained' --ppn 14 --top 12
    # hottest PEs + processes, last-active time, top/median ratio
python3 sumd_tool.py fanout    --ep 'drainForeign' --ppn 14
    # is work spread across each process's PEs or serialized on one?
    # top-PE share 1.0 = one PE does it all; 1/ppn = perfect spread
python3 sumd_tool.py first     --ep 'drainForeign' --ppn 14
    # arrival times: when did each PE first run this EP (staggering)
python3 sumd_tool.py profile   --ep 'startPhase1|phaseBChained' --bin 100
    # text time-profile: busy-PE count per bin — the GUI's Time
    # Profile as an ASCII histogram
```

Cost: ~2 s per 1792-PE 2B trace on laptop SSD; **12-18 s on a cluster
login node over Lustre** (Frontier, measured: cold 18.2 s, warm 11.8 s
— partly I/O, partly real compute). So BATCH every --ep regex you need
into ONE invocation (repeatable; each regex is its own group): a
five-entry roster sweep is one ~12 s call, not five.

## Recipes that have paid off

- PROTOCOL LIFETIME CHECK (do this EARLY for any new protocol —
  charm-notes best-practices lesson 2026-08-13): `totals` over every
  entry of the protocol (for S3: see design/s3-entry-roster.md for
  names and expected magnitudes), then `fanout` on the execution
  entry, `first` on it for arrival staggering, `straggler` on the
  local-work entry. One complete chain of one stolen taskset exposes
  serialization that 30 exact timing runs will not.
- ms/call from `totals` is the entry-method granularity: a data-plane
  entry averaging tens of ms (or a max run of hundreds — see the
  overview images) is a serialization suspect.
- Compare two runs (A/B) by running the same commands in both trace
  dirs; EP names are stable even though ids are not.
- Idle: this format records busy time only; idle = wall x PEs - sum
  of all EP busy. For "how idle was the machine after t" use
  `profile` on the dominant EPs and read the gap to the PE count.

## Images

- You CAN read images: the Read tool renders .png/.jpg visually. When
  Kale relays Projections screenshots (overview/time-profile), read
  them directly and cross-check against the numbers from this tool —
  the 2026-08-13 session's method was exactly that pairing (eyes on
  the overview, numbers from the parse), recorded in section 31.
- You can also PRODUCE images if matplotlib is available
  (`python3 -c 'import matplotlib'` to check; often not on login
  nodes) — but prefer the tool's text tables/histograms in reports;
  they survive the scp/relay path losslessly.

## Caveats

- Traced binaries cost RSS (~40% at 2B; a 2B sumd arm OOMed before
  the windowed-flush fix aba7833). `+traceprocessors 0,10,20-30`
  restricts recording to listed PEs — measured on Frontier
  (relay1 item 5): 224/1792 PEs at 2B ran with NO OOM, ~1.8 MB per
  recorded PE, wall 23/17 s vs 17-19 untraced. (Not separated whether
  the flag avoids the RSS cost or the subset is just small.) Note PE 0
  records regardless, and some in-range PEs may write nothing.
- FULL projections traces (-tracemode projections, .log.gz per PE) go
  further than sum-detail: event records carry per-call timestamps AND
  message length, so entry duration can be regressed against bytes —
  this is what proved s3Shipment ~100% byte-proportional (r=0.999)
  and located the send at 99.2% through its block. A stdlib reader
  exists on Frontier at ~/software/scripts/projlog_tool.py
  (totals/calls/regress/entries; format verified against
  ProjDefs.java: 2/3 BEGIN/END_PROCESSING, 18/19 BEGIN/END_UNPACK,
  field 7 of BEGIN = msglen; ~28 s per 224-PE subset trace).
- Do NOT `du` a traceroot right after srun returns — Lustre writes are
  still landing (59 MB once measured as 531 KB).
- TraceSummary writes at EXIT: a killed run leaves an empty trace dir.
- Verify a binary's tracing state with
  `nm -C <bin> | grep -ci TraceSummary` (0 = untraced, ~465-524 =
  traced); never assume from the filename.
