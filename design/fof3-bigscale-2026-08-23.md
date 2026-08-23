# FoF3 scaling on the 24B and 58B NChilada snapshots (2026-08-23)

First scaling study of FoF3 above the 2B set, and the first run of this
code past 128 nodes. Two snapshots, five node counts each, one FoF
iteration per run.

## What is being measured, and what is being gated

**Measured**: load, decomposition and iteration-0 wall time, and peak
memory, per node count. Iteration 0 carries the scaling signal.

**Gated**: `FOF3STAT components` / `max_size`. A FoF answer is a property
of the data, so these MUST be identical at every node count. Until
2026-08-23 they were not — they drifted upward with process count, a
silent under-merge traced to a 32-bit truncation in unionfind's
`local_union` (design/uf2-under-merge-2026-08-23.md). Every number below
is from the fixed stack; the gate is reported with the table rather than
left to the reader to diff two 11-digit values.

## Configuration

Identical at every point, from one parameterised sbatch
(`/lustre/orion/csc710/scratch/rrao/bigscale/run_fof3_bigscale.sbatch`)
so shape cannot drift between points:

  - 8 processes/node (one per GCD), ppn 7 (one PE per physical core, no
    SMT), 7 LCI devices/process, poll thread 1, leaf 128
  - device phase 1 (`PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1`)
  - oct decomposition, `-u dist`, `-c stats`, **`-i 1`**
  - GCD assignment by NUMA-local wrapper, not by rank

**`-i 1`, not 2.** A second FoF iteration recomputes the same answer on
the same tree: it measures nothing the first does not, and doubles the
cost of every point.

## Two launch-side traps, both fixed here

Neither is a code defect; both present as crashes.

**1. `PMI_MAX_KVS_ENTRIES` must be derived, not inherited.** LCI's
bootstrap alltoall publishes one PMI key per (rank, peer) pair
(`lci-src/src/bootstrap/bootstrap.cpp`, the `LCI_BOOTSTRAP_%d_%d_%d`
loop), so the KVS holds **rank_n^2 entries per bootstrap round**. The
value carried in every script in this project since the 2B campaign is
`4194304` — which is EXACTLY 1024^2, i.e. sized for 128 nodes x 8
processes, and correct only because nothing had ever run wider. At 256
nodes the requirement is 2048^2 = 4194304, already at the limit, and the
job dies during bootstrap before reading a particle:

    _pmi2_add_kvs:ERROR: The KVS data segment of 4194304 entries is not
    large enough

with `rc=143` and thousands of "Terminated" lines — which reads exactly
like a crash. Cost jobs 5333502 (256n) and 5333507 (512n). The sbatch now
computes `2 * NT^2`, floored at the historical value so points <= 128
nodes are byte-identical to before.

**2. A queued job runs the script as it was AT SUBMIT TIME.** Slurm
copies the batch script on submission, so editing the sbatch does not fix
points already in the queue — they must be cancelled and resubmitted.
Relevant whenever a sweep is repaired mid-flight.

Standing traps from earlier campaigns, still enforced in the script:
PEMAP is derived in-script because `sbatch --export` splits on commas; no
filter at the end of an srun pipe (grep block-buffers off a tty, and a
stall then looks like an empty log); `ulimit -c 0` because a CmiAbort at
1024 processes will fill the home quota with core dumps.

## Results

```
dataset     nodes  procs  status   load s  decomp s  iter0 s  eff %  PE0 GB  maxRSS GB  node GB  % node  components
-------------------------------------------------------------------------------------------------------------------
cosmo25        16    128      OK    86.62    157.86    37.02  100.0   51.03      54.82   438.59    87.7  6730729617 / 2214117459
cosmo25        32    256      OK    82.26    118.36    20.17   91.8   31.75      35.99   287.91    57.6  6730729617 / 2214117459
cosmo25        64    512      OK    66.12     86.95    11.20   82.6   20.81      22.65   181.18    36.2  6730729617 / 2214117459
cosmo25       128   1024      OK    57.09     76.43     7.98   58.0   12.82      13.36   106.90    21.4  6730729617 / 2214117459
cosmo25       256   2048      OK    50.68    101.34     8.28   28.0   44.91      45.17   361.38    72.3  6730729617 / 2214117459
romulus25      32    256      OK    50.31    132.74    46.74  100.0   59.62      65.87   526.97   105.4  29193922694 / 125856955
romulus25      64    512      OK    42.96     92.24    23.50   99.4   32.73      36.56   292.51    58.5  29193922694 / 125856955
romulus25     128   1024      OK    32.38     65.35    13.04   89.6   21.42      23.21   185.72    37.1  29193922694 / 125856955
romulus25     256   2048      OK    30.32     84.41    10.23   57.1   49.75      50.04   400.32    80.1  29193922694 / 125856955
romulus25     512   4096    FAIL        -         -        -      -       -       5.14    41.12     8.2  NA

load/decomp/iter0: seconds.  eff % = parallel efficiency of iter0 vs
  the smallest point that fit (100% = ideal strong scaling).
PE0 GB = pe0 vmhwm after decomposition.
maxRSS GB = peak RSS of the WORST task in the step (sacct).
node GB = maxRSS x 8 procs/node; % node is against 500 GB. Above ~90% expect an OOM.
components = FOF3STAT components / max_size.

cosmo25: SCALE-INVARIANT -- components 6730729617 / max_size 2214117459 identical at all 5 node counts
romulus25: SCALE-INVARIANT -- components 29193922694 / max_size 125856955 identical at all 4 node counts

cosmo25: minimum scale that fits = 16 nodes
romulus25: minimum scale that fits = 32 nodes
```

