# ParaTreeT2

A restructured tree-traversal library for particle computations in Charm++,
extracted from [ParaTreeT](https://github.com/paratreet/paratreet). Initial
driving application: parallel Friends-of-Friends (FoF) cluster finding for
astronomy data, per the design in `../fof_design_note.md` (to be imported here).

## Why a new library

Two constraints of the FoF design that ParaTreeT cannot satisfy without a break:

1. **Node `Data` recomputable after tree build.** ParaTreeT computes node
   payloads (leaf ctor + `operator+=` upward accumulation) only at build time.
   FoF needs a post-phase-1 upward pass to annotate `min_frag`/`max_frag` on
   every node, shipped with remote nodes by the cache.
2. **Visitors with mutable node-local state.** The FoF boundary walk's `open()`
   consults and updates a process-level SEEN table (per-fragment-pair state
   machine). ParaTreeT visitors are pure over `(source, target)`.

## Components carried from ParaTreeT (minimal edits)

- SMP-aware atomic treenode cache: `CacheManager` + `Node`/`FullNode`/node pools
  (`src/CacheManager.h`, `src/Node.h`, `src/Resumer.h`, `src/MultiData.h`)
- Reader pipeline: Tipsy/NChilada load, SFC key generation, sample sort,
  redistribution (`src/Reader.*`, `utility/` structures)
- Decomposition hierarchy (Oct / SFC / kd) and `TreeSpec`
- Tree build: `TreePiece` + `Modularization` strategies
- Traverser/Visitor framework (`src/Traverser.h`) — with the visitor contract
  widened as above

## Deliberately restructured

- `Data` gains an explicit "upward pass" API callable between traversals —
  **implemented**: `TreePiece::upwardPass(cb)` recomputes node Data bottom-up
  over the built tree and re-propagates to the TreeCanopy;
  `TreePiece::callPerLeafFn(fn, cb)` mutates the TreePiece-side particle copies
  (the ones the cache ships to traversals). Tested by `examples/annotate`.
  Contract: run mutation + upwardPass *before* the traversal's cache
  loading; cached node copies from earlier rounds are not invalidated.
- Visitor `open()` may consult mutable process-level state.
- The union-find coupling changes shape entirely: two-level UF. UF_1 is serial
  per-process over particles (freezes at end of phase 1); the existing
  distributed [unionfind](https://github.com/UIUC-PPL/unionfind) library becomes
  UF_2 over ~1000x fewer fragments, driven by an emitted merge-edge stream
  rather than calls from inside a traversal visitor.

## Layout (planned)

```
src/            core library -> libparatreet2.a
examples/fof/   FoF application (first client)
tests/          correctness tests vs serial FOF on small boxes
```

## Status

Phases 1 and 3 (v1 + step 3a) are implemented and validated; the working
harness is `examples/fof3` (see "Testing on a cluster" below). Design notes
live in `design/`; see `../prompt_log.md` for project history and
`../fof_design_note.md` for the algorithm design.

## Testing on a cluster

This section is the self-contained recipe for correctness/assessment runs of
the FoF harness (`examples/fof3`) on a parallel machine. It assumes a Linux
cluster with your own Charm++ build; datasets of 10M-100M+ particles are the
target regime.

### Prerequisites

- A Charm++ build (v7.0 or later; an SMP build such as
  `netlrts-linux-x86_64-smp`, `mpi-linux-x86_64-smp`, or `ucx-linux-x86_64-smp`
  is recommended — multi-PE processes exercise the intra-process phase-B path).
  Non-SMP builds also work; phase B is then a no-op and cross-PE merging all
  flows through phase 3.
- `export CHARM_HOME=/path/to/charm/<your-build>` (the Makefiles default to a
  sibling checkout that will not exist on your machine).
- The N-BodyShop `utility` submodule, configured and built:

  ```sh
  git submodule update --init
  cd utility/structures && ./configure && make    # builds libTipsy.a
  ```

- The distributed union-find (FoF's UF_2) is the sibling
  [UIUC-PPL/unionfind](https://github.com/UIUC-PPL/unionfind) library
  (branch `fof_with_aggregation`), checked out **next to** paratreet2
  (`../unionfind`), plus [htram](https://github.com/UIUC-PPL/htram) at
  `../htram`. These are needed only for the FoF applications: the core
  toolkit (`src/`) and non-FoF examples build without them. Build unionfind
  **AGGREGATION-off** (plain sends; htram is linked but dormant — turn it on
  later with `make` once perf data calls for it):

  ```sh
  cd ../htram && make                              # -> libhtram_group_unionfind.a
  cd ../unionfind/prefixLib && make                # -> libprefix.a
  cd ..     && make AGGREGATION= PROFILE=           # -> libunionFind.a (htram-off)
  ```

### Build

In order (the core library is application-free; the FoF chares live in the
`fof/` module, which links the sibling `unionfind` — see
`design/fof-module.md`):

```sh
cd src && make                 # -> libparatreet.a  (core toolkit, no FoF)
cd ../fof && make              # -> libfof.a        (FoF module; needs ../unionfind)
cd ../examples/fof3 && make    # -> FoF3
cd ../../inputgen && make      # -> plummer, uniform, tipsyPlummer,
                               #    tipsy2nchilada
```

Non-FoF examples (`examples/gravity` — monopole Barnes-Hut,
`examples/annotate`, `examples/searchAlgos`) need only `src/`.

Performance tracing is off by default and is a build-time choice
(`make PROJECTIONS=1` / `make SUMMARY=1`) — see
"[Tracing with Projections](#tracing-with-projections)" below.

The GPU arm is likewise off by default and is a build-time choice
(`make GPU=1`) — see "[Building the GPU arm](#building-the-gpu-arm)" below.
With `GPU` unset the compile and link lines are byte-identical to the CPU
build, so the CPU chain is never a "GPU build with the GPU turned off".

`make test` in `examples/fof3` runs the standard 12-run small matrix
({100, 1k, 10k} x {+p1, +p2, 2 procs x 1 PE, 2 procs x 2 PEs}) against the
checked-in inputs; every run must print `FOF3 TEST PASSED`. Run it once on
the cluster before anything larger.

### Building the GPU arm

Phase 1 has a device implementation (`fof/gpu/`, designed in
`design/phase1-gpu.md`) that replaces the intra-process phaseA + phaseB +
merge chain with one Kokkos pass over a flat tree. It is a separate build,
not a runtime switch alone: you need a HIP-enabled Charm++, a Kokkos
install, and `GPU=1` on the FoF makefiles.

**Extra prerequisites.**

- A **HIP-enabled Charm++**, i.e. a build made with the `amd` option word.
  The option word is part of the BUILD DIRECTORY NAME —
  `reconverse-linux-x86_64-amd`, not `reconverse-linux-x86_64` (the `cuda`
  equivalent on NVIDIA) — and `hapi.h` exists only in the suffixed one, so
  `CHARM_HOME` must carry the suffix too. The GPU arm needs HAPI for the PE→GPU mapping and the
  stream; `Makefile.common` detects such a build by the presence of
  `$(CHARM_HOME)/include/hapi.h`.
- **ROCm** (developed against 6.2.4) for `hipcc`. On Frontier `module load
  rocm/6.2.4` is a SILENT NO-OP in a non-interactive shell — it sets no
  `ROCM_PATH` and does not show up in `module list`, and the CMake builds
  below (and reconverse's own `find_package(hip REQUIRED CONFIG)`) then fail
  looking for HIP. Pass the path explicitly instead:
  `-DCMAKE_PREFIX_PATH=/opt/rocm-6.2.4`.
- **Kokkos**, built with the HIP backend for the target arch. **Check out a
  4.x release, not `master`**: master's `Kokkos_BitManipulation.hpp` needs
  C++20 while `fof/gpu/Makefile` compiles `-std=c++17`, which is ~20 errors
  that never mention the standard. 4.7.04 is what this was built against.
  What the Frontier install was configured with:

  ```sh
  git clone --branch 4.7.04 --depth 1 https://github.com/kokkos/kokkos.git
  cd kokkos && cmake -B build-hip \
    -DCMAKE_CXX_COMPILER=/opt/rocm-6.2.4/bin/hipcc \
    -DCMAKE_PREFIX_PATH=/opt/rocm-6.2.4 \
    -DCMAKE_BUILD_TYPE=Release \
    -DKokkos_ENABLE_HIP=ON -DKokkos_ENABLE_SERIAL=ON \
    -DKokkos_ARCH_AMD_GFX90A=ON \
    -DCMAKE_INSTALL_PREFIX=$HOME/kokkos
  cmake --build build-hip -j16 && cmake --install build-hip
  ```

  The host backend is Serial on purpose: the host side of this arm is Charm
  PEs, and an OpenMP pool underneath them would contend with the PE threads
  for the same cores. `KOKKOS_DIR` defaults to `$HOME/kokkos` in both
  `src/Makefile.common` and `fof/gpu/Makefile`.

**Build order.** The device library comes first, and it is built by `hipcc`
rather than `charmc` — no Charm header is on its include path and no Charm
library is linked, so a failure there is a Kokkos/HIP failure and nothing
else. Then rebuild `fof/` and the application **with `GPU=1`**:

```sh
cd fof/gpu && make                      # -> libfofdevice.a (hipcc)
make test                               # standalone gate; needs a GPU node
cd .. && make clean && make GPU=1       # -> libfof.a with the device arm
cd ../examples/fof3 && make clean && make GPU=1   # -> FoF3
```

`make clean` in between is not optional: `GPU=1` changes `-DFOF_GPU` on the
compile line, and the templated FoF chares live in headers, so a stale
object silently keeps the CPU-only instantiation. (`libfofdevice.a` is named
as a link prerequisite, so *that* archive is tracked and a rebuild of it
does relink the application.)

On NVIDIA, build the device library through the Kokkos nvcc wrapper:

```sh
cd fof/gpu && make HIPCC=$KOKKOS_DIR/bin/nvcc_wrapper ARCH_FLAG="-arch=sm_80"
```

**Selecting the arm at run time.** Two environment variables, both needed —
the build only makes the arm available, it does not turn it on:

| variable | effect |
| --- | --- |
| `PARATREET_DEVICE_TREE=1` | emit the flat per-TreePiece tree the device traverses. **Must be set**: the emit happens at TREE BUILD, so phase 1 cannot turn it on later. |
| `FOF_GPU_PHASE1=1` | *Replace* mode: the device answer is adopted. |
| `FOF_GPU_VERIFY=1` | *Verify* mode: the CPU chain also runs and every particle's label is compared; any disagreement aborts. Slower, and the right mode for a first run on new hardware. |

`FOF_GPU_PHASE1` set in a binary built without `GPU=1` aborts rather than
falling back to the CPU — a silent fallback turns "the GPU path regressed"
into "the GPU path was never on". Replace mode does not implement periodic
boundaries (`-P`); use verify mode there. Tuning knobs, all off/zero by
default: `FOF_GPU_ASYNC=1` (non-blocking launch), `FOF_GPU_RELEASE=1` (free
the pinned staging each iteration), `FOF_GPU_GRID=<occupancy>` (the dense-node
cell grid), `PARATREET_DEVICE_TREE_VERIFY=1` (check the flat tree against the
pointer tree).

**One process per GCD is an invariant, not a preference.** Two processes
sharing a GCD silently halves every measurement, so phase 1 refuses to run
in that shape. On Frontier, one process per GCD is 8 processes per node; a
Frontier GCD is half an MI250X, and `--gpus-per-node=8` exposes all eight.

If you launch **fewer** processes per node than there are visible GCDs, note
that HAPI sets its per-process device count to `visible_GCDs / processes_per_node`
and hands each process that many devices, while FoF initializes Kokkos on
exactly one of them (the home PE's). At 8 processes per node that quotient is
1 and the two agree. At 4 it is 2, and the process's PEs are round-robined
across two GCDs while Kokkos lives on one. Pin visibility to a single GCD per
process to keep the shapes in agreement — and pick the one that is NUMA-local
to the cores the process is pinned to:

```sh
# Frontier core -> nearest GCD: 0-7:4  8-15:5  16-23:2  24-31:3
#                               32-39:6 40-47:7 48-55:0  56-63:1
GCDS=(4 2 6 0)      # for 4 processes taking cores 1-15, 17-31, 33-47, 49-63
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
```

### Input formats

`-f <input>` takes either format; the Reader picks between them by whether the
path is a file or a directory (`src/Reader.C`, the same test ChaNGa uses):

- **Tipsy** — a single file. Its header stores the particle counts in 32-bit
  fields, so a Tipsy snapshot tops out near 2^31 particles.
- **NChilada** — a *directory* of per-attribute field files, which is the
  format to use past that limit:

  ```
  <dir>/description.xml           optional, purely descriptive
  <dir>/{gas,dark,star}/pos       required per family (also fixes its count)
  <dir>/{gas,dark,star}/mass      required per family
  <dir>/{gas,dark,star}/{vel,soft}  read when present, else defaulted to 0
                                    (and skipped entirely for an app that
                                     sets read_velocity_and_soft = false)
  ```

  Each field file is `FieldHeader | min | max | numParticles values`, XDR
  (big-endian) encoded; a family directory that is not there is simply an
  empty family, and a field whose min equals its max may omit the values.
  Any float or integer type code is accepted and converted. Particles are
  numbered gas, then dark, then star — the same global ordering Tipsy uses,
  so a snapshot converted between the two formats gives identical results.
  Only `pos`, `vel`, `mass` and `soft` are read; the other attributes a
  snapshot may carry (`GasDensity`, `timeform`, ...) have no home in
  ParaTreeT's `Particle` and are ignored.

  An app that does not use velocity or softening says so with
  `conf.read_velocity_and_soft = false` in `setDefaults` (FoF does), and the
  loader then never opens those two files even when the snapshot has them.
  Because NChilada gives every attribute its own file, that halves both the
  bytes read and the `open()` count per family — `pos` + `mass` is 16 B per
  particle against 32 B with `vel` and `soft`. The flag has no effect on the
  Tipsy loader, where all four are fields of the same packed struct and come
  for free; the two formats therefore still produce identical FoF output.
  Velocity and softening are left zeroed when not read, so the reported
  kinetic energy is zero — exactly as for a snapshot that omits the files.

### Generating datasets

Generate `.dat` files and convert to tipsy:

```sh
cd inputgen
./plummer 0 1000000 1m.dat            # Plummer model (clustered); arg 1 is
                                      # mode (0 = write), NOT a seed: the
                                      # internal RNG is fixed, so a given N
                                      # reproduces the same file everywhere
./uniform 42 1000000 1m-uniform.dat   # uniform unit box; arg 1 IS the seed
./tipsyPlummer 1m.dat 1m.tipsy        # .dat -> tipsy (works for either)
```

To get an NChilada test input, convert a tipsy one:

```sh
./tipsy2nchilada 1m.tipsy 1m.nchilada          # float32 fields
./tipsy2nchilada 1m.tipsy 1m-f64.nchilada double  # float64 pos/vel
```

The converter holds the whole snapshot in memory, which is fine for anything
Tipsy can express; it exists to build test inputs and to give the two reader
paths a common answer to agree on. `examples/fof3/scripts/run_fof3_nchilada_check.sbatch`
runs that comparison: the same 100k snapshot as Tipsy, as float32 NChilada and
as float64 NChilada must produce identical `FOF3STAT components` lines.

Suggested sizes: 1M and 8M for shakeout, then 32M, 64M, 100M+ as memory
allows. Generate both a Plummer and a uniform box at each size you assess:
Plummer stresses clustering/imbalance (it produces two giant components by
construction — the generator mirrors two offset half-models), uniform at the
default b factor is deep subcritical (almost all singletons) and stresses the
tree walk instead. Note the generators are serial and O(N); at 100M expect a
few minutes and ~3.2 GB per `.dat` (32 B/particle) plus ~3.6 GB per tipsy.

### Running

The app-specific flags (all other flags are the framework's; see
`src/Configuration.h`):

- `-b <factor>` — linking-length factor, default `0.2`;
  b = factor * (V/N)^(1/3) with V the bounding-box volume.
- `-c <mode>` — correctness-check mode:
  - `full`: gather all particles to PE 0 and compare the parallel partition
    against an exact serial grid-hash reference (exhaustive; the default
    behavior for small N).
  - `stats`: no gather, no serial reference — statistics only. The
    distributed checks stay on (the per-PE tip-sentinel check and the
    annotation-validity `CkEnforce` on every node the walk consults), and
    determinism is assessed by comparing the `FOF3STAT components` line
    across runs/configs (see below).
  - `auto` (default): `full` if N <= 20,000,000, else `stats` with a printed
    warning that full verification was skipped.
- `-u <impl>` — UF_2 (cross-process union-find) implementation: `dist`
  (default; distributed UnionFindLib) or `serial` (gather-to-one oracle,
  kept for A/B; requires `-w transposed`).
- Distribution: SINGLE distribution (no Partition array) is the default
  since 2026-08-04 — decomposition is ~25% faster at 80M (the partition
  creation and assignment passes vanish) with bit-identical output.
  `-w transposed` (and with it `-u serial`) needs the Partition array
  and automatically selects dual distribution, printing a note. `-S` is
  accepted as a no-op for compatibility.
- `-w <walk>` — phase-3 walk: `dual` (default; symmetric dual-tree
  traversal) or `transposed` (the original walk, kept permanently as the
  independent A/B oracle).
- `-m <size>` — minimum component size for REPORTING: when > 0, an extra
  `FOF3STAT surviving` line lists only components with size >= m (the
  full, unpruned components line always prints too). Reporting filter
  only; never changes the computed partition. Default 0.
- `-P <L>` — periodic boundary conditions: cubic box period L on all
  axes (minimum-image; requires b < L/2 and L >= the box extent).
  Default 0 = open boundaries.
- `-g` — compute and print the phase-1 fragments histogram
  (`FOF3STAT fragments` line). Off by default: it adds a full
  fragment-counting pass to an otherwise enumeration-free path.
- `-G <threshold>` — phase-1 per-chare grid: a chare whose density
  exceeds `threshold` expected particles per cell (cell side
  b/sqrt(6)) solves its internal linking with a cell grid (test-free
  same-cell and face-adjacent unions) instead of the tree walk.
  DEFAULT 4 (since 2026-08-04); `0` = off (the walk-only oracle for
  A/B). Measured: at 2B, `-G 4` cuts slowest-PE phaseA ~19-29%; at 80M
  it is a mild win at ~21k particles/chare, fading as particles per
  chare shrink with PE count (the grid accelerates only intra-chare
  linking). Output is bit-identical either way, so it is always safe
  to A/B; the effect is worth re-measuring when
  particles per chare (roughly N / (8 x total PEs)) is ~15k or more
  AND the dataset has dense cores. Thresholds 2 and 16 measured worse
  than 4.

Example run matrix per input (adapt launcher syntax to your Charm++ build):

```sh
# Single node, one SMP process, 8 worker PEs:
./FoF3 -f 8m.tipsy -d oct +p8

# Single node, 2 processes x 4 PEs (netlrts standalone):
./charmrun ++local ./FoF3 -f 8m.tipsy -d oct +p8 ++ppn 4

# Multi-node, netlrts with a nodelist (4 nodes x 8 PEs):
./charmrun +p32 ++ppn 8 ++nodelist nodelist ./FoF3 -f 32m.tipsy -d oct

# Multi-node under Slurm (mpi/ucx builds; one process per node, 8 PEs each):
srun -N 4 --ntasks-per-node=1 --cpus-per-task=9 ./FoF3 -f 32m.tipsy -d oct +ppn 8

# Force full verification above the auto gate (needs PE-0 memory; see caveats):
./FoF3 -f 32m.tipsy -d oct -c full +p8
```

Cross-process behavior only engages with >= 2 processes, so every input
should be run at (a) one process and (b) at least two different multi-process
configs. Keep `-d oct` (the FoF configuration; it is also the default).

### What to capture and send back

Save full stdout per run; the assessment data is the grep-able block:

```sh
grep -E "FOF3STAT|FOF3 TEST|FOF3 STATS" run.log
```

Specifically:

1. The complete `FOF3STAT` block of every run. It is self-describing: the
   `config` line records PEs, processes (`nodes`), N, b, decomposition, and
   check mode; then wall times per phase, counters/edge statistics,
   min/avg/max-over-PEs load-balance lines (`balance`), and memory
   (`memory_MB`).
2. Any failure output verbatim: `CkEnforce`/`CkAbort` messages, `FOF3
   MISMATCH`, or `FOF3 TEST FAILED` lines, with the run's config line.
3. The determinism check (this is the correctness signal in stats mode): for
   each input, the `FOF3STAT components` line from two runs under DIFFERENT
   configs (e.g. 1 proc x 8 PEs vs 4 procs x 2 PEs). The line — component
   count, max size, and full log2 histogram — must be bit-identical across
   configs of the same input. Note the `FOF3STAT fragments` line
   (phase-1 process-level tips) legitimately differs across process counts;
   only the `components` line is config-invariant.

### Known caveats

1. **Gather-to-one UF_2 placeholder.** Phase 3 gathers the deduplicated
   merge-edge stream to PE 0 and runs the second-level union-find serially
   there (`src/FoFPhase3.h`). Fine for correctness at these scales (edge
   counts are ~1000x smaller than N); it is a scaffold that step 4 replaces
   with a distributed UF_2. Expect the `uf2`/`edge_gather` times to grow with
   process count — that is the placeholder, not a defect.
2. **No periodic boundaries.** The walk and both serial references treat the
   box as open. Use the synthetic generators above for exact comparisons; a
   cosmological snapshot will produce answers that differ from any
   PBC-respecting FoF at the box faces.
3. **Full-verification auto-gate at 20M.** `-c auto` (the default) skips full
   verification above N = 20,000,000 because it gathers ~24 bytes/particle to
   PE 0 and runs the serial grid reference there (plus reference working
   memory of roughly the same order). Force it with `-c full` where PE 0's
   memory permits; otherwise rely on stats mode plus the cross-config
   determinism check.

## Runtime options reference (fof3)

Everything tunable at run time, in one place. Two kinds: **command-line
flags** (parsed by fof3's getopt; `-h`/any bad flag prints the same
list) and **environment knobs** (read once at first use; `FOF_*` are
FoF-specific, `PARATREET_*` are framework-level). Defaults are the
shipped, measured-best values — a plain
`./FoF3 -f <input> -d oct -u dist` is a correct, near-optimal CPU run;
the knobs exist for scale tuning, A/B oracles, and diagnostics.

### Command-line flags

| flag | default | meaning |
|---|---|---|
| `-b` | 0.2 | linking-length factor; b = factor·(V/N)^(1/3) |
| `-c` | auto | correctness check: `full` (O(N²) serial oracle), `stats`, `auto` (full below a size gate) |
| `-u` | dist | UF_2 backend: `dist` (distributed UnionFindLib), `serial` (gather every raw edge to PE 0), or `gather` (staged: per-process contraction retires same-process edges before shipping — design/staged-gather.md). **`dist` remains the shipping mode.** Measured at 2B/16 nodes (relay86, 2 reps, all three modes bitwise identical including every histogram bucket): dist 4486.6 ms, gather 4783.5 (+6.6%), serial 5975.4 (+33.2%). Contraction repairs 80% of serial's penalty by retiring 739k same-process edges of 1.23M — confirming the split's edge inflation as serial's cause — but gather still forfeits streaming's ~132 ms of walk-concurrent cascade and pays a 0.35 s serial finisher, and that finisher grows with process count. Keep `gather` as a validated instrument, not a candidate |
| `-E` | 16 | mid-walk edge-batch size streamed to UF_2 (overlaps phase-3 walk with union-find); `0` = classic post-walk injection (the no-overlap A/B oracle); large values silently never fire (per-PE yield is 266–924 at 2B scales) |
| `-G` | 4 | phaseA grid occupancy threshold (particles per b/√6 cell) above which a chare is solved by the cell grid instead of the tree walk; `0` = walk-only oracle |
| `-w` | dual | phase-3 walk: `dual` (requires `-u dist`) or `transposed` (original walk, A/B oracle) |
| `-m` | 0 | min component size for REPORTING only |
| `-P` | 0 | periodic box period L (cubic); 0 = open boundaries; requires b < L/2 |
| `-s` | 0 (unset = ship everything) | cap on tree-canopy entries broadcast in the starter pack. **Unset was the scaling blocker; capping it removes it.** The ship is O(P²) — one message per destination and the pack itself grows with P — 78.6% of `loadCache`, and `loadCache` alone is why 64→128 nodes bought nothing (relay92). **Measured at 2B/128 nodes (relay93/94, 32 arms all exact): unset 1852 ms → `-s 128` 1499.5 ms, and 128 nodes is 13.3% faster than 64 (1728.9) for the first time.** Interior optimum: 8192→1685, 2048→1553.7, 512→1540.8, **128→1499.5**, 1→1600.8 (+94 ms of it in the walk, spread 230 ms). The mechanism is not the predicted bytes-vs-fetches trade — cached leaves/particles are unchanged at every cap and each process needs only ~339 of the 34,835 canopy entries, **under 1%** — so capping declines to broadcast a payload 99% of recipients never read. Too small still costs: withholding the top levels turns a broadcast into a fan-in on the few chares that own them. Keys are prefix-coded and `sortStorage()` sorts by key, so a cap ships the *shallowest* N. Correctness never depends on it (exact locally at every cap down to `-s 1`). **Optimum measured only at 128 nodes / 2B; not yet swept at other scales, so the default is unchanged pending that.** design/allgather-design-notes.md |
| `-D` | 3 | cache share depth: levels of descendants (plus leaf particles) shipped with each node-request reply. **Swept at 2B/16 nodes (relay90, all arms exact): the default is already optimal.** D1 4635 ms, D2 4525.7, D3 4553.1, D4 4725.5 — D2/D3 statistically tied, D1 and D4 separated and worse. The invariant that explains it: the walk's *used* node count is ~9.6M at every depth (±1.5%) while total fetches vary 6.2×, so bundling trades wasted bytes against request count, and requests are the expensive term (18.2% of walk wall at 14.9 µs each vs 14.0% for processing replies). **`-D 2` is the memory/bytes choice**: same wall as D3 but half the bytes moved (4.4 GB vs 8.7 GB in the walk window) and ~130 MB/process less resident — untested at 64/128 nodes, where the binding constraint may differ |
| `-S` | off | single-distribution mode (no Partition array; requires dual walk) |
| `-C` | off | skip the post-run cache memory accounting (use in traced runs) |
| `-g` | off | phase-1 fragments histogram (diagnostic pass over all particles) |

Framework flags (`-f -n -p -l -d -t -i -s`) are listed by the usage
text; `-d oct` is the FoF configuration.

### Environment knobs — production tuning

| knob | default | meaning |
|---|---|---|
| `FOF_PE_SETS` | AUTO | PE-set split (§36/§38): sets per process; phaseB pairs crossing a set boundary are deferred to the phase-3 walk. AUTO = one set per PE (equals the measured 2B optimum, s=14 at ppn 14, on Frontier and Anvil: −15 to −16% Iter0 at 16 nodes, more at 64/128) — except under any GPU mode, where AUTO resolves to 1 (engine contract §3). `1` = off; explicit values win and are clamped to PEs/process. Effectively requires `-u dist` (serial stays exact but is a net loss at scale — the app warns). AUTO at shapes other than ppn 14 is on the measurement list |
| `FOF_PE_SETS_MODE` | 1 | rank→set mapping: 1 = round-robin (correct: scatters SFC-near pieces across sets, dropping the m2-heavy pairs, −96% phaseB), 0 = blocked (comparison arm; −3% phaseB only — §38 mechanism) |
| `FOF_PE_SETS_NODES` | all | comma-separated process list to split on (singular `_NODE` also accepted). **Mixed CPU/GPU jobs: list only the CPU processes** — a Replace-mode GPU process with the split active aborts by design (engine contract §3) |
| `FOF_STEALA` | 1 | phaseA claim pool: any PE claims any piece by CAS, own-first then nearest-centroid; flattens within-process phaseA skew (1.15–1.5 → ~1.05). `0` = static owner assignment (comparison arm) |
| `FOF_STEALA_GEO` | 1 | claim priority: 1 = nearest-centroid, 0 = scan-order (comparison arm) |
| `FOF_PB_PARTS` | 16 | KD partitioning of the phaseB pool into N spatial partitions (partition = natural GPU/batch unit; 16 = best 2B value). `0` = off. Rarely engages under the AUTO split (pool near-empty); matters when sets are reduced or scoped |
| `FOF_PB_M2KEY` | 1 | LPT-sort the phaseB pool by the m2 expected-pairs estimate |
| `FOF_PB_SPLIT` | 8 | adaptive tail split: split units costlier than N× the mean |
| `FOF_PB_MERGE` | off | two-round phaseB (B1/mid-merge/B2 over compressed tips) |
| `FOF_PHASEB_SLICE_MS` | 2 | phaseB drain slice deadline; the claim loop yields by self-send so the PE stays responsive. `0` = drain in one call (pre-campaign behavior); the 2 ms default is provisional (Kale, 2026-08-20) |
| `FOF_KEEPALIVE` | 1 | keep-alive ring: one raw-Converse message per process per period to its ring successor. Suppresses the LCI idle-stall on InfiniBand (Anvil); fabric-scoped comment in fof/FoF.C. `0` = off (reproduces the raw bug for LCI debugging) |
| `FOF_KEEPALIVE_MS` | 100 | ring period. 100 = workaround + gap-monitor tripwire; 10 = finer monitor sampling; 1000+ = probe mode (deliberately leaves quiet windows past the ~1 s stall onset — a measurement, no longer a workaround) |
| `FOF_PROCS_PER_PNODE` | 8 | processes per physical node (block structure for the probe and coordinator layouts) |
| `FOF_UF_SIZES` | 1 | union-find component-size maintenance. `0` = skip it entirely (the add_size message flow plus local merges). **CPU: established, ~−2% at 2B** (relay79 −1.9%, relay82 −2.4%, both within-job pairings with separated ranges). **GPU: direction only** — −1.8% pooled over 4 reps with overlapping ranges (relay83 §4; do not quote relay80's −2.6%). All arms EXACT everywhere; FoF3 never reads the sizes (max_size comes from the label histogram), and the traced pair shows the find cascade's drain itself shrinks (relay81). **Recommended `0` for all FoF3 runs** — free and exact on both arms. Library default stays 1 because unionFindLib is shared; `prune_components` aborts under 0. Absolute walls are job-dependent (±2.6% across jobs): quote within-job deltas, walls as ranges |
| `FOF_UF_SHORTCIRCUIT` | 0 | backward short-circuit: a find chain leaving a chare tells its sender to point at the continuation (monotone smaller-id guard, no epoch). Remote climb hops −32%, wall unchanged at 2B (relay78) — comparison arm, not a win |
| `FOF_WAVE` | 0 | union-find compression wave: 1 = direct parent rewrites (owner-side, strictly-smaller ancestors only), 2 = hedge mode (redundant `union(p,q)` instead of rewrites; validation arm, +12.7% at 2B). Alone, fires once at the fireUF2Edges barrier — measured useless there (relay74/75: the forest is shallow at that barrier under any `-E`). Experimental |
| `FOF_WAVE_MS` | 0 | with `FOF_WAVE` set: periodic wave passes every N ms from walk start until labeling. QD-safe (a settled forest sends nothing). **Parked — do not enable for timing**: benefit is capped at ~260 rewrites at any trigger point while each pass is a global sweep; measured +71% at 25 ms, +176% at 10 ms at 2B (relay77). Correct at all gates; kept as a validation mechanism only |

### Environment knobs — instruments (all report-only)

| knob | default | meaning |
|---|---|---|
| — | always on | `FOF3STAT keepalive_gaps`: per-process ring-message inter-arrival vs inter-send gap deviations (over 5/10/25/100 ms + max). Arrival jitter WITHOUT matching send jitter = delivery delay, no tracing needed. Compare within a job only |
| `FOF_STAGE_DUMP` | off | per-PE phaseA line: pieces held, self and cross seconds |
| `FOF_PROBE` / `FOF_PROBE_MS` | off / 25 | responsiveness probe: block coordinator pings every sibling; RTTs measure whether busy processes attend to messages |
| `PARATREET_LB_SELF_EXP` | 1.2 | self-term exponent of the printLoadModel proxy (`FOF3STAT load_model`) |

Walk-work counters (edges, leaf visits, prunes) are placement- and
race-dependent: they drift 0.1–0.3% between jobs at identical code —
regression signals **within** a job only (relay15).

### Environment knobs — A/B oracles and debug

`FOF_TIP_ANNOTATE=0` (disable frozen-tip uniformity shortcuts),
`FOF_GRID_ROOT_ONLY=1`, `FOF_POOL_DEPTH` (default 2),
`FOF_POOL_SPLIT_SIZE`, `FOF_SLICE_MIN_BYTES` (1 MB),
`FOF_EDGE_CHECK` / `FOF_EDGE_DUMP`, `FOF_WALK_QD`,
`PARATREET_FLUSH_WINDOW` (reader flush windowing),
`FOF_KEEPALIVE_VERBOSE=1` (ring tick diagnostics). Each is documented
at its `getenv` site; none belongs in a production run.

### Environment knobs — GPU arm (needs a `GPU=1` build; see above)

| knob | default | meaning |
|---|---|---|
| `PARATREET_DEVICE_TREE` | off | emit the flat device tree at tree build — **required** for any GPU mode (`_VERIFY=1` adds the device-side check) |
| `FOF_GPU_PHASE1` | off | Replace mode: phase 1 entirely on the device. A CPU-only binary refuses it (no silent fallback); a split-configured process refuses it (engine contract §3) |
| `FOF_GPU_VERIFY` | off | Verify mode: device runs alongside the CPU chain and mismatches abort (`FOF_GPU_STAGE0` is the historical spelling) |
| `FOF_GPU_ASYNC`, `FOF_GPU_GRID`, `FOF_GPU_WALK` | off/0 | launch and walk-shape controls (design/phase1-gpu.md) |
| `FOF_GPU_RELEASE`, `FOF_COUNT_VERIFY` | off | measurement-mode changers — do not enable in timed runs |
| `FOF_HELPER_CPUS` | derived | landing zone for ROCm/HIP helper threads (the affinity fix in fof/gpu/FoFDevice.cpp): unset = the pemap's SMT siblings, automatic and correct at ppn 7 (fix worth ~−30% on a production build, relay49 job 5320452; best GPU shape 2881.9 ms at 2B/16); at ppn 13/14 every sibling is a PE, so name the OS-reserved CCD-first cores `0,8,16,24,32,40,48,56` and add `--core-spec=0 --cpus-per-task=16` to Slurm. The fix DECLINES with a warning when no safe CPU exists — a silent decline never masquerades as success |
| `FOF_NO_AFFINITY_FIX` | off | `1` disables the helper-thread affinity fix (the A/B arm) |

### Recommended configurations (2026-08)

CPU cluster run — **the defaults ARE the recommended config** as of
2026-08-20 (split AUTO, STEALA on, PARTS 16, SLICE 2; the
§36/§38-validated settings; −15 to −30% Iter0 depending on scale):

```sh
./FoF3 -f <input> -d oct -u dist -c stats
# LEAF SIZE IS A FIRST-ORDER KNOB WITH OPPOSITE OPTIMA PER ARM
#   (relays 61-64, full sweeps, 2B/16): CPU-only optimum -l 32 — NOW
#   THE DEFAULT (was 12, which costs +11.5%; the GPU's 128 costs +47%
#   on the CPU chain); GPU optimum -l 128 (true interior minimum; 12
#   costs +28%, 384 costs +53%) — pass -l 128 explicitly on GPU runs.
# Frontier: +ppn 7 +pemap <nosmt map> -> 4785-4811 ms at 2B/16
#   (production build). The once-headline "ppn 7 beats ppn 14 by 17%"
#   was an artifact of measuring at Debug build + leaf 128: at the
#   shipping configuration the ppn effect is +0.8% (real, nearly
#   worthless) — choose ppn freely on CPU. (On the GPU arm ppn 7
#   remains decisive.) The relay48 SMT decomposition also inverts at
#   leaf 32 — do not quote it outside its operating point.
# A/B against the pre-campaign behaviour: FOF_PE_SETS=1 FOF_STEALA=0 \
#   FOF_PB_PARTS=0 FOF_PHASEB_SLICE_MS=0
```

GPU run (Frontier, device phase 1; `FOF_GPU_PHASE1` makes the split
default resolve to sets=1 automatically):

```sh
PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1 \
  ./FoF3 -f <input> -d oct -u dist -c stats -l 128
# +ppn 7 +lci_ndevices 7 +backend_poll_thread 1 — poll 1 is REQUIRED at
#  this shape: one PE per device, so stride 2 permanently silences the
#  odd devices and HANGS (perf-sweep-2026-08-21.md section 4); poll 1
#  costs nothing. (one thread per domain;
#  ndevices x processes/node must stay near 56 — 112 fails libfabric
#  memory registration). ppn 7 beats ppn 14 by ~27% with the helper-
#  thread affinity fix active (automatic at ppn 7; see FOF_HELPER_CPUS
#  in the knobs table and design/campaign-archive/
#  RECOMMENDATION-affinity-fix.md).
```

Mixed jobs: add `FOF_PE_SETS_NODES=<CPU process list>` so GPU
processes stay at sets=1.

## Tracing with Projections

**Timing-run rule (relay49, 2026-08-21): never link `PROJECTIONS=1`
into a binary used for timing.** The hooks sit in the message path and
cost **7.7% at 2B/16 even with tracing disabled at runtime**. Build a
separate traced binary when traces are wanted. Second build rule from
the same incident: charm/reconverse must be built `--with-production`
— a default (Debug) cmake build compiles the runtime with no
optimization at all and cost 15.6% on the same workload.


Charm++ ships two performance-tracing back ends, both **off by default** in
this tree. They are not runtime switches: tracing has to be *linked in*
(`charmc -tracemode <mode>` pulls in `lib/libtrace-<mode>.a`, which turns the
runtime's and the generated `.def.h` code's trace hooks from no-ops into real
instrumentation), so choosing a mode is a build-time decision.

| Build | charmc option | Cost | Output |
| --- | --- | --- | --- |
| `make PROJECTIONS=1` | `-tracemode projections` | high — every event logged | per-PE event logs for the Projections GUI |
| `make SUMMARY=1` | `-tracemode summary` | low — binned utilization only | small per-PE `.sum` profiles |

Use **projections** when you need to see individual entry-method executions,
message sends, and idle gaps on a timeline (finding a specific stall, e.g. in
the FoF phase-3 walk). Use **summary** when you only need utilization over
time — it writes a fixed number of time bins per PE instead of one record per
event, so it survives long runs and high PE counts where a full event log
would not.

### Building a traced binary

The knobs are defined once in `src/Makefile.common` and are honored by every
application (`examples/gravity`, `annotate`, `searchAlgos`, `fof1`, `fof3`):

```sh
cd examples/fof3
make PROJECTIONS=1              # full event log
make SUMMARY=1                  # utilization profile
make PROJECTIONS=1 SUMMARY=1    # both at once (charmc accepts both modes)
make                            # back to untraced
```

Because `-tracemode` affects only the final link, `libparatreet.a` and
`libfof.a` are unaffected — there is no need to rebuild `src/` or `fof/` when
switching modes, and no separate "tracing build" of the libraries. The mode
currently linked into a binary is recorded in a `.tracemode` stamp file next
to it, which is what makes a bare `make PROJECTIONS=1` over an existing build
relink instead of reporting "up to date"; an unchanged setting relinks
nothing. `make clean` removes the stamp.

Both variables follow the `AGGREGATION` convention: any non-empty value turns
the mode on, an empty one (`make PROJECTIONS=`) leaves it off.

### Running and collecting traces

A traced binary runs exactly like an untraced one and writes its logs into the
working directory at exit, named after the executable:

- projections — `FoF3.<pe>.log[.gz]` per PE, plus `FoF3.sts` (the symbol
  table the GUI needs) and `FoF3.projrc`
- summary — `FoF3.<pe>.sum` per PE (`.sumd` with `+sumDetail`), plus
  `FoF3.sum.sts`

Useful runtime flags (they exist only in a traced binary; see the Charm++
manual's Projections chapter for the full list):

- `+traceroot <dir>` — write logs to `<dir>` instead of the launch directory.
  Point this at parallel scratch for anything large; the per-PE log count
  scales with PE count.
- `+logsize <n>` — entries buffered per PE before a flush (default 1,000,000).
  Raising it trades memory for fewer mid-run flush perturbations.
- `+gz-trace` — gzip the projections logs as they are written.
- `+traceoff` — start with tracing disabled, so only regions explicitly
  re-enabled from the application are recorded.
- `+trace-subdirs <n>` — scatter the logs over `n` subdirectories, for
  filesystems that behave badly with thousands of files in one directory.
- `+sumDetail` (summary only) — also record per-entry-method time in each bin,
  written to `.sumd`; `+bincount <n>` sets the number of bins.

Load the resulting `.sts` in the Projections GUI (`charm/tools/projections`,
built separately — it is not part of the Charm++ build used here).

Delete collected logs with `make cleanp` in the application directory; plain
`make clean` deliberately leaves them alone so a rebuild never discards a
trace you have not looked at yet.

### Caveats

1. **Traced runs are not timing runs.** Instrumentation inflates the per-phase
   wall times in the `FOF3STAT` block, projections much more than summary.
   Take reported timings from an untraced build and use traces only to
   attribute time within a run.
2. **Projections log volume grows with events, not with wall time.** A
   fine-grained tree walk at high PE counts can produce gigabytes per run.
   Prefer summary for first-look scaling questions, then re-run the
   interesting configuration under projections, ideally at a smaller PE count
   or with `+gz-trace` and a `+traceroot` on scratch.
3. **Correctness output is unchanged.** Tracing does not alter the computed
   partition; a traced `make test` must still print `FOF3 TEST PASSED`.

## License and provenance

ParaTreeT2 is licensed under the Apache License 2.0 with LLVM Exceptions
(see `LICENSE`), the same license as Charm++.

This work is based on [ParaTreeT](https://github.com/paratreet/paratreet),
written primarily by **Joseph Hutter** with contributors at the Parallel
Programming Laboratory and collaborating institutions; ParaTreeT2 extends
and streamlines it. See `NOTICE` for full credits. The N-BodyShop
`utility` code (Tipsy/NChilada readers, SFC) retains its own license.
