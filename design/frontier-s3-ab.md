# Frontier instructions: S3 phaseB stealing A/B at 2B (+ dist pair)

For the Claude session on Frontier (or Ritvik), written 2026-08-12.
Prereq: the stack builds and the 10k smoke test passes (3549
components) per charm-notes machines/frontier.md.

## What this measures

S3 v1 = coordinator-mediated whole-partition phaseB stealing within the
physical node (design/phaseab-balancing.md sections 19-20). Status when
this was written: laptop-gated both runtimes; Anvil 80M gate green (all
counts exact; forced arm shipped 16 processes' entire pools — 513
shipments, 377k units remote); Anvil 2B job queued (19839458). Frontier
is an independent 2B point on Slingshot, plus the production-path (-u
dist) pair Kale asked for.

## Build

- paratreet2 branch `phaseab-campaign` (S3/S1/S2 exist ONLY there):
  `git fetch && git checkout phaseab-campaign && git pull`.
- unionfind stays on `fof_with_aggregation` tip (has the merged batch
  labeling). htram unchanged.
- `make clean` in paratreet2 src/, fof/, examples/fof3 on EVERY rebuild
  or branch switch — no header dependency tracking (stale-lib trap).

## The one env variable that must be right

`FOF_PROCS_PER_PNODE` = your `--ntasks-per-node` (processes per
physical node). The S3 coordinator block and the physical-node steal
scope are derived from it; default is 8. If your Frontier config runs a
different number of ranks per node, EXPORT IT or the blocks span
machine boundaries.

## Arms (2x2 + one forced), 16 nodes, 2B

Common flags on every arm: `FOF_STEALA=1 FOF_STEALA_GEO=1
FOF_PB_PARTS=32 FOF_PB_M2KEY=1` (S1 claims + KD partitions + cost key —
the S3 substrate). Interleave, 2 reps each of the first four:

| arm | extra env | -u |
|---|---|---|
| base-serial | — | serial |
| s3-serial | FOF_S3=1 | serial |
| base-dist | — | dist |
| s3-dist | FOF_S3=1 | dist |
| s3forced-serial (ONCE, LAST) | FOF_S3=1 FOF_S3_TEST=1 | serial |

The forced arm makes even-rank processes refuse every local claim so
their whole pools must ship — the maximal protocol exercise; expect it
slower, it is a correctness arm not a timing arm.

## Run line

Your standard 2B srun (per design/frontier-labeling-ab.md /
fof3-2b-scaling.md: cray_shasta, job_vni, your pemap, +lci_ndevices
min(8, ppn/2), CXI env), with the arm's env vars prepended and
`-u serial` / `-u dist` as per the table. Input: the same
cosmo25cmb.768g2_dm.001024 as all 2B work.

## Readout (per run)

- EXACTNESS FIRST: `FOF3STAT components: 424897832` — every run, every
  arm. A mismatch is a stop-and-report event.
- The A/B number: `phaseB` field of `FOF3STAT time_s: phase1_stages`
  (and phaseA — S1's effect shows there).
- `FOF3STAT s3:` per-process lines (out_ships/out_units/ret_edges;
  donor-side numbers are authoritative, helper-side in_* may
  undercount — known cosmetic). Natural arms may ship LITTLE if no
  process drains early; that is itself a finding (report it, with the
  phaseA_skew line).
- `FOF3STAT phaseA_skew: within W cross C ...` from every run — the
  cross(P) value at your node count extends the growth curve that
  decides whether S3 v2 must escalate beyond the physical node.
- `FOF3STAT s1_claims:` lines (a sample suffices) — direct steal
  counts.

## Reporting

Collect: per-arm components + rc, the phase1_stages lines, aggregate s3
totals (sum out_ships/out_units/ret_edges over processes), one skew
line per arm, and any anomalies, into a single results file for Kale to
relay back. Include the paratreet2 commit hash and the srun line used.