**Correctness gate: PASSED.** Both snapshots give one answer at every node
count -- cosmo25 6730729617 / max_size 2214117459 across 16-256 nodes,
romulus25 29193922694 / max_size 125856955 across 32-256. Log2 histograms
are bit-identical too. Before the `local_union` fix these drifted upward
with process count.

**Minimum scale that fits: cosmo25 16 nodes, romulus25 32 nodes.** The
`% node` column is an upper bound (maxRSS x 8, i.e. assuming every process
peaks simultaneously): romulus25/32n reads 105% and ran, so the real OOM
boundary is a little above that.

### Scaling: both problems stop paying at 128-256 nodes

Iteration 0 strong-scales well to 128 nodes and then stalls or reverses:

  - cosmo25   37.02 -> 20.17 -> 11.20 -> 7.98 -> **8.28** s (256n is SLOWER)
  - romulus25 46.74 -> 23.50 -> 13.04 -> **10.23** s (256n gains only 27%)

Efficiency against the smallest fitting point falls off a cliff there:
cosmo25 100 / 92 / 83 / 58 / 28 %, romulus25 100 / 99 / 90 / 57 %.
romulus25 holds efficiency better at equal node counts, which is the
expected consequence of its being 2.4x larger -- more work per process at
the same width.

Decomposition shows the same knee and more sharply: cosmo25's decomp time
RISES from 76.4 s at 128 nodes to 101.3 s at 256, and romulus25's from
65.3 to 84.4. Past ~1024 processes the decomposition is the thing to fix,
not the walk.

### Open: memory jumps at 256 nodes

maxRSS/process falls smoothly with scale and then jumps at 2048 processes:

    cosmo25    54.8 -> 36.0 -> 22.7 -> 13.4 -> 45.2 GB
    romulus25          65.9 -> 36.6 -> 23.2 -> 50.0 GB

That is +31.8 GB and +26.8 GB respectively -- similar increments for two
snapshots differing 2.4x in size, so most of it is overhead rather than
science data. It is a STEP at 2048 processes, not a trend.

**It is not the PMI KVS.** That was the first hypothesis and it is wrong.
Three 2-node probes with `KVS_FORCE` (peer count held fixed, KVS varied
4.2M / 8.4M / 16.8M) measure **68.5 bytes per entry**, and the byte count
in the 512-node `_pmi2_kvs_fence` overflow decodes independently to 69.2
B/entry. So the KVS costs 0.54 GB at 256 nodes, not tens of GB. Cause not
yet identified; it does not affect correctness and at 72-80% of a node it
did not threaten the budget.

## The 512-node point: BLOCKED by LCI's bootstrap, not by FoF

romulus25 at 512 nodes (4096 ranks) could not be launched by EITHER
bootstrap backend compiled into this build. FoF3 never ran; no science
conclusion is affected.

Root cause is one thing: **`bootstrap::alltoall` is O(rank_n^2) in
published keys** (`lci-src/src/bootstrap/bootstrap.cpp` -- each rank
publishes one `LCI_BOOTSTRAP_%d_%d_%d` key per peer). Both backends fail
downstream of that:

  - **pmi2** (the default; pmix and mpi are NOT compiled in here):
    `_pmi2_kvs_fence` gathers the whole KVS into one buffer whose size is
    computed in a SIGNED 32-BIT int. At 4096 ranks that is 2.32 GB and
    wraps negative:
    `malloc of full_kvs_recvstr failed (-1972027392 bytes)`.
    **Not tunable** -- jobs 5333628 (`PMI_MAX_KVS_ENTRIES`=33.5M) and
    5333839 (=21.0M) printed the IDENTICAL byte count, which is the proof
    that the fence is sized by KVS CONTENT, not by the cap. Raising the
    cap fixes 256 nodes (where the cap really was too small) and does
    nothing at 512.
  - **tcp** (`LCT_PMI_BACKEND=tcp`, validated correct at 2 nodes): the
    single-master rendezvous barrier never completes at 4096 ranks. Not a
    timeout-length problem -- with the default 60 s (job 5333888) and with
    900 s (job 5333902) it fails the same way, 4087 of 4096 ranks timing
    out in the barrier.

So the ceiling for this build is ~2048 ranks (256 nodes at 8 processes per
node), and it is a LAUNCH ceiling. Ways past it, in rough order of value:

  1. Fix the quadratic. A real allgather instead of rank_n^2 KVS keys
     removes the ceiling for every backend at once. Right fix.
  2. Compile LCT with PMIX (`LCT_PMI_BACKEND_ENABLE_PMIX`) and launch with
     `--mpi=pmix`. Sidesteps the int32 fence; the quadratic remains, so it
     buys headroom rather than removing the limit.
  3. Fewer processes per node at 512 nodes (4 x 512 = 2048 ranks) fits
     under the ceiling, but changes the shape and is not comparable to the
     points above.

## Reproducing

    cd /lustre/orion/csc710/scratch/rrao/bigscale
    NODES="256 128 64 32 16" ./submit_bigscale.sh cosmo25
    NODES="512 256 128 64 32" ./submit_bigscale.sh romulus25
    python3 bigscaletab.py logs/bigscale_*.out    # table + correctness gate
    python3 driftcheck.py                          # scale-invariance only

`DSET=cosmo2b` runs the 1.98B tipsy set as a regression gate: it sits
below 2^31, so no width defect can fire there, and its answer
(424897832 / max_size 185317566) must never change.
