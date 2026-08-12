# S3 phaseB-stealing A/B — Frontier, 2B, 16 nodes

Run 2026-08-12 by the Claude session on Frontier, per paratreet2
`design/frontier-s3-ab.md`. Kale to relay upstream.

**Headline: all 9 runs exact (424,897,832). S3 has NO measurable effect on
phaseB at 2B/16 nodes, because the natural arms barely ship — 1-8 shipments
against ~1,100 declines. The forced arm proves the protocol itself works at
scale (2,035 shipments / 1.15M units, still exact).**

## Provenance

| | |
|---|---|
| paratreet2 | `fbc04ed67054d9b5bb0dcfcfb4bf0a528c15635f` (branch `phaseab-campaign`) |
| unionfind | `8933bae434abc51c9bcef54fdb6c83dbe08786df` (branch `fof_with_aggregation`) |
| htram | `3f2ee4007b26d7cb78ac8b413471dc013ed07fbd` |
| charm | `3d1fdd89f2f53bdb06e2cb4d7989b432a7b812d7` (branch `reconverse-specific-build`) |
| reconverse | `a1207a877bfe0ed6e7cec229e5b2e4e83cbc078b` (branch `main`) |
| input | `/lustre/orion/csc710/scratch/rrao/cosmo25cmb.768g2_dm.001024` (XDR header: nbodies 1,981,808,640; size 76,780,929,056 = header+nsph*48+ndark*36, verified intact) |
| job | 5248429, 16 nodes, COMPLETED, elapsed 00:03:24 |
| logs | `/lustre/orion/csc710/scratch/lvkale/s3ab/5248429/` |
| sbatch | `~/software/s3-ab-16n.sbatch` |

**Note on the spec:** `design/frontier-s3-ab.md` is on branch `main`
(commit `43cb599`), NOT on `phaseab-campaign` where the S3 code lives.
Following the task's step 1 literally leaves you on a branch without the doc;
I read it via `git show origin/main:design/frontier-s3-ab.md`.

## Exact srun line (as executed)

```
srun -N 16 --ntasks-per-node=8 -t 3 \
     --mpi=cray_shasta --network=job_vni --unbuffered \
     --cpu-bind=none --distribution=block:block \
     ./FoF3 -f /lustre/orion/csc710/scratch/rrao/cosmo25cmb.768g2_dm.001024 \
     -d oct -u <serial|dist> +ppn 14 \
     +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
     +lci_ndevices 7 +backend_poll_thread 2 +traceoff
```

Environment on every arm:

```
LCI_ATTR_BACKEND=ofi  FI_CXI_RX_MATCH_MODE=hybrid  PMI_MAX_KVS_ENTRIES=4194304
FOF_STEALA=1  FOF_STEALA_GEO=1  FOF_PB_PARTS=32  FOF_PB_M2KEY=1
FOF_PROCS_PER_PNODE=8        # == --ntasks-per-node
```

Per-arm extra: `FOF_S3=1` (s3 arms), `FOF_S3=1 FOF_S3_TEST=1` (forced).
Arms interleaved, 2 reps of the four natural arms, forced arm once and last.

## Exactness gate — PASSED 9/9

Every run printed `FOF3STAT components: 424897832`, rc=0.

| arm | rep | rc | components | wall |
|---|---|---|---|---|
| base-serial | 1 | 0 | 424897832 | 36 s |
| s3-serial | 1 | 0 | 424897832 | 19 s |
| base-dist | 1 | 0 | 424897832 | 18 s |
| s3-dist | 1 | 0 | 424897832 | 18 s |
| base-serial | 2 | 0 | 424897832 | 19 s |
| s3-serial | 2 | 0 | 424897832 | 18 s |
| base-dist | 2 | 0 | 424897832 | 18 s |
| s3-dist | 2 | 0 | 424897832 | 19 s |
| s3forced-serial | 1 | 0 | 424897832 | 35 s |

(The 36 s on the first run is cold-cache read of the 76.8 GB input; every
subsequent natural run is 18-19 s. The forced arm's 35 s is the expected
protocol cost — it is a correctness arm, not a timing arm.)

## The A/B number: phase1_stages (seconds)

