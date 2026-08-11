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
cd ../../inputgen && make      # -> plummer, uniform, tipsyPlummer
```

Non-FoF examples (`examples/gravity` — monopole Barnes-Hut,
`examples/annotate`, `examples/searchAlgos`) need only `src/`.

Performance tracing is off by default and is a build-time choice
(`make PROJECTIONS=1` / `make SUMMARY=1`) — see
"[Tracing with Projections](#tracing-with-projections)" below.

`make test` in `examples/fof3` runs the standard 12-run small matrix
({100, 1k, 10k} x {+p1, +p2, 2 procs x 1 PE, 2 procs x 2 PEs}) against the
checked-in inputs; every run must print `FOF3 TEST PASSED`. Run it once on
the cluster before anything larger.

### Generating datasets

Generate `.dat` files and convert to tipsy (the reader consumes tipsy):

```sh
cd inputgen
./plummer 0 1000000 1m.dat            # Plummer model (clustered); arg 1 is
                                      # mode (0 = write), NOT a seed: the
                                      # internal RNG is fixed, so a given N
                                      # reproduces the same file everywhere
./uniform 42 1000000 1m-uniform.dat   # uniform unit box; arg 1 IS the seed
./tipsyPlummer 1m.dat 1m.tipsy        # .dat -> tipsy (works for either)
```

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

## Tracing with Projections

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
