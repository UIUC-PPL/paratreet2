# Frontier instructions: S3 reservation (§27) at 2B — sliced s3-serial pair

For the Claude session on Frontier, written 2026-08-13 morning.
Follows your msg2 (composition finding) and job 5250364 (v2 first net
win). The reservation you proposed is implemented, with the dynamic
trigger refinement — see design/phaseab-balancing.md §27 (on main) and
commit 309673c (on phaseab-campaign).

## What this measures

Donor-side reservation: when the coordinator's remaining-m2 poll sees a
member above FOF_S3_RESERVE_FACTOR (2.0) x the block mean EXCLUDING
SELF (your self-inclusive-mean review point is in), it sends RESERVE;
the donor fences a window [cursor, hi) over the costliest-first pool —
the top-m2 unclaimed prefix — sized by FOF_S3_RESERVE_FRAC (0.5) of
remaining, capped at 4 grants' worth. Local claims skip the window;
grants collect from it FIRST. Starvation valve: locals reclaim
leftover reserved units after the main drain, so the termination
ledger never waits on helpers. This is the fix aimed at your finding:
grants should now carry giants, not dust.

Reservation is ON BY DEFAULT when S3 is armed. FOF_S3_RESERVE=0
disables; FOF_S3_RESERVE_FACTOR=0 makes every member reserve
(correctness-gate config, not a timing arm).

Laptop gates (classic + reconverse -n 4) are green but the windows come
up empty there — small pools drain before RESERVE lands. The 2B
straggler's slow cursor is the real target; this is the first run where
the mechanism can actually engage.

## Build

- `git fetch && git checkout phaseab-campaign && git pull` — must land
  on 4630ef2 (merge of main onto the code branch; contains 309673c
  "S3 section 27: dynamic donor-side reservation").
- NOTE: that merge also brings in aba7833 (windowed-flush reader fix,
  main-side) — your first build with it. Loading/absolute wall rows may
  shift vs 5250364; judge reservation against THIS job's base-serial
  and s3-noreserve arms, not against 5250364's absolute numbers.
- Clean rebuild: `make clean` in src/, fof/, examples/fof3 (stale-lib
  trap, as always). unionfind/htram unchanged.

## Arms — 16 nodes, 2B, -u serial, sliced

Same srun line and base env as your 5250364/5250425 runs
(cray_shasta, job_vni, your pemap, +ppn 14, +lci_ndevices 7,
FOF_PROCS_PER_PNODE=8, FOF_STEALA=1 FOF_STEALA_GEO=1 FOF_PB_M2KEY=1).
For SLICE/GRANT/PARTS, use the best cell from your 5250574 sweep if it
finished (fold its results into the report); otherwise the 5250364
configuration. Hold them FIXED across all arms and state them in the
report.

| arm | extra env | reps |
|---|---|---|
| base-serial | — | 2 |
| s3-reserve | FOF_S3=1 (reservation defaults on) | 2 |
| s3-noreserve | FOF_S3=1 FOF_S3_RESERVE=0 | 1 |

Interleave. The s3-noreserve control isolates reservation from
anything else that moved since 5250364. If the natural s3-reserve runs
show NO `s3_reserve:` lines (trigger never fired at 2B — a finding in
itself), add one run with FOF_S3_RESERVE_FACTOR=1.2 to see where the
trigger threshold sits against the real skew, and report the rem_m2
values so we can calibrate FACTOR.

## Readout (per run)

- EXACTNESS FIRST: `FOF3STAT components: 424897832`, every run.
  Mismatch = stop and report.
- `FOF3STAT s3_reserve: node N lo L hi H m2 M` — which processes got
  windows, how big, how much m2 fenced. Expect: the straggler(s), not
  everyone.
- `FOF3STAT s3:` lines — `resv_shipped` (units shipped FROM windows),
  and out_m2 against `tot_m2` (the shipped-work fraction you asked
  for, now printable).
- `s3_grant_m2_hist` vs the s3-noreserve control — THE composition
  verdict: mass should shift to the high log2m2 buckets if grants now
  carry giants.
- `phaseB_s min/avg/max` — the headline: v2 straggler max was
  1.98-2.11 s against a 0.25 s granularity floor. How far does
  reservation close that 6x gap?
- `phase1_stages` phaseB + the wall-clock rows (Pre-traversal,
  Iteration 0) — v2 references: 4.47-4.53 s / 6.98-7.24 s.
- declines (v2 ran ~1420 — reservation should cut them: ordered
  partitions now hold fenced units) and one `phaseA_skew` line per run.

## Reporting

Single markdown report at the fixed path (~/software/reports/), with
commit hash, job ids, exact srun line, per-arm tables, and the 5250574
sweep results folded in if available. Anomalies verbatim.
