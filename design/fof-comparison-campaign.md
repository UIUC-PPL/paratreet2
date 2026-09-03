# FoF3 vs ArborX vs SWIFT: a scaling comparison on Frontier

Campaign started 2026-09-02. Purpose: performance comparisons for the FoF
paper against the two published distributed FoF/clustering codes —
**ArborX** ("Advances in ArborX to support exascale applications", distributed
DBSCAN) and **SWIFT**'s stand-alone FoF ("A Hybrid MPI+Threads Approach to
Particle Group Finding Using Union-Find").

Target sweep:

| dataset | particles | node counts |
|---|---|---|
| `cosmo25cmb.768g2_dm.001024` (Tipsy)      |  1,981,808,640 | 4–64 |
| `cosmo25PLK.2304g_dm.004096` (NChilada)   | 24,461,180,928 | 16–512 |
| `romulus25.3072g.ad.000352` (NChilada)    | 57,982,058,496 | 16–512 |

Everything lives in `/lustre/orion/csc710/scratch/rrao/compare/`
(`src/`, `scripts/`, `bin/`, `data/`, `logs/`, `runs/`).

---

## 1. What is being compared, and why these choices

FoF at linking length `b` is exactly the connected components of the graph
joining particles closer than `b`. That is DBSCAN with `minPts = 2` (ArborX
special-cases it to a connected-components solve) and it is what SWIFT's FoF
computes directly. So all three codes can be made to solve the *identical*
problem — but only if four things are forced to agree, and each of them was
found to differ by default.

### 1.1 The linking length is passed, not recomputed

FoF3 uses `b = 0.2 * (V/N)^(1/3)` where `V` is the volume of **paratreet's
universe box** — and `remakeUniverse` (`src/Driver.h:79`) makes that a CUBE of
side `(1 + 1.91e-6) * max_extent` centred on the particle bounding box, *not*
the bounding box itself. Recomputing `0.2*cbrt(V/N)` from the raw particle
extents therefore gives a different number: on the 100k check set, 0.148620
against FoF3's 0.150205, a **1.07% difference**, which is far more than enough
to move a component count.

SWIFT is worse: its `linking_length_ratio` path derives the mean
inter-particle separation from `Omega_cdm`, `Omega_b` and the critical density
(`src/fof.c:419`) — from cosmological parameters, not from the snapshot's
geometry at all.

So FoF3 was patched to print its own `b` at full precision:

    FOF3STAT linking_length_exact: %.17g volume_exact: %.17g n: %ld

and that value is handed to ArborX as `-e` and to SWIFT as
`FOF:absolute_linking_length`. The linking length is then identical **by
construction** rather than by three independent derivations happening to
agree. Measured values so far:

    cosmo2b    b = 0.00015922463116076563   (V = 1.0000057220458984, N = 1981808640)
    100k check b = 0.150205

### 1.2 All particles are one species

The snapshots are gas+dark (the 2B set is 452,984,832 gas + 1,528,823,808
dark; the two big ones are half gas, half dark). FoF3 and ArborX treat the
snapshot as one undifferentiated point set. SWIFT's FoF does **not**: it
distinguishes *linkable* types (DM, which can start a group) from
*attachable* ones (gas/stars, which are only attached to a group already
found among the linkables). Left at production settings, SWIFT would solve
FoF over the DM subset and then attach the rest — a smaller and cheaper
problem than the one FoF3 is timed on.

Fix: the converter writes **every** particle as `PartType1`, and the parameter
file sets `linking_types: [0,1,0,0,0,0,0]` with `attaching_types` all zero.
All N particles are then linkable, and `fof_link_attachable_particles` does
nothing, so SWIFT is not charged for work the others do not do.

### 1.3 Non-periodic, everywhere

FoF3's reference runs pass no `-P`, i.e. open boundaries. ArborX has no
periodic option at all. So SWIFT is run with `InitialConditions:periodic 0`
and the whole comparison is non-periodic. (This is a real restriction on the
science, not on the comparison: it is the configuration the existing FoF3
scaling numbers were taken in.)

### 1.4 What "components" means

FoF3 counts an isolated particle as its own component — confirmed from its
own log2 histogram, whose bucket 0 (size 1) is 27,971 on the 100k set out of
33,933 components, and 252,506,722 of 424,897,832 on the 2B set. ArborX's
DBSCAN instead labels such particles `-1` ("noise") and does not count them,
so the driver reports **both** conventions. SWIFT counts groups of at least
`FOF:min_group_size` particles.

`min_group_size = 1` is not usable at scale: `fof_compute_group_props`
allocates ~10 arrays of `num_groups` doubles **per rank** and `MPI_Allreduce`s
them, and `num_groups` would be 6.7e9 on the 24B set. Production runs
therefore use SWIFT's own default `min_group_size = 32`, and the comparable
FoF3 quantity is taken from its log2 histogram (buckets 5 and up):

    cosmo2b: 424,897,832 components total, of which 1,423,069 have size >= 32,
             max_size 185,317,566

`max_size` is directly comparable across all three codes at any setting and is
the strongest single gate.

---

## 2. Build state (all on Frontier, gcc 13.2 / ROCm 6.2.4 / cray-mpich 8.1.31)

| component | how | state |
|---|---|---|
| charm_reconverse | `build_charm_cmp.sh`, suffix `cmp`, LCI from `lci-patched` branch `allgather-bootstrap` | built |
| paratreet2 + unionfind + fof + fof3 | `build_fof3.sh` (GPU=1) against `-amd-cmp` | built, **2B gate reproduced exactly** |
| Kokkos 4.6.01 | `build_kokkos.sh`, HIP + gfx90a | built |
| ArborX (51923b4) | `build_arborx.sh`, MPI + Kokkos/HIP | built incl. distributed DBSCAN |
| SWIFT (f219f0f) | `build_swift.sh`, `--enable-stand-alone-fof` | `fof` and `fof_mpi` built |

The 512-node points are reachable only because of the `allgather-bootstrap`
LCI patch: upstream LCI's bootstrap publishes O(rank_n^2) PMI KVS entries and
Cray PMI's fence overflows a signed int32 at 4096 ranks
(`design/fof3-bigscale-2026-08-23.md`). The patched bootstrap is confirmed to
be the one actually executing — `LCI_LOG_LEVEL=info` prints
"Bootstrap allgather round 0 with LCT PMI", not "Bootstrap round 0" — because
`liblci.so` resolves by SONAME through `LD_LIBRARY_PATH` and a mismatched
charm build silently runs the *unpatched* library while reproducing its
behaviour faithfully enough to pass a correctness gate vacuously.

### Four build traps, all resolved

1. **Cray's `h5pcc` cannot be used to configure SWIFT.** `h5pcc -show` emits
   only an rpath (no `-I`, no `-l`) and `-showconfig` reports the *build
   machine's* paths with the version component missing
   (`/opt/cray/pe/hdf5-parallel/gnu/12.3`) plus a leaked
   `/workspace/csml-hdf5/src/H5FDsubfiling`. `ax_lib_hdf5.m4` builds
   `HDF5_CPPFLAGS` as `-I${Installation point}/include`, so every `.c` failed
   on `hdf5.h: No such file or directory`. Fixed with a shim (`shim/h5cc`).
2. **`--with-hdf5=<path>` silently disables parallel HDF5.** `configure.ac:1384`
   gates the parallel test on `if test "$with_hdf5" = "yes"` — the literal
   word. Given a path, the test never runs and the build is SERIAL-IO, which
   is unusable at 58B particles. Must be `--with-hdf5=yes` with the shim on
   PATH.
3. `-Werror` + gcc 13 rejects Cray MPICH's `MPI_Waitall` macro expansion
   (`stringop-overflow`); `--enable-compiler-warnings=yes` keeps the warnings
   without making them fatal.
4. `--with-lustreapi` finds the library but not `lustre/lustre_user.h`; disabled.

---

## 3. Data path

`src/cosmoio.hpp` is a standalone reader for both formats, shared by the
ArborX driver and the SWIFT converter. It deliberately does **not** use
paratreet/utility, so the comparison codes cannot inherit a paratreet
behaviour and thereby stop being an independent check.

Wire facts, each pinned by an exact byte count rather than by documentation:

* **Tipsy** (the 2B set) is **XDR, i.e. big-endian**: 28-byte header + 4-byte
  pad = 32-byte preface, then gas records of 48 B, dark of 36 B, star of 44 B,
  with `pos` at offset 4 and `mass` at offset 0 inside every record.
  Pinned by `32 + 452984832*48 + 1528823808*36 == 76780929056`, the exact file
  size — which is what fixes the preface at 32 and not 28.
* **NChilada** field files are big-endian XDR: 28-byte `FieldHeader`
  (`int magic=1062053; double time; uint64 numParticles; uint32 dimensions;
  int32 typecode`) then min, then max, then the data — so `pos` data starts at
  28 + 12 + 12 = 52. Pinned by `52 + 12230590464*12 == 146767085620`.
* Global ordering is gas, then dark, then star, in both formats.

Reader validation (`bin/cosmoinfo`, and a direct A/B):

* the 100k check set exists in both formats and the two files are
  **bit-identical in all 300,000 coordinates**;
* every slab decomposition (P = 2, 4, 8, 32) agrees between formats and
  agrees with the corresponding window of a single full-file read — 0
  mismatches. This matters because the distributed drivers read slabs, not
  whole files.
* NChilada masses on the 100k set sum to 0.999999975, i.e. a normalised box.
* **family-boundary reads are exact on all three real snapshots**
  (`src/boundary_check.cpp`). This is the one place an off-by-one would
  silently corrupt part of a conversion that costs 2 TB to redo: the 2B Tipsy
  set changes both record stride and base offset mid-slab at particle
  452,984,832 (48 B gas records give way to 36 B dark ones), and the two
  NChilada sets change FILE there. Reads straddling the boundary by +/- 1, 3,
  17, 1000 and 1,048,583 particles -- the last deliberately larger than the
  reader's internal chunk, so it crosses a chunk boundary as well -- agree
  exactly with reads taken wholly inside each family, for both `pos` and
  `mass`, 0 mismatches everywhere. Reads of the final particles succeed and
  reads one past the end are refused rather than silently truncated.
  The gas/dark mass ratio comes out sensible in each case (2B: 1.015e-10 vs
  1.465e-10; cosmo25: 3.945e-12 vs 2.129e-11; romulus25: 1.664e-12 vs
  8.980e-12), and the counts confirm 12,230,590,464 + 12,230,590,464 =
  24,461,180,928 and 28,991,029,248 x 2 = 57,982,058,496.

`bin/nc2swift` (MPI + collective parallel HDF5) converts a snapshot to a
single SWIFT IC file: `Coordinates` float32 (SWIFT asks `H5Dread` for DOUBLE
and HDF5 converts, so storing float32 halves the file and loses nothing —
the source data *is* float32), zero `Velocities`, real `Masses`, and
`ParticleIDs` = global index + 1. 36 B/particle: 71 GB / 880 GB / 2088 GB.
Coordinates are translated so the minimum corner is at the origin (SWIFT
requires `0 <= x < BoxSize`); FoF is translation invariant.