| arm | rep | reset | register | phaseA | **phaseB** | merge | relabel |
|---|---|---|---|---|---|---|---|
| base-serial | 1 | 0.001 | 0.001 | 1.992 | **3.250** | 0.013 | 0.116 |
| base-serial | 2 | 0.001 | 0.001 | 1.985 | **3.243** | 0.013 | 0.109 |
| s3-serial | 1 | 0.001 | 0.001 | 1.978 | **3.270** | 0.014 | 0.114 |
| s3-serial | 2 | 0.001 | 0.001 | 1.984 | **3.257** | 0.014 | 0.112 |
| base-dist | 1 | 0.001 | 0.001 | 1.989 | **3.254** | 0.013 | 0.117 |
| base-dist | 2 | 0.001 | 0.001 | 1.994 | **3.256** | 0.014 | 0.115 |
| s3-dist | 1 | 0.001 | 0.001 | 1.980 | **3.249** | 0.013 | 0.117 |
| s3-dist | 2 | 0.001 | 0.001 | 2.014 | **3.268** | 0.013 | 0.114 |
| s3forced-serial | 1 | 0.001 | 0.001 | 1.977 | **3.236** | 0.041 | 0.113 |

Arm means (phaseB): base-serial **3.247**, s3-serial **3.264**;
base-dist **3.255**, s3-dist **3.259**.

**S3 changes phaseB by +0.017 s (serial) and +0.004 s (dist) — 0.1-0.5% on a
3.25 s phase, well inside run-to-run noise, and the sign is not even
consistent** (s3-dist rep1 at 3.249 is *faster* than base-dist rep1 at 3.254,
while rep2 is slower). Treat this as **no effect**, not as a small regression.

phaseA is flat at 1.98-2.01 across every arm, as expected: `FOF_STEALA=1`
(S1) is part of the common substrate and is therefore ON in the "base" arms
too. This A/B isolates `FOF_S3` alone; it is *not* an S1 A/B.

`merge` is the one field that moves in the forced arm: 0.041 s vs 0.013-0.014 s
everywhere else (~3x), which is the whole-pool shipment being merged back.

## Why there is no effect: the natural arms barely ship

Donor-side `FOF3STAT s3:` totals, summed over all 128 processes:

| arm | rep | out_ships | out_units | ret_edges | declines |
|---|---|---|---|---|---|
| s3-serial | 1 | 2 | 28 | 5 | 1092 |
| s3-serial | 2 | 1 | 2 | 2 | 1085 |
| s3-dist | 1 | 4 | 148 | 10 | 1163 |
| s3-dist | 2 | 8 | 1134 | 57 | 1040 |
| **s3forced-serial** | 1 | **2035** | **1147457** | **611368** | 424 |

(base arms print no `s3:` lines at all — `FOF_S3` is off. Expected, not an
anomaly.)

