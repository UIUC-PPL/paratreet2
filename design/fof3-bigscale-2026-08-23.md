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

**SUPERSEDED for the iteration, later the same day.** The 128->256
iteration regression above was the uncapped canopy starter pack, and `-s`
removes it -- tuned, 256 nodes is the FASTEST point on cosmo25 (5.35 s).
See "Tuning" below. The decomposition knee is NOT addressed by any of
those flags and stands as written.

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

**Partly reduced by the post-pull commits.** The control run (new binary,
no flags) reads 34368 MB PE0 / 33.7 GB MaxRSS at 256 nodes against the
pre-pull 45984 MB / 45.2 GB -- a 25% cut for free. The STEP itself
survives: 128n control is 13326 MB and 256n control is 34368 MB, still a
2.6x jump across one doubling. And `-D 2` pushes it back up (44.9 GB,
"COST" under Tuning below), so the step is worth understanding before
`-D` is adopted at this width.

## Tuning: `-s` / `-D` / `FOF_UF_SIZES` (2026-08-23, later the same day)

Three settings were recommended for the 24B runs. All three are EXACT --
every run below returns 6730729617 / max_size 2214117459, unchanged.

  - `-s N`  caps the tree-canopy starter pack (`num_share_nodes`).
  - `-D N`  cache share depth (`cache_share_depth`, default 3).
  - `FOF_UF_SIZES=0` skips union-find size bookkeeping FoF never reads.

### Method: control for the binary, not just the flags

The first comparison ran tuned points on the post-pull binary against
baselines from the PRE-pull binary, so binary and flags moved together.
That is not attributable. A CONTROL was run -- post-pull binary, no flags
-- at 128 and 256 nodes, and every claim below is control-vs-tuned on the
SAME binary.

The control was worth the two jobs: it showed the new commits change
iteration time by <=2% (7.98->8.12 at 128n, 8.28->8.31 at 256n), i.e. they
supply the CAPABILITY and the flag is what engages it, while load and
decomposition moved 30-56% between binaries and I/O runs -- so those
columns could not have carried an attribution at all.

### Result: the win is real and grows with process count

cosmo25 iteration 0, control vs `-s 128 -D 2` + `FOF_UF_SIZES=0`:

    nodes   control   tuned    flags alone
       64    11.20*   10.98        -2.0%
      128     8.12     7.02       -13.5%
      256     8.31     5.35       -35.6%
    (*64n is the pre-pull baseline; no 64n control was run.)

**This removes the 256-node regression.** Untuned, 128->256 nodes COSTS
3.7% (design section above). Tuned, 128->256 GAINS 23.8%, and 256 nodes
is now the fastest point measured on this snapshot.

The mechanism is visible in `loadcache_pack`. Uncapped, `raw_canopies`
DOUBLES with node count -- 35120 (64n), 69462 (128n), 136588 (256n) --
and the pack is broadcast to every process, which is the O(P^2). Capped
at `-s 128` it is CONSTANT at 1170 at every scale, and `shipped` is
exactly the cap. Broadcast volume at 256 nodes falls from 8.74 MB x 2048
processes (~17.9 GB) to 0.02 MB x 2048 (~41 MB).

### `FOF_UF_SIZES=0` propagation is CHECKED, not assumed

The library reads it per-process, so a failure to propagate is silent --
the run keeps doing the bookkeeping and merely looks 2% slower. The sbatch
now asserts the census identity and prints UFSIZES_CHECK PASS/FAIL:

    addsize_root    == 0
    addsize_SKIPPED == fb2_UNION   (exactly)

PASS at all three tuned points (8015981 == 8015981 at 256n).

### COST: process memory goes UP, and the cache counters hide it

Same binary, 256 nodes, control vs tuned:

    pool_MB          793537 -> 331178   -58%
    max_MB            607.6 ->  344.4   -43%
    placeholders      2.83B ->  1.20B   -58%
    requests          30.9M ->  37.5M   +21%
    MaxRSS/process   33.7GB -> 44.9GB   +33%   <-- UP

Every cache-accounting number improves as advertised while ACTUAL RSS
rises 11 GB per process. The likely mechanism is `-D 2`: halving the
bundle depth raises the request count 21%, and outstanding requests carry
runtime buffer state the cache pool does not count. At 128 nodes the two
are level (13.87 vs 13.97 GB), so the cost appears at the same scale the
benefit does.

This matters because memory, not time, sets the minimum node count. Do not
read `pool_MB`/`max_MB` as a memory saving without checking sacct MaxRSS
alongside. `-s` and `-D` have not been separated yet; `-s` is the one with
the O(P^2) argument behind it and `-D` is the likelier source of the
memory, so the next pair to run is `-s 128` alone vs `-s 128 -D 2`.

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

### `--mpi=pmi2` clears the bootstrap and destroys the network

Frontier offers `none`, `pmi2` and `cray_shasta`. The harness has always
used `cray_shasta` -- which is WHY `_pmi2_kvs_fence` appears: that is
CRAY's PMI (the `[PE_n]:` prefix is Cray PMI's format), supplied by the
cray_shasta plugin. The untried option is therefore `pmi2`, i.e. Slurm's
own PMI2 server.

It works, and it is unusable. At 2 nodes, same binary, identical HAPI
config:

    plugin         load ms   decomp ms   iter0 ms
    cray_shasta      10.98       26.01      31.10
    pmi2            111.97      772.02    2747.93

10x / 30x / 88x. A uniform slowdown across unrelated phases points at
transport, and the likely mechanism is that `--network=job_vni` (Slingshot
VNI allocation) is coupled to cray_shasta, so LCI loses CXI. NOT YET
CONFIRMED -- the provider probe is still owed. What is established is that
`pmi2` trades the 4096-rank ceiling for a 30-90x network penalty, so it
does not make 512 nodes usable even though it launches.

## Reproducing

    cd /lustre/orion/csc710/scratch/rrao/bigscale
    NODES="256 128 64 32 16" ./submit_bigscale.sh cosmo25
    NODES="512 256 128 64 32" ./submit_bigscale.sh romulus25
    python3 bigscaletab.py logs/bigscale_*.out    # table + correctness gate
    python3 driftcheck.py                          # scale-invariance only

`DSET=cosmo2b` runs the 1.98B tipsy set as a regression gate: it sits
below 2^31, so no width defect can fire there, and its answer
(424897832 / max_size 185317566) must never change.

Tuning knobs (all unset = the historical shape, so the table above
reproduces): `SCAP` -> `-s`, `DEPTH` -> `-D`, `UFSIZES=0` ->
`FOF_UF_SIZES=0` with the census assertion, `MPITYPE` -> the srun `--mpi`
plugin, `BOOTSTRAP=tcp` -> LCI's own TCP rendezvous, `KVS_FORCE` -> pin
`PMI_MAX_KVS_ENTRIES` for memory probes. E.g. the 256-node tuned point:

    sbatch --nodes=256 --export=ALL,DSET=cosmo25,PPN=7,LEAF=128,NDEV=7,\
      POLL=1,ITERS=1,SCAP=128,DEPTH=2,UFSIZES=0 run_fof3_bigscale.sbatch

**Always run a no-flag CONTROL on the same binary before attributing a
change to a flag.** Two separate wrong conclusions in this campaign came
from comparing across binaries (the timing attribution, and a "memory
saving" that is actually a 33% memory INCREASE).