`Header` arrays are written `swift_type_count` = **7** long, not 6:
`read_ic_parallel` indexes `NumPart_Total[ptype]` for every ptype, so a
6-long attribute leaves the neutrino entry uninitialised.

---

## 4. Correctness gate: FoF3 vs SWIFT — PASSED

On the 100k check set, at the same linking length, independently:

    FoF3   FOF3STAT components: 33933  max_size 26042   (singletons 27971)
    SWIFT  No. of groups: 33933
           Largest group (linkables only) by size: 26042

Exact agreement, from a completely independent code reading an independently
written file. FoF3 also gives the identical answer from the Tipsy and the
NChilada copy of that data.

FoF3's own 2B gate reproduces on the rebuilt stack at both 4 and 16 nodes:
`424897832 / 185317566`, matching `design/fof3-2b-scaling.md`.

### 4.1 The production gate is exact at every threshold

`min_group_size = 1` cannot be used at scale (§1.4), so the scale runs use
SWIFT's own default of 32 and the comparable FoF3 quantity comes from its
log2 histogram. That derivation is not assumed -- it was checked against
SWIFT directly on the 100k set, at four thresholds spanning the range:

| `min_group_size` | SWIFT `No. of groups` | predicted from FoF3's histogram |
|---|---|---|
| 1   | 33933 | 33933 = every component |
| 2   |  5962 | 33933 - 27971 singletons |
| 32  |    18 | buckets 5+ = 15 + 1 + 2 |
| 256 |     2 | bucket 14 = 2 |

and `Largest group (linkables only) by size` is 26042 at every one of them,
matching FoF3's `max_size`. (FoF3's 100k histogram is
`0:27971 1:4499 2:1129 3:266 4:50 5:15 6:1 14:2`.)

Note SWIFT's "No. of particles not in groups" is **-66067** at
`min_group_size = 1`: with every particle in a group its
`num_parts_in_groups` accounting double-counts. It is a reporting quirk in a
line this campaign does not use, not a wrong answer -- the group count and
the largest-group size are both exactly right at that setting.

So the gate for the scale runs is fully determined. For the 2B snapshot:

    FoF3   424,897,832 components, of which 1,423,069 have size >= 32
           max_size 185,317,566
    SWIFT  min_group_size 32 must report  No. of groups: 1423069
           Largest group ... by size: 185317566


---

## 5. ArborX's distributed DBSCAN was racing: root cause and fix

**RESOLVED 2026-09-02** (pending the repetition test, job queued). Written up
as "what was expected vs what was found", because the first hypothesis was
wrong and the way it was wrong is the useful part.

### 5.1 What was found

On the 100k check set, at the linking length where FoF3 and SWIFT both answer
33933 / max_size 26042, ArborX answered differently every way it was asked:

| ranks | input | components | max_size | isolated |
|---|---|---|---|---|
| truth | — | **33933** | **26042** | **27971** |
| 2 | nchilada | 32750 | 56951 | 27971 |
| 2 | tipsy    | 33292 | 55082 | 27971 |
| 4 | nchilada | 31798 | 61224 | 27971 |
| 8 | nchilada | 94908 | 21     | 91880 |

Wrong, rank-dependent, and different between the Tipsy and NChilada copies of
a point set that is **bit-identical in all 300,000 coordinates**.

### 5.2 The wrong hypothesis, and how it was killed