This is the outcome design/frontier-s3-ab.md anticipated ("Natural arms may
ship LITTLE if no process drains early; that is itself a finding"), now
quantified at 2B/16 nodes: across 128 processes the natural arms managed
**1-8 shipments totalling 2-1134 units**, against **~1,040-1,163 declines**.
So the coordinator is alive and soliciting throughout — it is the donors that
decline, i.e. by the time a helper asks, the donor's pool is already drained.
Stealing has essentially nothing to move at this scale.

The forced arm is the control that proves this is a *workload* property and
not a broken protocol: forcing even-rank processes to refuse local claims
moves **1.15M units in 2,035 shipments and returns 611k edges**, and the
answer is still bit-exact. The mechanism works; the natural opportunity is
absent.

## phaseA_skew (for the S3-v2 escalation curve)

| arm | rep | within | **cross** | global | size_r | max_piece_n |
|---|---|---|---|---|---|---|
| base-serial | 1 | 1.25 | **1.41** | 1.71 | 0.078 | 138233 |
| base-serial | 2 | 1.23 | **1.40** | 1.70 | 0.078 | 138233 |
| s3-serial | 1 | 1.23 | **1.40** | 1.70 | 0.085 | 138233 |
| s3-serial | 2 | 1.26 | **1.41** | 1.70 | 0.083 | 138233 |
| base-dist | 1 | 1.25 | **1.42** | 1.70 | 0.075 | 138233 |
| base-dist | 2 | 1.26 | **1.41** | 1.71 | 0.083 | 138233 |
| s3-dist | 1 | 1.26 | **1.40** | 1.70 | 0.084 | 138233 |
| s3-dist | 2 | 1.26 | **1.41** | 1.73 | 0.079 | 138233 |
| s3forced-serial | 1 | 1.23 | **1.40** | 1.70 | 0.087 | 138233 |

**cross = 1.40-1.42 at 16 nodes / 128 processes**, remarkably stable across
arms and modes (spread 0.02). within ~1.23-1.26, global ~1.70-1.73. This is
the datapoint for the growth curve that decides whether S3 v2 must escalate
beyond the physical node.

## s1_claims

Sample (base-serial rep1, 4 of 128 process lines):

```
FOF3STAT s1_claims: node 75 own 442 foreign 40 pes_with_foreign 11 of 14
FOF3STAT s1_claims: node  2 own 478 foreign 40 pes_with_foreign 10 of 14
FOF3STAT s1_claims: node 16 own 424 foreign 94 pes_with_foreign 10 of 14
FOF3STAT s1_claims: node 77 own 463 foreign 53 pes_with_foreign 10 of 14
```

Summed over all 128 processes:

| arm | own | foreign | foreign share |
|---|---|---|---|
| base-serial rep1 | 56090 | 7856 | 12.3% |
| s3-serial rep1 | 56108 | 7838 | 12.3% |
| s3forced-serial rep1 | 56072 | 7874 | 12.3% |

Direct (S1) stealing is active and identical across arms — as it must be,
since `FOF_STEALA` is common substrate. Typically 10-11 of each process's 14
PEs take at least one foreign claim.

## phase3 / uf2 (dist arms, rep1)

```
base-dist: uf2_setup 0.024  phase3_walk 0.296  edge_gather 0.001  uf2 0.463  relabel 0.122
s3-dist  : uf2_setup 0.023  phase3_walk 0.298  edge_gather 0.001  uf2 0.457  relabel 0.121
```

Unchanged by S3, as expected — S3 acts in phaseB, not phase3.

## Anomalies

None affecting validity. Two things worth noting:

1. **First-run wall time** 36 s vs 18-19 s steady-state — cold page cache on
   the 76.8 GB input, not an arm effect. Any future campaign should treat run
   1 as a warm-up or discard its wall time (phase timings are unaffected:
   base-serial rep1's phaseB 3.250 matches rep2's 3.243).
2. **`declines` dominates the natural arms** (~1,100 per run). Reported
   verbatim; the donor-side counters are the authoritative ones per the task
   doc, and helper-side `in_*` may undercount (known cosmetic issue), so no
   conclusion is drawn from `in_*` here.

## Prerequisite: 4-node 80M shakedown (job 5248439)

Run first to debug the multi-node pipeline. Input
`/lustre/orion/csc710/scratch/rrao/lambb.00500` (XDR nbodies 80,621,568,
size verified). All three arms exact at gold **23,707,197**:

| arm | wall | components | out_ships | out_units | ret_edges |
|---|---|---|---|---|---|
| base-serial | 7 s | 23707197 | 0 | 0 | 0 |
| s3-serial | 3 s | 23707197 | 0 | 0 | 0 |
| s3forced-serial | 7 s | 23707197 | 511 | 382475 | 48950 |

17 s for all three, against the ~20 s Anvil calibration — pemap/`+ppn`
correct, nothing misconfigured. The forced arm's **511 shipments / 382k
units** sits essentially on top of Anvil's recorded 80M gate (**513
shipments / 377k units**) — independent agreement across two machines and two
interconnects.

Also note the natural arms shipped *nothing at all* at 80M/4 nodes, and only
1-8 shipments at 2B/16 nodes: the "no natural opportunity" result is
consistent across both scales, not an artifact of the 2B configuration.

## Frontier-specific gotchas hit on the way (also in ~/software/frontier-corrections.md)

- **Single-node runs need `srun --network=single_node_vni`**; `job_vni` (what
  every recorded 2B line uses) provisions a VNI only for multi-node jobs, and
  on one node the CXI provider fails with `cxip_gen_auth_key failed: -38` →
  `fi_domain()` ENOSYS → an opaque LCI assert. Multi-node `job_vni` is
  correct and was used for both jobs here.
- **`FOF_S3_TEST=1` deadlocks on a single-process run** — rank 0 is even, so
  it refuses every local claim and has no helper to ship to. The 10k forced
  self-check must use >= 2 processes (verified working at 2 processes x 4 PEs,
  3549 components). Not an issue at 128 ranks.
- **Walltime sizing**: `sacct -j <id>` on the job IDs already recorded in
  design/fof3-2b-scaling.md gives the real figures (16-node 2B FoF3 step =
  00:00:52). This whole 9-run campaign took 00:03:24. Size jobs from that,
  not from guesses.