The first hypothesis was that ArborX's cross-rank label reconciliation
(`sortAndFilterMergePairs` + `relabel`, a single pass carrying two of the
authors' own `FIXME`s) is complete for shallow merge graphs but not deep ones
— ArborX's generator makes well-separated clusters, whereas cosmological FoF
at b=0.2 percolates (one component holds 9.4% of the 2B snapshot).

That is **refuted**. ArborX's own generator has a documented percolation knob
("If eps is larger than spacing, all the clusters will be merged together"),
and sweeping eps from 1.0 to 8.0 against `spacing = 2` at 4 and 8 ranks gives
`Verification passed` at **every** point (job 5403692). Percolation alone does
not break it.

It should have been killed sooner: percolation is a deterministic property of
the data, and the observations were **nondeterministic**. A deterministic
cause cannot produce a nondeterministic symptom.

### 5.3 What the instrumented run showed

Three probes were added to the driver (job 5403664), each aimed at one
candidate. Eight runs, 2/4/8 ranks, two repetitions each:

* `REDIST_CHECK` **PASS** everywhere, with byte-identical multiset checksums
  across repetitions *and* across the two input formats. The Morton partition
  neither loses, duplicates nor corrupts a point, and is deterministic.
* `LOCALITY` (mean per-rank box diagonal / global diagonal) 0.881 / 0.680 /
  0.475 against the ideal P^(-1/3) of 0.794 / 0.630 / 0.500. The partition is
  spatially compact, which is the property ArborX depends on.
* `ARBORX_VERIFY` — ArborX's *own* `Details::verifyDBSCAN` on ArborX's *own*
  labels — **FAIL, PASS, FAIL, FAIL, FAIL, PASS**. It flipped verdict between
  two runs on provably identical input. At 8 ranks the isolated-particle count
  itself moved, 91174 vs 27971.

So: my input is exact, my partition is compact, and ArborX condemns its own
output nondeterministically. That is a race.

### 5.4 Proven upstream, with a reproducer

The question "my driver or ArborX?" was settled by removing my driver
entirely. `bin/to_arborx_bin` writes the 100k point set in **ArborX's own
benchmark binary format** (`int num_points; int dim; float xyz[]`, 30 lines,
output size trivially checkable: 1200008 == 8 + 12 * 100000), and the stock
`ArborX_Benchmark_DistributedDBSCAN` then reads it with ArborX's own loader,
clusters it with ArborX's own distributed DBSCAN, and checks it with ArborX's
own `Details::verifyDBSCAN`:

    UPSTREAM ranks=2 rep=1 : Verification failed
    UPSTREAM ranks=2 rep=2 : Verification passed
    UPSTREAM ranks=2 rep=3 : Verification failed
    UPSTREAM ranks=2 rep=4 : Verification failed
    UPSTREAM ranks=4 rep=1..4 : Verification failed  (all four)
    UPSTREAM ranks=8 rep=1..4 : Verification failed  (all four)

**11 of 12 runs fail** (job 5403814). No code of mine is in that path. The
defect is upstream.

This also **retracts the refutation in §5.2**. Sweeping the generator's eps
past its `spacing` did force percolation, but it left the generator's
*spatial separation between ranks* intact -- `generateDistributedData` gives
each rank its own lattice block, so a component lives on one or two ranks
however large eps is. What matters is not percolation but the **depth of the
cross-rank merge graph**, and a file-order slab of a cosmology snapshot is
the extreme case: every component has a fragment on every rank, so every
component needs a P-way label reconciliation.

### 5.4.1 The mechanism

`sortAndFilterMergePairs` establishes the invariant `relabel` needs, and does
it with one pass:

1. sort the merge pairs by `(from, to)`;
2. for each run of equal `from`, keep only the entry with the lowest `to`, and
   re-insert `{other_to, to}` for each other `to` in the run so that dropping
   them cannot disconnect anything;
3. re-sort -- **and do not re-filter.**

Step 2's re-inserted edges introduce **new `from` values**, and after step 3's
sort one of those can coincide with a `from` that already carries an edge. So
the output can still contain two edges out of the same label. `relabel`
resolves a label with

    lower_bound(from.begin(), from.end(), label)

and follows the **first** match only, so the second edge out of that label is
silently discarded and two components that should merge do not.

Which edge lands "first" depends on which particle the LOCAL union-find left
as a component's representative, and that varies run to run -- which is why
the symptom is intermittent rather than a clean, reproducible wrong answer,
and why it took the wrong hypothesis twice to corner.

Both of the author's `FIXME`s sit exactly here: *"I need to convince myself
that this is truly necessary"* on the re-insertion, and *"I don't know whether
we have duplicates here or not. If we do, I don't think it matters"* on the
final sort. They do, and it does.

### 5.4.2 Fix

Iterate step 1-2 until no `from` value appears twice -- precisely the
precondition that makes `relabel`'s single-successor lookup well defined.
Each pass rewrites some label to a strictly smaller one, so it terminates; the
patch carries a 64-iteration cap as a safety net rather than an expected path.
Patched locally with the whole argument inline at the site. Worth reporting
upstream together with `bin/to_arborx_bin` and the 12-run job above, which is
a self-contained reproducer.

### 5.4.3 Three missing fences, found on the way and kept

Independently of the above, three host->device `Kokkos::deep_copy`s are issued
on the `space` execution-space instance -- hence asynchronously -- with a
**function-local host mirror** as the staging buffer, whose last reference
dies as the function returns:

| file | function | feeds |
|---|---|---|
| `ArborX_DistributedDBSCANHelpers.hpp:227` | `gatherGlobalBoxes` | `computeRanksTo`, i.e. which ranks exchange ghosts |
| `ArborX_DistributedDBSCANHelpers.hpp:611` | `communicateMergePairs` | `relabel` |
| `ArborX_Distributor.hpp:~437` | `doPostsAndWaits` | every ghost/label exchange |

ArborX already fences the *send* buffers before MPI reads them ("fill on
device done before MPI_Allgather"); nothing guarded a *receive*-path host
buffer's lifetime. Adding the three fences did not fix the component counts --
the merge-pair defect above is the dominant one -- but it did stabilise the
isolated-particle count, which had been swinging between 27971 (correct) and
91174 at 8 ranks. Both changes are kept.

### 5.4.4 Structural limits in ArborX's reconciliation, independent of the bug

Reading the reconciliation closely turned up scaling ceilings that are not
bugs but do bound what the ArborX arm can measure, and they need to be
established BEFORE the 24B/58B points are attempted:

**Every rank materialises the entire global merge-pair set.**
`communicateMergePairs` does an `MPI_Allgatherv` into
`global_merge_pairs` of size `offsets.back()`, and then a host mirror of the
same size. So per-rank memory is O(total merge pairs), not O(local), and the
traffic is O(P x total).

**And the byte offsets are 32-bit.** `computeCountsAndOffsets` fills
`std::vector<int> counts, offsets`, which are then multiplied in place by
`sizeof(MergePair)` because the transfer is done in `MPI_BYTE`:

    auto const sc = sizeof(typename Pairs::value_type);
    std::for_each(offsets.begin(), offsets.end(), [sc](auto &x) { x *= sc; });

so `total_merge_pairs * sizeof(MergePair)` must fit in a signed int. With a
16-byte `MergePair` that is a hard ceiling of roughly **134 million merge
pairs globally**, whatever the rank count or the machine.

That matters a great deal here. The 24B snapshot has 6.73e9 components and
the 58B one 2.92e10; a merge pair is generated per (component fragment, rank)
incidence beyond the first, so even a small percent of components straddling a
rank boundary would exceed 134M. The ArborX arm may therefore be unable to run
the two large snapshots at all -- which is itself a result worth reporting,
but it has to be MEASURED (the merge-pair count per point), not asserted.
Plan: sweep the 2B set first, record the global merge-pair count at each node
count, and extrapolate before spending 512-node jobs.

**`int`-typed local counts.** `n_local`, `ghost_ids`, `ghost_ranks` and
`num_merge_pairs` are all `int`, so per-rank particle counts must stay under
2^31. At 58B particles that needs >= 28 ranks purely for indexing, well below
the memory floor, so it does not bind in practice.

**FDBSCAN_DenseBox is unusable at these linking lengths** and it is the
DEFAULT in `DBSCAN::Parameters` (`_implementation = FDBSCAN_DenseBox`). It
builds a grid of (box/eps)^3 cells: on the 2B snapshot box/eps = 1.0/1.592e-4
= 6280 per side, i.e. 2.48e11 cells. Every run in this campaign therefore
passes `--impl fdbscan` explicitly, and any code calling ArborX's DBSCAN
without `setImplementation` is silently getting DenseBox.

### 5.5 A separate, definite ArborX bug

ArborX's distributed DBSCAN **hangs at `comm_size == 1`**. `computeRanksTo`
filters out the local rank's own box, so a single-rank run queries an *empty*
bounding-volume hierarchy — the case ArborX's own
`FIXME: not 100% sure what's going on` (`ArborX_DistributedDBSCAN.hpp:309`)
marks. Irrelevant to the campaign (every measured point is >= 32 ranks), but
it is what made the first two diagnostic jobs look like total failures.

### 5.6 Method note: two diagnostics that lied

* `R() { ...; timeout 150 srun ... | tail -25; echo "rc=$?"; }` reports
  `tail`'s exit status, not `timeout`'s, so every run printed `rc=0` and a
  genuine completion was indistinguishable from a timeout. The stock
  benchmark also prints **nothing** on success without `--verbose`, so
  "parameter block, then silence" was read as a hang when it was a pass.
  Cost: one wrong conclusion ("it hangs at 2 ranks") that had to be retracted.
* A `grep -m1 -o "log2_histogram:.*"` picked up FoF3's *other* histogram —
  it prints an unrelated `log2_histogram` on its `redundancy_concentration`
  line, which appears FIRST — so the component-size histogram silently read
  `0:279 1:161 2:37 3:7` instead of `0:252506722 ...`. Both greps are now
  anchored.

## 5.7 Status after the fix: upstream correct, MY DRIVER still wrong

The fixed-point patch works, and the gate at campaign-relevant rank counts
says so unambiguously (job 5404085, five repetitions each at 16, 32 and 64
ranks, on the real 100k point set):

    stock ArborX + the fix        15 / 15 Verification passed
    my cosmo_fof driver            0 / 15  (components ~95000, max_size ~20)

Before the fix the stock benchmark failed 11 of 12; after it, 15 of 15 pass at
16/32/64 ranks and 8 of 8 at 4/8 ranks. Two ranks still fails 3 of 4, so
something remains there, but no campaign point uses fewer than 32 ranks.

So the upstream defect is real and fixed, AND my driver has a SEPARATE defect.
That correction matters because §5.3 claimed the probes had "vindicated" my
reader, partition and census. They had not: `REDIST_CHECK` and `LOCALITY` both
read the HOST buffer, and neither checks what ArborX actually searches. A
device-side Kokkos reduction over `points`, compared against the host
checksum of `xyz`, is the check that should have been written first and is now
in place (`DEVICE_CHECK`).

Note also what `REDIST_CHECK` structurally cannot catch: it compares the sum
and sum-of-squares of all coordinates, which are invariant under ANY
permutation -- including a misalignment of the xyz triples that would preserve
the value multiset while destroying every point's geometry. `LOCALITY`
argues against that (the per-rank boxes come out at roughly the ideal
P^(-1/3)), but the decisive test is running my driver with
`--no-redistribute`, which makes it produce exactly the file-order slabs the
now-passing stock benchmark uses. That bisection is job 5404149.

The merge-pair counts are worth recording because they show the two
distributions behaving as designed even while the answer is wrong:

    stock (file-order slabs)  48667 / 75435 / 106000  pairs at 16/32/64 ranks
    mine  (Morton partition)   5256 /  6367 /   9478  pairs at 16/32/64 ranks

File-order slabs give every rank a box spanning the whole domain, so every
component acquires a representative on every rank and the merge graph is
O(components x P). A compact partition only generates pairs for components
that straddle a boundary, an order of magnitude fewer. Both numbers are
2-3 orders of magnitude below the ~134e6 int32 ceiling of §5.4.4 at this
problem size; the 2B sweep is what will say whether that headroom survives.

## 5.7.1 RETRACTION: ArborX's distributed verifier cannot see over-merge

`Details::verifyDBSCAN`'s distributed overload
(`benchmarks/cluster/ArborX_DBSCANVerification.hpp:450`) runs four checks:

    verifyCorePointsNonnegativeIndex
    verifyConnectedCorePointsShareIndex     <-- catches UNDER-merge
    verifyNoisePoints
    verifyConnectedBorderPoints

and carries this comment immediately above them:

    // FIXME: we are skipping verifyClustersAreUnique check as no idea how to
    // do it in distributed setting right now

`verifyClustersAreUnique` is precisely the check that two DISTINCT clusters do
not share a label -- the OVER-merge direction. The distributed verifier is
therefore blind to over-merging by construction.

**This retracts how §5.4/§5.7 read those results.** "Verification passed" was
treated as evidence that the labels were right; it is only evidence that
nothing was under-merged. Concretely, the bisection (job 5404149) at 16 ranks
with `--no-redistribute` reports

    verify=PASS   components 28589   max_size 70651

against a truth of 33933 / 26042 -- i.e. FEWER components and a largest
component 2.7x too big, which is over-merge, passing verification. So after
the merge-pair fix ArborX is still wrong on this data; the failure has moved
from the detectable direction (under-merge, which is what the Morton
distribution still shows: 95172 components, max_size 27, verify=FAIL) to the
undetectable one.

The only trustworthy correctness signal for this arm is therefore the
component census against FoF3's and SWIFT's independently agreed answer, not
ArborX's own verifier. The census's own validity is being pinned separately
by running ArborX's NON-distributed dbscan through the same reader, eps and
census (`--serial`), where the answer must come out 33933 / 26042.

## 5.8 Results so far: FoF3 reference

All three snapshots reproduce the pre-campaign answers exactly, and every
answer is invariant across node count -- the gate the whole comparison rests
on.

    dataset    nodes  procs   load s  decomp s  iter0 s   components / max_size
    cosmo2b        4     32    18.24     38.88    9.674   424897832 / 185317566
    cosmo2b        8     64    30.16     40.89    5.121   (same)
    cosmo2b       16    128    14.53     20.09    2.594   (same)
    cosmo2b       32    256    25.99     29.69    1.671   (same)
    cosmo2b       64    512    24.03     28.00    1.055   (same)
    cosmo25       16    128    47.36    120.68   36.640   6730729617 / 2214117459
    cosmo25       32    256    44.64     83.15   19.569   (same)
    cosmo25       64    512    29.95     52.44   10.328   (same)
    cosmo25      128   1024   108.57    128.99    6.977   (same)
    romulus25     16    128    60.71         -        -   OOM
    romulus25     32    256    49.40    135.44   46.014   29193922694 / 125856955

`romulus25` at 16 nodes OOMs, which is a RESULT and not a failure: the
minimum scale at which the 58B snapshot fits is 32 nodes, exactly as
`fof3-bigscale-2026-08-23.md` found.

Exact linking lengths, now driving the other two arms
(`scripts/linklen.env`):

    cosmo2b    b = 0.00015922463116076563
    cosmo25    b = 6.8897746514307755e-05
    romulus25  b = 5.167330988573081e-05

SWIFT gate values derived from FoF3's log2 histograms, for
`min_group_size = 32`:

    cosmo2b    1,423,069 groups   max_size   185,317,566
    cosmo25    9,156,530 groups   max_size 2,214,117,459

## 5.9 STOP-AND-REPORT: FoF3's component count drifts by +1 at 512 nodes

The standing gate for this project is that a FoF answer is a property of the
data and must be IDENTICAL at every node count. On romulus25 it is not, at the
largest width:

    nodes  procs   PEs     components      max_size
       32    256   1792    29193922694    125856955
       64    512   3584    29193922694    125856955
      128   1024   7168    29193922694    125856955
      256   2048  14336    29193922694    125856955
      512   4096  28672    2919392269_5_  125856955   <-- +1

One component too many out of 29.19 billion, with `max_size` unchanged, so it
is a single missed merge between two SMALL components (a singleton pair, most
likely). cosmo25 is invariant across 16-256 nodes; its 512-node point is still
running as of writing.

**Why this is new.** The 2026-08 campaign could not run 512 nodes at all --
LCI's bootstrap publishes O(rank_n^2) PMI KVS entries and Cray PMI's fence
overflows a signed int32 at 4096 ranks
(`fof3-bigscale-2026-08-23.md`). With the `allgather-bootstrap` patch 4096
ranks now forms cleanly (verified: `Reconverse> Starting Reconverse with 4096
processes, 28672 PEs`), so this is the FIRST time the gate has been evaluated
at this width. The defect was presumably always there.

**Signature matches the width bug class.** `uf2-under-merge-2026-08-23.md`
records exactly this failure mode -- component counts drifting UPWARD with
process count, from a 32-bit truncation in unionfind's `local_union`. That one
was fixed; this is a residual of the same shape, one merge lost somewhere that
only 4096 processes reaches.

**Not yet distinguished: deterministic width bug vs race.** A rerun at 512
nodes would say which (a race gives a different number; a width bug gives
+1 again), but that is ~1000 node-hours for one bit of information, so it
should not be spent blind. Cheaper probes, in preference order:

  1. Look for a residual 32-bit quantity indexed by PROCESS or PE count in the
     phase-3 / UF_2 merge path. 4096 and 28672 are both under 2^15, so a plain
     int16/int32 overflow is unlikely; a more probable candidate is a count or
     offset that scales as procs x something.
  2. Reproduce the PROCESS count without the node cost: 4096 processes fits on
     128 nodes at 32 processes/node (ppn 2). That changes the shape and so is
     not a comparable timing point, but it is a valid CORRECTNESS probe at
     1/4 the cost, and the earlier under-merge was process-count dependent
     rather than node-count dependent.
  3. Run cosmo2b (whose exact answer, 424897832, is the project's regression
     gate) at 512 nodes -- 4096 processes on a snapshot below 2^31, which
     separates "a width defect that needs >2^31 particles" from "a defect in
     the merge that only needs 4096 processes".

Until it is resolved, the 512-node romulus25 TIMING is still usable (the
answer is wrong by one component in 2.9e10, which cannot move a wall time),
but it must be reported with the discrepancy stated, not silently averaged in.

## 5.10 The ArborX arm, rebuilt HACC-style -- and it works

Following the observation that codes running ArborX at trillion-particle
scale do not use `ArborX::Experimental::dbscan`, the arm was rebuilt as
`benchmarks/cosmo_fof/cosmo_fof_hacc.cpp`: ArborX's SINGLE-RANK `dbscan`
kernel plus our own distributed stitching, shaped like HACC's overload-zone
decomposition.

  1. read a file-order slab (the same bytes FoF3 reads);
  2. redistribute onto a REGULAR 3-D BRICK grid. Not a space-filling curve:
     a uniform grid is what HACC uses, and it makes the halo exactly the 26
     neighbouring bricks, so the overload region is an O(1) test per particle
     per direction with no tree over rank bounding boxes. The price is load
     imbalance on clustered data, which is measured and printed;
  3. exchange an eps-thick overload shell with those 26 neighbours;
  4. `ArborX::dbscan` (minPts 2) over own + halo. `setImplementation` is
     MANDATORY here: `DBSCAN::Parameters` defaults to FDBSCAN_DenseBox, which
     builds a (box/eps)^3 grid = 2.5e11 cells on the 2B snapshot;
  5. stitch: label each local component by the minimum GLOBAL particle id it
     contains, send halo labels home, take minima, iterate to a fixed point.

This avoids both problems with the distributed wrapper -- the correctness
defect of §5.7.1, and the ~134e6 merge-pair int32 ceiling of §5.4.4 that made
the 24B and 58B snapshots unreachable through it -- because no global
merge-pair set is ever formed.

**Gate: PASSED, 24/24.** On the 100k check set at 2, 4, 8, 16, 27 and 32
ranks, in both input formats, two repetitions each, every run reports

    census=PASS   components: 33933   max_size 26042

i.e. exactly the answer FoF3 and SWIFT independently agree on, deterministic
across repetitions and invariant to rank count and input format. The stitch
converges in 2-3 rounds at these widths.

**What to watch as this scales.** Load imbalance of the uniform brick grid on
clustered data grows sharply with rank count on this small snapshot:

    ranks     2      4      8     16     27     32
    imbal  1.037  1.620  2.916  6.363 23.915 11.518
    halo%   2.60   5.16   7.87   5.49   2.35   8.20

100k particles over 27 bricks is only 3.7k per brick, so this is close to a
worst case; at 1e9-1e10 particles the per-brick counts are large enough that
density contrast averages out considerably. It must still be reported --
HACC accepts this imbalance too, and it is a real property of the method, not
an artefact of the harness.

**A bug in the first draft, worth recording.** The halo reverse-map
(`back_idx`, which says which own-point each returned halo label belongs to)
was initially left unpopulated: the draft tried to rebuild it by replaying the
forward halo loop, but the coordinates had already been freed, so the loop was
stubbed out and every entry stayed -1. The stitch would then have run its
rounds and propagated NOTHING, reporting a plausible wrong answer. It is now
recorded while the halo send buffers are built, which is the only point at
which the mapping exists.

## 5.10.1 First 2B results, and the +1 is the KNOWN FMA value

    nodes ranks  read  decomp  dbscan  stitch  SOLVE   components   max_size    imbal
        4    32  14.78   3.59   25.26   15.54  40.80   424897833  185317566     4.84
        8    64  13.43   2.51   23.57    9.76  33.33   424897833  185317566     7.57
       16   128  12.03   2.19   25.84   10.60  36.45   424897833  185317566    15.18

`CENSUS_CHECK PASS` at every point; halo 0.27-0.55% of N; stitch converges in
6-7 rounds.

**The component count is NOT off by one in any meaningful sense.** It is
424,897,833 at every rank count, and that is precisely the value
`simd-and-piece-mapping.md:214` and `frontier-session-brief.md:82` record for
FoF3 itself under FMA contraction: "424897833, one component OVER gold,
reproducibly 6/6. gcc defaults to `-ffp-contract=fast`" -- the base FoF3
binary cannot fuse because SSE2 has no FMA, and enabling FMA changes the
rounding of the linking-length test by exactly this one component.

ArborX here is compiled with hipcc (clang), which contracts to FMA by
default. So an entirely independent code, reading the data through an
independent reader, lands on FoF3's own FMA-variant answer EXACTLY, with
`max_size` 185,317,566 identical to the last digit. That is a much stronger
cross-validation than a bit-identical match would have been cheap to claim:
the one-component gap is a fully documented property of the distance test at
this linking length, not a defect in either code.

It also gives the campaign a clean way to state agreement. The three codes'
answers on the 2B snapshot are:

    FoF3   (gcc, no FMA)        424,897,832   max 185,317,566   [gold]
    FoF3   (FMA enabled)        424,897,833   max 185,317,566   [documented]
    ArborX (hipcc, FMA)         424,897,833   max 185,317,566   [this work]
    SWIFT  (gcc -ffast-math)     groups>=32 1,423,075 vs FoF3's 1,423,069,
                                 max 185,316,849 vs 185,317,566

so FoF3 and ArborX agree exactly modulo FMA, while SWIFT's `-ffast-math`
build sits a little further out (4e-7 of the particles) -- consistent with a
more aggressive floating-point mode, and worth confirming by rebuilding SWIFT
with `-ffp-contract=off -fno-fast-math` at one node count.

**Performance, first look.** FoF3's iteration 0 on the same snapshot is
9.674 / 5.121 / 2.594 s at 4 / 8 / 16 nodes against this arm's 40.80 / 33.33 /
36.45 s solve, i.e. FoF3 is 4-14x faster and the gap widens with scale
because this arm barely scales at all between 4 and 16 nodes. Two visible
reasons, both worth reporting rather than tuning away silently:
  - the uniform brick grid's load imbalance rises 4.84 -> 7.57 -> 15.18, and
    the local DBSCAN is set by the worst brick;
  - the stitch performs ~1.8e9 label updates -- for 2e9 particles that is
    close to one update per particle per round, so the label propagation is
    doing far more work than the halo size (0.55% of N) implies it needs to.
    A per-component representative array updated once per round, rather than
    rewriting every point's label, would cut most of it.

## 5.11 SWIFT: works to 64 ranks, blocked above it by METIS

SWIFT now produces correct, deterministic results and a clear best shape.
Shape sweep, 8 nodes, cosmo2b, all four shapes giving the IDENTICAL answer:

    shape   ranks   FOF search s
    1x56        8        547.7
    2x28       16        547.8
    4x14       32        317.6      <-- best
    8x7        64        362.3

and at 4 nodes / 32 ranks (8x7), 419.6 s. Its answer is 1,423,075 groups /
max_size 185,316,849 against FoF3's 1,423,069 / 185,317,566 -- +6 groups and
-717 in the largest, i.e. 4e-7 of the particles, IDENTICAL across all four
shapes and both node counts. Deterministic, so not a race: almost certainly a
floating-point boundary effect at the linking length (FoF3's `Real` vs
SWIFT's double coordinates, plus SWIFT's `-ffast-math`). Worth pinning before
publication but it is not an algorithmic disagreement.

Three SWIFT blockers fixed:

  - `engine_maxproxies` is a COMPILE-TIME cap of 64 on the number of
    NEIGHBOUR RANKS per rank. Every run at >= 128 ranks died with
    "Maximum number of proxies exceeded"; 64 ranks was fine. Raised to 1024.
  - `--verbose` needs an INTEGER (`--verbose=1`); bare `--verbose` exits 1 on
    every rank.
  - `--cpus-per-task` must be requested at JOB level, not passed to srun.

**OPEN.** At >= 128 ranks SWIFT still dies in its initial domain
decomposition with METIS's

    ***Cannot bisect a graph with 0 vertices!

This is NOT simply cells-per-rank: it failed at 16/rank (cdim 16, 256 ranks),
succeeded at 64/rank (cdim 16, 64 ranks), and failed again at 72/rank
(cdim 21, 128 ranks). Next things to try, cheapest first:
  1. `DomainDecomposition:initial_type: grid` -- a purely geometric split
     that does not call METIS at all. Robust, at some cost in balance, and
     closer to what FoF3 and the HACC-style arm do anyway.
  2. `usemetis: 1` (serial METIS instead of ParMETIS) and `adaptive: 0`.
  3. Check whether SWIFT requires the cell grid to be commensurate with the
     rank count in some way the docs do not state.

## 6. Harness

    scripts/run_fof3.sbatch     derived from the proven bigscale sbatch; all
                                its traps retained (PMI KVS sizing derived not
                                inherited, PEMAP derived in-script because
                                sbatch --export splits on commas, no filter at
                                the end of an srun pipe, ulimit -c 0)
    scripts/run_arborx.sbatch   8 ranks/node, one per GCD, NUMA-local wrapper
    scripts/run_swift.sbatch    RPN x THREADS parameterised; SWIFT is CPU-only
    scripts/convert_swift.sbatch
    scripts/collect.py          one table + the scale-invariance gate

### A "fix" that did nothing for two jobs

`MPICH_OFI_NIC_POLICY` was added to `env/env.sh` as

    export MPICH_OFI_NIC_POLICY=${MPICH_OFI_NIC_POLICY:-ROUND-ROBIN}

after the NUMA NIC policy killed conversion job 5403507. The next conversion,
5403617, died with the **identical** error. The defensive `:-` default is the
reason: Frontier's site modules already export
`MPICH_OFI_NIC_POLICY=NUMA`, so the variable was never empty and the default
never fired. Written as a plain unconditional assignment (with a separate
`CMP_NIC_POLICY` for a deliberate override), it works.

The general lesson, worth carrying: `${VAR:-default}` for a variable the
environment already sets is not a default, it is a no-op, and it fails
silently. Every job now echoes the resolved value into its log so the same
thing cannot recur unnoticed:

    ### MPICH_OFI_NIC_POLICY=ROUND-ROBIN (must NOT be NUMA)

### A harness race that cost a job: one shared GCD wrapper

Every script wrote its NUMA-local GCD wrapper to the same path,
`scripts/gcd_wrap.sh`, with a heredoc at job start. With several jobs
starting close together one truncates the file while another is exec'ing it:

    /var/spool/slurmd/job5403759/slurm_script: line 26:
      .../scripts/gcd_wrap.sh: Text file busy

The wrapper's CONTENT is identical in every script, so this cannot produce a
wrong GPU assignment -- it fails loudly or not at all -- but losing a 64-node
job to it would be expensive. The path is now per-job
(`gcd_wrap.$SLURM_JOB_ID.sh`).

Fixing it required re-submitting the seven queued FoF3 jobs: **Slurm copies
the batch script at SUBMIT time**, so editing the sbatch does not fix points
already in the queue (the same trap `fof3-bigscale-2026-08-23.md` records for
the PMI KVS sizing). They had not started, so nothing was lost.

`MPICH_OFI_NIC_POLICY=ROUND-ROBIN` is set in `env/env.sh`: Cray MPICH's
default NUMA NIC policy refuses to start when a rank is not confined to one
NUMA node, and 8 ranks x 7 cores straddles Frontier's four 16-core NUMA
domains however it is bound. This killed the first conversion job (5403507)
in two seconds.

### Predicted minimum node counts for SWIFT

From SWIFT's own reported struct sizes (`sizeof(gpart) = 112` bytes) plus
FoF's two `size_t` arrays per gpart (`group_index`, `attach_index`), the floor
is ~128 B/particle before cells (896 B each), tasks, proxies and MPI buffers:

    cosmo2b    1.98e9  x 128 B = 0.25 TB  -> fits from 4 nodes
    cosmo25   24.46e9  x 128 B = 3.13 TB  -> >= 7 nodes bare, ~16 with headroom
    romulus25 57.98e9  x 128 B = 7.42 TB  -> >= 15 nodes bare, ~32 with headroom

which lines up with where FoF3 fits (16 nodes for cosmo25, 32 for romulus25),
so the requested 16-512 node range should be reachable for SWIFT on both big
snapshots. `gpart_foreign` is 40 B and scales with the halo, not the volume.

**SWIFT is CPU-only** — there is no GPU port of `fof.c`. Comparing it to
FoF3's GPU-assisted phase 1 and to ArborX's HIP DBSCAN is an asymmetry that
must be stated rather than hidden, so the campaign will also run a CPU-only
FoF3 arm (`FOF_GPU_PHASE1` unset) to give a like-for-like CPU comparison
alongside the best-vs-best one.

---

## Session 2026-09-02b: the two blockers cleared

### SWIFT above 64 ranks: `DomainDecomposition:initial_type: grid`

The METIS abort (`***Cannot bisect a graph with 0 vertices!`) that killed every
SWIFT point at >= 128 ranks is **not** a cells-per-rank threshold. It failed at
16 cells/rank, worked at 64, and failed again at 72, which is why four rounds of
retuning `Scheduler:max_top_level_cells` got nowhere.

The decisive probe was three jobs at 128 ranks on cosmo2b, identical shape
(32 nodes, 4 ranks x 14 threads), identical cdim 21, differing only in the
decomposition strategy:

| job | `initial_type` | `usemetis` | result |
|---|---|---|---|
| 5404566 | `grid` | 0 | **OK**, fof_ms 404158.311, groups 1423075, max 185316849 |
| 5404567 | `memory` | 1 (serial METIS) | FAIL rc=143 |
| 5404568 | `memory` | 1 + adaptive | FAIL rc=143 |

`grid` is a purely geometric split of the top-level cell grid over the ranks
(`main: initial partitioning: axis aligned grids of cells / grid set to
[ 8 4 4 ]`) and never enters METIS at all. It reproduces the same answer the
`memory` runs gave at <= 64 ranks (1423075 / 185316849), so it is not trading
correctness for reach.

`DDTYPE` now **defaults to `grid`** in `run_swift.sbatch`, and the whole SWIFT
curve is being re-run on it -- including the low-rank points that `memory`
could do -- because a scaling line assembled from two different decomposition
strategies is not quotable. It also takes ParMETIS out of the timed path, so
the SWIFT numbers depend only on SWIFT.

### The 1500 s stall watchdog was killing good SWIFT runs

Job 5404516 (cosmo2b, 4 nodes, 4x14) was killed by `WATCHDOG STALL: log
unchanged for 1500s`. Its last log line was at SWIFT t=210 s with 74 GB
resident per rank and no error of any kind: SWIFT simply goes silent for long
stretches inside `fof_find_foreign_links` / fragment linking, because that
phase has no periodic progress print. `STALL_TO` is now 3000/3600/4200 s by
node count. `BOOT_TO` is unchanged at 420/600/900 s -- the boot timeout is the
one that has to stay aggressive, since it catches a job that never forms
before it has burned real node-hours.

### The ArborX arm: the uniform brick grid had to go

Two failures, one cause.

* **cosmo25 (24B) could not run at all.** At 16/32/64/128 nodes the driver hit
  its own guard: `FATAL redistribute: this brick holds 2235713351 particles,
  past ArborX's int32 local indexing`. At 128 nodes the imbalance was 46.7 --
  one brick with 2.24e9 particles against a mean of 187e6.
* **romulus25 (58B) was not scale-invariant.** 29193922723 / 29193922691 /
  29193922688 components at 256/512/1024 ranks against FoF3's 29193922694,
  drifting in both directions. Every run converged (rounds 7, `g == 0`) and
  every `CENSUS_CHECK` passed, so the stitch was reaching a genuine fixed
  point -- the fixed point just was not the same one each time.

Three changes to `benchmarks/cosmo_fof/cosmo_fof_hacc.cpp`:

1. **Balanced rectilinear ("multi-jagged") partition.** px slabs in x, then py
   slabs in y *inside each x-slab*, then pz bricks in z inside each column,
   with every cut placed at an equal particle COUNT. The cuts come from a
   two-level global histogram over a strided sample (~64e6 particles globally),
   so the whole partition costs six small `MPI_Allreduce`s and moves no
   particles to compute. Every domain is still an axis-aligned box, which is
   the property that keeps the halo cheap. `min_width = min(4*eps, span/k)`
   stops a group being cut into slabs thinner than a few linking lengths,
   where the halo rather than the imbalance would dominate.

2. **Range-search halo instead of a 26-neighbour stencil.** With balanced cuts
   the bricks no longer line up face to face (the y cuts differ between
   x-slabs, the z cuts between columns), so "the 26 neighbours" is not the set
   of bricks a particle can reach. The new test walks the same cut arrays
   `slot()` uses for ownership, which is what guarantees a straddling pair is
   present on **both** of the bricks holding its ends. The common case still
   costs one comparison: a particle at least `heps` inside its own box exits
   immediately.

3. **The halo is widened by a relative 1e-6.** ArborX evaluates the squared
   distance in FLOAT, so a pair whose true separation is a few times 1e-7*eps
   above eps can still be linked. With a halo of exactly eps, such a pair is
   linked when both ends happen to land on one rank and MISSED when the
   partition splits them -- a partition-dependent answer, which is precisely
   the romulus25 drift. Widening only the halo (the linking test is still
   ArborX's, at exactly eps) makes every pair ArborX could link present on
   both sides at every rank count.

4. **The stitch keeps one label per COMPONENT, not per point.** Every point,
   own or halo, clustered or ArborX-noise, is given a component id; the state
   of the stitch is then a single `long long` per component, and lowering a
   component's label lowers it for every member at once. A round costs
   O(n_halo + n_hsend) -- a few million -- instead of O(n_loc). The per-point
   version performed 2.9e10 label writes on the 58B snapshot and spent 44 s in
   a phase whose actual communication is a few megabytes.

Gate: `scripts/test_hacc.sbatch` (job 5404948), 100k set, 2/4/8/16/27/32 ranks
x both formats x 2 reps against the known 33933 components / max_size 26042.

### Two more SWIFT ceilings, both found the same afternoon

`grid` cleared METIS, and immediately exposed the next two limits. They are
distinct and neither is the one the earlier `engine_maxproxies` patch
addressed.

**(a) The 64-proxy bitmask, at >= 256 ranks.** Job 5404838 (cosmo2b, 64 nodes,
256 ranks) died 2 s in with

```
engine_proxy.c:engine_add_proxy():159: Created more than 64 proxies.
    cell.mpi.sendto will overflow.
```

`cell.mpi.sendto` (src/cell.h:443) is an `unsigned long long` used as a
bitmask with `1ULL << proxy_id`, so **64 proxies per rank is a hard cap**
whatever `engine_maxproxies` says -- raising that macro from 64 to 1024
earlier in the campaign did nothing for this, because it guards a different
limit.

The reach is set by GRAVITY, not by FoF. `engine_makeproxies` computes
`delta_cells = (int)(2 * r_max / theta_crit / dmin) + 1`, with
`r_max = sqrt(3)*w` for a cubic cell of side w, so at `theta_cr = 0.7` SWIFT
looks for proxies **5 top-level cells away in every direction** -- an 11^3
neighbourhood -- even though FoF links only within one linking length, a tiny
fraction of a cell. Raising theta is not an escape: SWIFT hard-errors on
`theta_crit >= 1` (gravity_properties.c:147) and 0.999 still gives 4.

The number of RANKS in that reach is what has to be bounded, and it depends on
how many cells thick each rank's block is:

```
proxies ~ prod_d ( 2*ceil(delta_cells / (cdim/grid_d)) + 1 )
```

If every block is at least `delta_cells` cells thick the reach never crosses
more than one block per axis, giving 3^3 = 27 proxies at ANY rank count.
So `cdim >= 5 * max(grid_d)`. The old "64 top-level cells per rank" heuristic
satisfied that by luck up to 128 ranks and violated it at 256 (grid [8 8 4],
cdim 26, blocks 3.25 cells thick -> 5*5*3 = 75).

`run_swift.sbatch` now **pins** `DomainDecomposition:initial_grid` to the
most-cubic factorisation and derives `max_top_level_cells = 5 * max(grid_d)`:

| ranks | grid | cdim | block thickness | cells/rank |
|---|---|---|---|---|
| 16 | 4 2 2 | 20 | 5 10 10 | 500 |
| 32 | 4 4 2 | 20 | 5 5 10 | 250 |
| 64 | 4 4 4 | 20 | 5 5 5 | 125 |
| 128 | 8 4 4 | 40 | 5 10 10 | 500 |
| 256 | 8 8 4 | 40 | 5 5 10 | 250 |
| 512 | 8 8 8 | 40 | 5 5 5 | 125 |
| 1024 | 16 8 8 | 80 | 5 10 10 | 500 |

**(b) NIC memory registration on the 24B redistribute.** Job 5404839
(cosmo25, 32 nodes, 128 ranks) reached "Running FOF on ... 24461180928 DM
particles" and then died in `engine_do_redistribute` with a wall of
`cxil_map: write error` and

```
MPIDI_OFI_do_irecv(356): OFI tagged recv failed (No space left on device)
PMPI_Irecv(... count=19173961, dtype=USER<contig> ...)
```

19.2e6 gparts x 112 B = **2.1 GB in a single Irecv**, one per peer per
particle type, and the NIC ran out of memory-registration resources.
`run_swift.sbatch` now exports the same fabric settings the FoF3 arm uses
(`FI_CXI_RX_MATCH_MODE=hybrid`, `FI_MR_CACHE_MONITOR=userfaultfd`,
`FI_CXI_DEFAULT_CQ_SIZE=131072`), so both codes get the same fabric
configuration. Whether that alone is enough, or the 24B set simply needs more
ranks to make the messages smaller, is what job 5405083 tests.

Validation points in flight: 5405082 (cosmo2b, 64 nodes, 256 ranks -- the
proxy bound) and 5405083 (cosmo25, 64 nodes, 256 ranks -- the fabric limit).
Both run the production path, not a special probe configuration.

Confirmed meanwhile: cosmo2b at 4 nodes now completes (job 5404834,
fof_ms 327695.668, groups 1423075, max_size 185316849), so that point was
the stall watchdog and nothing else.

### CORRECTION: the >=128-rank SWIFT failures were the proxy cap, not METIS

The `grid`-vs-`memory` probe (5404566-68) was read as "METIS fails above 64
ranks, `grid` is the fix". That attribution is **wrong**, and the evidence was
in the probe logs the whole time. Job 5404567 (`memory`, serial METIS, 128
ranks) aborts with

```
[0064] [00005.7] engine_proxy.c:engine_add_proxy():159:
    Created more than 64 proxies. cell.mpi.sendto will overflow.
```

-- the same 64-proxy bitmask that later killed the `grid` run at 256 ranks,
not `Cannot bisect a graph with 0 vertices!`. (That METIS message was real,
but it belongs to the EARLIER round of failures at ~8 cells per rank, jobs
5404314-25, and was already fixed by raising cdim.) I had only read the tail
of the probe logs, which is a wall of `srun: error: ... Terminated` lines, and
did not check the first abort.

Why `grid` survived at 128 ranks and `memory` did not: proxy count scales as
(physical reach) / (physical domain size), and the reach is 5 cell widths.
`grid` gives every rank a compact axis-aligned block, so a compact domain;
METIS gives irregular domains with much larger surface area, which touch many
more ranks at the same cell count. Nothing about METIS's ability to partition
was involved.

**This matters for the numbers, not just the diagnosis.** `grid` ignores load
balance completely, and on clustered data that is expensive:

| nodes | ranks | decomposition | `Complete FOF search` | `fof_search_foreign_cells` |
|---|---|---|---|---|
| 8 | 32 | `memory` | 280.1 s | -- |
| 8 | 32 | `grid` | **923.1 s** | 727.9 s |
| 4 | 16 | `grid` | 327.7 s | 97.8 s |
| 32 | 128 | `grid` | 398.8 s | 248.2 s |

3.3x worse at the identical shape. The cost is almost entirely
`fof_search_foreign_cells` -- the cross-rank phase -- while
`fof_compute_local_sizes` is 58-293 ms throughout. Quoting SWIFT on `grid`
alone would understate it substantially, and a SWIFT user would run the
METIS-weighted default.

Since the true blocker was the proxy cap, and cdim has now nearly doubled at
128+ ranks under the `cdim >= 5*max(grid_d)` rule (21 -> 40 at 128 ranks),
`memory` may now clear the cap on its own -- the reach shrinks with cell size.
Job 5405175 tests exactly that: cosmo2b, 64 nodes, 256 ranks, `memory`,
cdim 40. If it passes, the whole SWIFT curve can run on SWIFT's own
decomposition and `grid` is dropped to a footnote.

### ArborX gate after the rewrite: 24/24 exact, imbalance 1.000

Job 5404948, 100k set, 2/4/8/16/27/32 ranks x NChilada and Tipsy x 2 reps,
against the known 33933 components / max_size 26042:

| ranks | components | max_size | imbalance (uniform grid) | imbalance (balanced) | halo |
|---|---|---|---|---|---|
| 2 | 33933 | 26042 | 1.037 | **1.000** | 2.49% |
| 4 | 33933 | 26042 | 1.620 | **1.000** | 10.72% |
| 8 | 33933 | 26042 | 2.916 | **1.000** | 19.43% |
| 16 | 33933 | 26042 | 6.363 | **1.000** | 31.51% |
| 27 | 33933 | 26042 | 23.915 | **1.001** | 39.33% |
| 32 | 33933 | 26042 | 11.518 | **1.001** | 44.38% |

24/24 PASS, both formats, both reps, every rank count. Load imbalance is gone:
23.9 -> 1.001 at 27 ranks. The halo fractions look alarming but are an artifact
of the check set -- 100k particles over 32 ranks makes each brick only a few
linking lengths across. On the production snapshots the halo was 0.18-0.35%
with the uniform grid and the balanced bricks are no smaller, since they are
the same count of bricks over the same volume.

Sweeps released: cosmo2b 4-64 nodes, cosmo25 16-128, romulus25 32-128
(jobs 5405181-92). The cosmo25 row is the one that could not be produced at
all before.

The 16-node `grid` point closes the argument: 1370.979 s, of which
1195.471 s is `fof_search_foreign_cells`, against 255.866 s for `memory` at
the identical shape -- 5.4x. The full `grid` line on cosmo2b is

| ranks | 16 | 32 | 64 | 128 |
|---|---|---|---|---|
| `grid` Complete FOF (s) | 327.7 | 923.1 | 1371.0 | 398.8 |

which is not a scaling curve at all, just noise from how a uniform geometric
split happens to cut the clusters -- the same pathology the ArborX arm had
with a uniform brick grid, and for the same reason. `grid` is therefore NOT a
usable configuration for the SWIFT arm at any rank count, only a fallback if
`memory` cannot be made to clear the proxy cap. Everything now rests on job
5405175 (`memory`, 256 ranks, cdim 40).

### The `${VAR:-default}` fabric trap, again

Job 5405082 printed

```
### fabric: rx_match=hybrid mr_monitor=kdreg2 cq=131072
```

`kdreg2`, not the `userfaultfd` the script asked for. Frontier's site modules
already export `FI_MR_CACHE_MONITOR`, so `${FI_MR_CACHE_MONITOR:-userfaultfd}`
never fires -- the identical failure mode already recorded in env/env.sh for
`MPICH_OFI_NIC_POLICY`, which cost two conversion jobs. All three fabric
variables are now set unconditionally with `CMP_`-prefixed overrides. The
`### fabric:` echo is what caught it, and is the reason every derived setting
in this campaign is echoed with its RESOLVED value rather than its intended
one.

Job 5405083 was cancelled and resubmitted as 5405398: Slurm copies the batch
script at SUBMIT time, so an already-queued job would have run the old,
no-op version.

### FoF3 CPU-only arm complete (cosmo2b)

Every point returns 424897832 / 185317566, identical to the GPU arm.

| nodes | GPU it0 (s) | CPU-only it0 (s) | ratio |
|---|---|---|---|
| 4 | 9.674 | 25.947 | 2.68 |
| 8 | 5.121 | 13.649 | 2.66 |
| 16 | 2.594 | 6.996 | 2.70 |
| 32 | 1.671 | 3.848 | 2.30 |
| 64 | 1.055 | 2.444 | 2.32 |

So the GPU is worth a consistent 2.3-2.7x, and the CPU-only arm is the number
to quote against SWIFT, which has no GPU FoF. Both arms use 56 cores/node
(FoF3 8 processes x ppn 7; SWIFT 4 ranks x 14 threads).

## Session 2026-09-02c: the balanced ArborX arm lands; SWIFT's real variable is cdim

### ArborX after the rewrite: cosmo2b exactly invariant, everything ~9x faster

cosmo2b, all five node counts, **424897833 / 185317566 at every one** -- the
same FMA-contraction value FoF3's own docs record, and now exactly
partition-independent where the uniform grid gave the right count only by
luck:

| nodes | ranks | imbalance (uniform) | imbalance (balanced) |
|---|---|---|---|
| 4 | 32 | 4.84 | **1.000** |
| 8 | 64 | 7.57 | **1.001** |
| 16 | 128 | 15.18 | **1.002** |
| 32 | 256 | 27.00 | **1.005** |
| 64 | 512 | 48.59 | **1.222** |

romulus25 (58B), uniform grid vs balanced, same node counts:

| ranks | imbalance | dbscan | stitch | solve | components |
|---|---|---|---|---|---|
| 256 (was) | 2.957 | 4.207 | 44.192 | 48.400 | 29193922723 |
| 256 (now) | **1.002** | 1.697 | **3.879** | **5.576** | 29193922689 |
| 512 (was) | 4.723 | 3.753 | 35.011 | 38.764 | 29193922691 |
| 512 (now) | **1.003** | 1.065 | **2.043** | **3.108** | 29193922688 |
| 1024 (was) | 5.043 | 2.109 | 16.249 | 18.358 | 29193922688 |
| 1024 (now) | **1.007** | 0.987 | **1.074** | **2.061** | 29193922695 |

A 9x faster solve at 1024 ranks, almost all of it from the per-component
stitch: 16.2 s -> 1.07 s.

### The residual romulus25 spread is ArborX's BVH, and it is ~1e-10

The counts still move: 29193922689 / 688 / 695 at 256/512/1024 ranks against
FoF3's 29193922694 (itself 694 at 32-256 nodes and 695 at 512). The spread is
down from 35 to 7, but it is not zero, and the widened halo cannot be the
remaining cause -- the halo is now provably complete. For a pair (p, q) with
|p-q| <= eps and p owned by A, q by B: dist(p, B's box) <= |p-q| <= eps <
heps, so p IS sent to B, and the pair is evaluated there. Symmetrically for q
on A. Every within-eps pair is co-resident somewhere at every rank count, and
the stitch's fixed point is provably the true connected components given the
edge set (all three runs converged at rounds 7, no cap hit, CENSUS_CHECK PASS).

What is left is inside ArborX's local kernel. FDBSCAN traverses a BVH, and an
internal node's box is the union of its children -- so the box depends on
WHICH points are on the rank, which is exactly what the partition changes. The
`distance(query, node_box) <= eps` test is evaluated in float, so a pair whose
separation sits within a few ULP of eps can be reached on one partition and
pruned on another. The leaf test is exact; the pruning test is not.

This is ~1.4e-10 relative on 2.9e10 components, of the same character as
FoF3's own 694/695 split, and it is a property of ArborX rather than of the
wrapper. Recorded, not chased -- cosmo2b is exactly invariant, which is the
control that shows the wrapper itself is deterministic.

### SWIFT: cdim, not the decomposition, was the dominant variable

Job 5405082 (cosmo2b, 64 nodes, 256 ranks, `grid`, **cdim 40**) came back at
**212.940 s** -- faster than every earlier SWIFT point at any node count,
including all the `memory` ones. The earlier `grid` line was run at cdim
16/16/16/21:

| ranks | 16 | 32 | 64 | 128 | 256 |
|---|---|---|---|---|---|
| cdim | 16 | 16 | 16 | 21 | **40** |
| `grid` Complete FOF (s) | 327.7 | 923.1 | 1371.0 | 398.8 | **212.9** |

so the "grid is 3-5x worse than memory" comparison was confounded: those
points differed in cdim as well as in decomposition. `fof_search_foreign_cells`
exchanges whole neighbouring top-level cells, and at cdim 16 each cell holds
2e9/4096 = 484k particles against 31k at cdim 40 -- a 15x difference in the
volume moved by the phase that dominates SWIFT's FoF. Both cosmo2b curves are
being re-run at the corrected cdim, `grid` (5405556-60) and `memory`
(5405561-64), so the two are finally compared at equal cell counts.

Settled separately: `memory` genuinely cannot reach 256 ranks. Job 5405175
(`memory`, 256 ranks, cdim 40) still hits `Created more than 64 proxies` --
METIS's irregular domains have too much surface area whatever the cell size.
Whether it clears 128 ranks at cdim 40 is what 5405560/5405564 answer.

### The same CXI limit killed the 24B points in BOTH codes

ArborX cosmo25 at 16/32/64/128 nodes: the partition is perfect (imbalance
1.002-1.010, 191e6 particles/brick against 2.24e9 before) and the whole solve
completes -- dbscan 237.6 s, stitch 5.0 s over 11 rounds at 128 ranks -- and
then the CENSUS dies with `cxil_map: write error`. Its `MPI_Alltoallv` moves
one long long per own particle: 191e6 * 8 = 1.5 GB per rank, registered on a
NIC that has just had DBSCAN's allocations through it.

SWIFT cosmo25 dies the same way in `engine_do_redistribute` (2.1 GB per
Irecv). Two independent codes, one machine limit.

Fixed in ArborX by (a) freeing every buffer the census does not read before
it starts, and (b) cutting each destination's segment into pieces so no
collective moves more than ~192 MB per rank. The census is not part of any
quoted timing, so the added latency costs nothing measured.

### FoF3 vs ArborX, and the inversion between datasets

Both arms now have complete cosmo2b and romulus25 rows. Seconds; "total" is
decomposition + solve, i.e. what one FoF run costs excluding the read.

cosmo2b (2B):

| nodes | FoF3 it0 | ArborX solve | FoF3 decomp | ArborX decomp | FoF3 total | ArborX total |
|---|---|---|---|---|---|---|
| 4 | **9.674** | 18.985 | 38.876 | 2.590 | 48.55 | **21.58** |
| 8 | **5.121** | 13.692 | 40.886 | 1.464 | 46.01 | **15.16** |
| 16 | **2.594** | 15.900 | 20.089 | 0.877 | 22.68 | **16.78** |
| 32 | **1.671** | 12.190 | 29.687 | 0.560 | 31.36 | **12.75** |
| 64 | **1.055** | 9.940 | 28.002 | 0.378 | 29.06 | **10.32** |

romulus25 (58B):

| nodes | FoF3 it0 | ArborX solve | FoF3 decomp | ArborX decomp | FoF3 total | ArborX total |
|---|---|---|---|---|---|---|
| 32 | 46.014 | **5.576** | 135.444 | 9.091 | 181.46 | **14.67** |
| 64 | 23.020 | **3.108** | 184.936 | 5.010 | 207.96 | **8.12** |
| 128 | 12.526 | **2.061** | 62.302 | 2.523 | 74.83 | **4.58** |

On group finding alone FoF3 wins cosmo2b by 5.8-9.4x and LOSES romulus25 by
6-8x. On decomposition+solve ArborX wins both.

Three things have to be stated with these numbers:

1. **The solve-only column is biased toward FoF3.** ArborX builds its BVH
   INSIDE the dbscan timing; FoF3 builds its tree during decomposition,
   outside Iteration 0. decomp+solve is the apples-to-apples comparison.

2. **FoF3's decomposition is the gap**: 20-185 s against ArborX's 0.4-9.1 s,
   and it does not scale down cleanly (romulus25: 135 s at 32 nodes, 185 s at
   64, 62 s at 128; cosmo25 at 512 nodes was 594 s). Amortised over many
   ParaTreet analyses that is a different argument, but a one-shot FoF pays
   it in full.

3. **The inversion is percolation.** cosmo2b's largest component is 9.35% of
   all particles (4.7 particles/component on average); romulus25's is 0.22%
   (2.0/component). ArborX's radius search pays for dense regions; FoF3's
   union-find handles the giant component well.

That last point also explains why ArborX's cosmo2b dbscan barely improves
with rank count -- 18.0 s at 32 ranks to 9.9 s at 512, over 16x the hardware.
The balanced partition equalises PARTICLES, but DBSCAN cost is in PAIRS, so
the brick holding the percolated structure sets the time no matter how many
ranks there are. Weighting the cuts by an estimated pair count would fix it
and would make ArborX's cosmo2b column BETTER than what is tabulated above,
so the current numbers under-report ArborX on the clustered set. That is the
honest direction of the residual error and must be said if these go to press
without the fix. It is deliberately NOT implemented yet: particle-count
balance is the standard, well-understood thing that any user would do
(Zoltan2 MultiJagged does exactly this), whereas pair-count weighting is a
contribution of this work rather than "how ArborX is used at scale", which is
what this arm claims to measure.

## Session 2026-09-02d: back to a HACC-faithful ArborX arm

Correction of scope, on the user's instruction: *"Be careful about optimizing
with the arborx driver, the point is to simulate what hacc would do as best as
possible."* The arm had drifted from measuring ArborX-as-HACC-uses-it toward
building the best possible distributed FoF around ArborX's kernel. Two planned
changes were dropped outright:

* **The pair-count-weighted partition.** That is a contribution of this work,
  not HACC's decomposition. It would have measured the wrapper.
* **Stripping ArborX's BVH build out of the reported time.** HACC calls
  `ArborX::dbscan` per invocation and that constructs a BVH every call -- it
  cannot be handed a prebuilt one -- so the build IS a per-FoF cost for this
  arm. The asymmetry against FoF3's Iteration 0 (which excludes tree build,
  done during decomposition) gets STATED, not engineered away.

`--partition uniform` is now the default and the primary configuration:
equal-width bricks, no sampling, no histograms, no communication, every cut
computable from the bounding box alone. That is HACC's decomposition.
`--partition balanced` remains as a clearly-labelled secondary, because it is
the only way the 24B snapshot runs at these rank counts and because the gap
between the two separates "ArborX is slow here" from "the decomposition is
imbalanced here". Both modes fill the same cut ARRAYS and share every line of
the ownership and halo code; only where the planes sit differs.

Note the earlier uniform-grid numbers are NOT a fair HACC baseline and are not
being reused: they ran with the old per-point stitch, which burned 44 s on
romulus25. HACC's own label exchange is not naive like that, so uniform is
re-run on the per-component stitch.

Gate 5405653 covers BOTH modes -- 48 runs (2 partitions x 6 rank counts x 2
formats x 2 reps) -- and all 48 are exact at 33933 / 26042. The uniform mode's
imbalance reproduces the original uniform-grid run exactly (1.037 / 1.620 /
2.916 / 6.363 / 23.915 / 11.518 at 2/4/8/16/27/32 ranks), confirming
`--partition uniform` is the decomposition this campaign started with.

Sweeps: both modes x cosmo2b 4-64, cosmo25 16-256, romulus25 32-256.

### Analysis framing, corrected

Also on the user's instruction: *"cluster finding generally runs as part of a
larger simulation. so what matters is fof time, not so much decomposition time
(which happens only once)."* The decomp+solve totals tabulated in the previous
section are therefore NOT the headline; group-finding time is. On that basis:

| dataset | nodes | FoF3 it0 | ArborX solve | winner |
|---|---|---|---|---|
| cosmo2b | 4 | 9.674 | 18.985 | FoF3 2.0x |
| cosmo2b | 64 | 1.055 | 9.940 | FoF3 9.4x |
| romulus25 | 32 | 46.014 | 5.576 | ArborX 8.3x |
| romulus25 | 128 | 12.526 | 2.061 | ArborX 6.1x |

with the scaling, not the ratio, being the real content: FoF3 goes 9.674 ->
1.055 s over 16x the nodes on cosmo2b (9.2x) while ArborX manages 1.9x. Those
ArborX numbers are from the BALANCED arm and will be restated from the
uniform one.

## Session 2026-09-02e: HACC-faithful results, and two corrections

### SWIFT: `grid` vs `memory` REVERSES at matched cdim

The earlier claim that `memory` (METIS) beats `grid` by 3.3x was a cdim
confound and is withdrawn. Re-run at equal cell counts:

| nodes | ranks | cdim | `grid` FoF (s) | `memory` FoF (s) |
|---|---|---|---|---|
| 4 | 16 | 20 | **306.5** | 3074.8 |
| 8 | 32 | 20 | **267.4** | 367.9 |
| 16 | 64 | 20 | **284.4** | 352.9 |
| 32 | 128 | 40 | 331.3 | **293.6** |
| 64 | 256 | 40 | **212.9** | FAIL (64-proxy cap) |

`grid` is better at three of the four points where both run, comparable at the
fourth, and it is the only one that reaches 256 ranks. It is therefore the
right choice for the SWIFT arm -- but for the OPPOSITE reason the earlier
entry gave. The 3074.8 s `memory` point at 4 nodes is a 10x outlier: METIS
produced a badly unbalanced partition there.

SWIFT's FoF does not scale on cosmo2b: 306.5 / 267.4 / 284.4 / 331.3 / 212.9 s
across 4/8/16/32/64 nodes, essentially flat, against FoF3 CPU-only's
25.9 -> 2.4 s over the same range.

### The 24B failure in SWIFT is a hardcoded 2 GB message, not the fabric

Neither `FI_MR_CACHE_MAX_COUNT=0` (job 5405617) nor 4x the ranks (5405618)
changed anything; both still died in `engine_do_redistribute`. The cause is
`engine_redistribute.c:71,150`:

```c
const int chunk = INT_MAX / sizeofparts;
```

sized so one message just fits in INT_MAX BYTES. With sizeof(gpart) = 112 that
is 19,174,318 particles = **2.1 GB in a single MPI_Irecv** -- and 19173961 is
exactly the count printed in the failure, which is what identified the line.
No CXI NIC will register that once the run holds tens of GB of particle data.

Capped at `SWIFT_REDIST_CHUNK_MB` (default 64 MB). The `while (activenodes)`
loop upstream already exists to run the exchange in rounds and already handles
being run repeatedly, so this only adds rounds. It is a portability fix, not
tuning, and redistribute belongs to engine_split, which this campaign reports
separately from "Complete FOF search".

Trap: `make -j16 fof_mpi` reports `'fof_mpi' is up to date` and never descends
into src/, so the first "successful" rebuild produced the ORIGINAL binary
(timestamp unchanged at 15:51). Always `touch` the changed source and run the
top-level `make`, then CHECK the timestamp and `strings` for the new symbol.
`engine_redistribute.c` also does not include <stdlib.h>, which getenv/atol
need.

### ArborX, HACC-faithful (uniform) vs balanced

FoF time (dbscan + stitch), seconds:

| dataset | nodes | FoF3 | ArborX uniform | ArborX balanced |
|---|---|---|---|---|
| cosmo2b | 4 | **9.674** | 29.117 | 18.119 |
| cosmo2b | 8 | **5.121** | 25.864 | 13.565 |
| cosmo2b | 16 | **2.594** | 27.684 | 15.758 |
| cosmo2b | 32 | **1.671** | 24.203 | 10.735 |
| cosmo2b | 64 | **1.055** | 24.301 | 10.942 |
| cosmo25 | 16 | **36.640** | FAIL | 238.449 |
| cosmo25 | 32 | **19.569** | FAIL | 168.762 |
| cosmo25 | 64 | **10.328** | FAIL | 124.506 |
| romulus25 | 32 | 46.014 | 14.659 | **5.863** |
| romulus25 | 64 | 23.020 | 12.351 | **3.226** |
| romulus25 | 128 | 12.526 | 6.262 | **2.078** |
| romulus25 | 256 | 8.322 | 5.483 | **1.275** |

In the HACC-faithful configuration ArborX does not scale on cosmo2b at all --
29.1 -> 24.3 s over 16x the nodes, because the uniform grid's imbalance grows
4.84 -> 48.59 and the worst brick sets the time. FoF3 scales 9.2x over the
same range. On romulus25 ArborX still wins but by 1.5-3.1x, not the 6-8x the
balanced arm suggested.

cosmo25 cannot be run in the HACC configuration at ANY node count tried
(16-256): imbalance 46.7 at 128 nodes and 93.3 at 256, and the worst brick
exceeds ArborX's int32 local indexing. That is a real property of uniform
overload decomposition at these rank counts; HACC avoids it by running at
~72k ranks where per-rank counts stay small.

Consistent across all three snapshots: ArborX degrades badly on percolated
data. cosmo2b and cosmo25 both hold ~9% of all particles in one component;
romulus25 holds 0.22%.

### Two component counts that must be reconciled before anything is quoted

* romulus25 balanced returned 29193922689 / 688 / 695 on one build and
  29193922688 uniformly on the next. The only change in that path was the
  census, so ONE OF THOSE BUILDS COUNTED WRONG and there is no confirmed
  explanation yet.
* cosmo25 balanced gives 6730729113 against FoF3's 6730729617 -- 504 apart,
  with max_size also differing (2214117497 vs 2214117459) -- far above the
  ~1e-10 float-boundary noise seen on the other two snapshots.

Rather than rationalise either, the driver now computes the component count a
SECOND, completely independent way, with no communication at all: a
component's label is the minimum gid it contains, exactly one rank owns that
particle, so counting own points where `label == gid` and summing is exact.
The two routes cross-check and a disagreement prints WARNING census. Gate
5406701: 48/48 exact, zero disagreements.

Job 5406704 measures d(components)/d(eps) on cosmo25 at +/-1e-7 and +/-1e-6
relative. ArborX evaluates the neighbour test in float, so the eps it applies
differs from FoF3's double by ~6e-8 relative; if the count moves ~500 per
1e-7, the 504 gap is entirely that conversion. If it is far less sensitive,
something real is wrong and the measurement will say so.

---

## Session 2026-09-02f — SWIFT redistribute cap verified; ArborX counts shown non-deterministic

### 1. SWIFT `engine_redistribute` 2 GB chunk cap: FIXED and answer-neutral

The patch (`SWIFT_REDIST_CHUNK_MB`, default 64 MB, applied to *both* chunk sites in
`swift/src/engine_redistribute.c`) is confirmed working. `engine_redistribute` now
completes at every scale tried, including the two that previously aborted:

| job | dataset | nodes x rpn | ranks | gparts moved | redistribute time |
|---|---|---|---|---|---|
| 5406730 | cosmo2b   | 8 x 4   | 32  | 1.98e9  / 100%   | (fast) |
| 5406731 | cosmo25   | 32 x 4  | 128 | 24.217e9 / 99.00% | 37.99 s |
| 5406732 | cosmo25   | 64 x 4  | 256 | 24.345e9 / 99.52% | 32.41 s |
| 5406733 | romulus25 | 64 x 4  | 256 | 57.765e9 / 99.62% | 13.61 s |
| 5406734 | romulus25 | 128 x 4 | 512 | 57.864e9 / 99.80% |  8.21 s |

**Regression check passed exactly.** cosmo2b @ 8 nodes, which ran fine before the
patch, returns bit-identical answers:

    VERDICT_SW dset=cosmo2b nodes=8 ranks=32 status=OK fof_ms=274566.494
               groups=1423075 max_size=185316849

`groups=1423075` is the same value as the pre-patch run. The chunking is a pure
transport change; it does not perturb the result.

**New capability: SWIFT now completes the 58B-particle dataset.** First successful
romulus25 run at any scale:

    VERDICT_SW dset=romulus25 nodes=128 ranks=512 status=OK fof_ms=361892.221
               groups=13692354 max_size=125859155

FoF-stage breakdown (job 5406734):

    fof_compute_local_sizes                    0.243 s
    fof_search_foreign_cells                  94.688 s
    fof_link_foreign_fragments                 3.310 s
    fof_assign_group_ids (local count)         0.179 s
    fof_assign_group_ids (global count)        1.862 s
    total fof_ms                             361.892 s

Note `groups` here counts only groups with >= min_group_size=32 members, so
13.69M is not comparable to the FoF3 gold 29193922694 (which counts every
component including singletons). `max_size` is comparable: SWIFT 125859155 vs
FoF3 125856955, a relative offset of 1.7e-5 -- larger than the ~4e-7 offset seen
on cosmo2b, still pending the `-ffp-contract=off -fno-fast-math` rebuild.

### 2. New SWIFT blocker at these scales: memory, not transport

Both cosmo25 points and romulus25 @ 64 nodes now die of OOM *after* a successful
redistribute, during `engine_rebuild` / task construction:

| job | dataset | ranks | mean gparts/rank | rank-0 gparts | outcome |
|---|---|---|---|---|---|
| 5406731 | cosmo25   | 128 | 191.1e6 |  71.1e6 | OOM (task 120) |
| 5406732 | cosmo25   | 256 |  95.6e6 |  24.7e6 | OOM (tasks 204-205) + PTLTE_NOT_FOUND |
| 5406733 | romulus25 | 256 | 226.5e6 | 159.7e6 | OOM (task 114) + PTLTE_NOT_FOUND |
| 5406734 | romulus25 | 512 | 113.2e6 |  66.4e6 | **OK** |

Rank 0 holds well under the mean in every case, so the OOM is decomposition
imbalance, not aggregate footprint. The tell is starkest on cosmo25 @ 128 ranks:
every rank owns exactly 500 of the 64000 top-level cells (perfectly even *cells*),
but rank 0 owns 0.37x the mean *particles*. `initial_type=grid` cuts equal volume,
and cosmo25 is the imbalanced dataset, so the heavy ranks carry a large multiple
of the mean.

This puts SWIFT in the same bind as ArborX-uniform, for the same reason:

* `initial_type=grid` respects the 64-proxy cap (blocks >= 5 cells thick) but
  cuts equal *volume*, so it OOMs on imbalanced data.
* `initial_type=memory` balances particle counts but produces thin/irregular
  rank blocks and hits the 64-proxy cap above ~128 ranks.

METIS is not a way out; it aborts with `Cannot bisect a graph with 0 vertices!`.

Two untried levers, in order of preference:
1. **Fewer ranks per node** (RPN=2/THREADS=28 -> 256 GB/rank, or RPN=1/THREADS=56
   -> 512 GB/rank). Costs nothing in proxies, since the pinned-grid rule
   `cdim >= 5*max(grid_d)` holds 27 proxies at any rank count. This is the way to
   get the *low* node-count points that the scaling curve needs.
2. **More nodes** at RPN=4. cosmo25 @ 128/256 nodes, romulus25 @ 256 nodes.

### 3. ArborX component counts are NOT run-to-run reproducible

Job 5406964 ran the *same binary* (md5 799d657607b3) twice, same nodes, same
partition, on cosmo25 @ 16 nodes / 128 ranks:

| | rep 1 | rep 2 |
|---|---|---|
| decomp_s | 8.905 | 8.933 |
| per-rank imbalance | 1.002 | 1.002 (identical min/max/mean) |
| dbscan_s | 244.152 | 244.693 |
| stitch_s | 5.238 | 5.262 |
| stitch rounds | 11 | 11 |
| component-min updates | 796682 | 796682 (identical) |
| CENSUS_CHECK | PASS 24461180928/24461180928 | PASS |
| **HACCCOUNT components** | **6730729115** | **6730729113** |

Everything upstream of the count is bit-identical -- the partition, the stitch
round count, even the exact number of component-min updates -- yet the component
totals differ by 2. Combined with the earlier build4/build5 values, the observed
spread on this fixed configuration is:

    6730729113, 6730729115, 6730729120   (spread 7, i.e. 1e-9 relative)

**This retracts the build-to-build conclusions.** I had read 6730729113 (build4)
vs 6730729120 (build5) as evidence that one census was buggy, and I read the
romulus25 balanced drift 689/688/695 -> 688 as a real effect of the stitch
rewrite. Neither is supported: differences of this size are run-to-run noise in
ArborX's DBSCAN (GPU union-find merge order under HIP atomics is the likely
source; the *set partition* should be order-invariant in principle, so the exact
mechanism is not established). No conclusion may be drawn from count differences
below ~10 components at these scales.

The noise is however two orders of magnitude smaller than the eps effect, so the
float-eps explanation of the 497-component cosmo25 gap stands unaffected:

    ArborX (this config)  6730729113-120   [noise +/- 7]
    FoF3 gold             6730729617
    gap                          ~497      ~= 5e-8 relative eps
    measured sensitivity    ~500-1100 components per 1e-7 relative eps
    float32 mantissa                        6e-8 relative

### 4. Why `HACCSTAT` was missing: the label-exchange census crashes

Not a grep miss, and not my probe script's fault after all. The full log shows the
exchange-based census aborting immediately after `HACCCOUNT` prints:

    HACCCOUNT components: 6730729115
    cxil_map: write error          (x ~40)
    MPICH ERROR [Rank 10] Abort ... Fatal error in PMPI_Irecv

So the throttled rotation schedule (KWAVE=32) still exhausts CXI memory
registration on cosmo25 @ 128 ranks. Consequences:

* `max_size` is unavailable for ArborX on cosmo25 -- only the component count is.
* The run exits **rc=143** even though the FoF result and all timings are already
  computed and printed. Any sweep row for cosmo25 marked FAIL on exit status may
  actually carry a valid count and valid timings; those logs need re-reading
  before the "cosmo25 uniform FAIL" entries are trusted.

`HACCCOUNT` itself is communication-free and unaffected -- it needs only the local
`cmin[comp_of[i]] == gid[i]` test plus one MPI_Reduce -- which is why it survives.
Fix options: drop KWAVE to 8, or compute max_size the same communication-free way
(per-component local sizes reduced by component representative) rather than by
exchanging labels.

### 5. State of the results table (FoF/group-finding time only, seconds)

| dataset | nodes | FoF3 GPU | FoF3 CPU | ArborX uniform | ArborX balanced | SWIFT |
|---|---|---|---|---|---|---|
| cosmo2b   |   4 |  9.674 | 25.947 | 29.117 | 18.119 | 306.5 |
| cosmo2b   |   8 |  5.121 | 13.649 | 25.864 | 13.565 | 274.6 |
| cosmo2b   |  16 |  2.594 |  6.996 | 27.684 | 15.758 | 284.4 |
| cosmo2b   |  32 |  1.671 |  3.848 | 24.203 | 10.735 | 331.3 |
| cosmo2b   |  64 |  1.055 |  2.444 | 24.301 | 10.942 | 212.9 |
| cosmo25   |  16 | 36.640 |        | FAIL   | 238.449 | -- |
| cosmo25   |  32 | 19.569 |        | FAIL   | 168.762 | OOM |
| cosmo25   |  64 | 10.328 |        | FAIL   | 124.506 | OOM |
| romulus25 |  32 | 46.014 |        | 14.659 |   5.863 | -- |
| romulus25 |  64 | 23.020 |        | 12.351 |   3.226 | OOM |
| romulus25 | 128 | 12.526 |        |  6.262 |   2.078 | **361.9** |
| romulus25 | 256 |  8.322 |        |  5.483 |   1.275 | -- |

(cosmo2b @ 8n SWIFT updated to 274.6 s from job 5406730, the post-patch rerun.)

`uniform` is the HACC-faithful ArborX configuration and is the primary ArborX
number; `balanced` is the multi-jagged variant, reported as an upper bound on what
better decomposition would buy ArborX's kernel.

### 6. Open items

1. Re-read the "cosmo25 uniform FAIL" sweep logs -- rc=143 from the census crash
   may be masking valid counts and timings (see section 4).
2. Make max_size communication-free so cosmo25 ArborX rows are complete.
3. SWIFT low-node points for cosmo25/romulus25 via RPN=2 / RPN=1.
4. SWIFT cosmo25 @ 128/256 nodes and romulus25 @ 256 nodes at RPN=4.
5. 512-node points for both ArborX and SWIFT (user asked 16-512 on the big sets).
6. SWIFT `-ffp-contract=off -fno-fast-math` rebuild to test the max_size offset
   (4e-7 on cosmo2b, 1.7e-5 on romulus25).
7. Parked by user: romulus25 512-node FoF3 +1 component.
