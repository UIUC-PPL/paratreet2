# FoF3 vs ArborX vs SWIFT: comparison harness

The harness behind the SC26-workshop paper's comparison (Fig. 10 and the 24B
results), moved here on 2026-10-29 from the campaign's purgeable scratch
directory so the ArborX and SWIFT arms survive. The full record of what was run,
why, and what went wrong is `design/fof-comparison-campaign.md`; read its
FINAL RESULTS section first, since earlier tables were superseded.

**Raw output and exact binaries are NOT here.** Logs, run directories,
Projections traces, the FoF3 binaries that produced the published numbers and
the charm shared libraries they need are archived read-only in
`/ccs/proj/csc710/rrao/fof-sc26w-archive/` (its README maps each binary md5
to the result it produced).

## Layout

| path | what |
|---|---|
| `env/env.sh` | modules + `CMP` (the campaign working dir: `data/`, clones, builds, logs). Override with `CMP_DIR`. |
| `scripts/build_*.sh` | Kokkos 4.6.01 (HIP, gfx90a), ArborX, SWIFT (stand-alone FoF, and a double-precision test build), the campaign's charm/FoF3 builds |
| `scripts/run_fof3*.sbatch`, `run_arborx*.sbatch`, `run_swift.sbatch`, `submit_sweep.sh` | one point per job; `submit_sweep.sh` drives the sweeps |
| `scripts/collect.py`, `census_check.py`, `linklen.sh`, `sum*.py` | result collection, the component census, FoF3's exact linking length per dataset |
| `scripts/test_*.sbatch` | the ArborX debugging and gate jobs (campaign doc section 5) |
| `src/` | `cosmoio.hpp` (Tipsy/NChilada reader shared with the ArborX driver), converters (`nc2swift`, `to_arborx_bin`), `cosmoinfo`, boundary/slab checks |
| `arborx-mods/` | against ArborX `51923b4`: `arborx-tracked-changes.diff` (distributed-DBSCAN merge-pair race fix, three missing fences, the benchmarks CMake hook) and `cosmo_fof/`, our drivers |
| `swift-mods/` | against SWIFT `f219f0f`: `engine_redistribute.c` 2 GB chunk cap (`SWIFT_REDIST_CHUNK_MB`) and `swift_fof.c` driver changes |
| `ARBORX_BUG_REPORT.md` | the distributed-DBSCAN race write-up with reproducer; **not yet filed upstream** |
| `shim/h5cc` | HDF5 compiler shim for the SWIFT build |

## The ArborX arm in one paragraph

ArborX's distributed `Experimental::dbscan` was not used for timing: on this
data it returned wrong answers (a merge-pair race, fixed in the diff above, and
an over-merge its own verifier cannot see) and its global merge-pair set has an
int32 ceiling (~134M pairs) that rules out the 24B/58B snapshots.
`cosmo_fof/cosmo_fof_hacc.cpp` instead runs ArborX's single-rank `dbscan`
(minPts 2, `fdbscan`) per rank inside a HACC-shaped decomposition --
`--partition uniform` (HACC's equal-width bricks, the primary arm) or
`balanced` (equal-count bricks) -- with an eps-thick overload shell and a
per-component label-propagation stitch. FoF3's exact linking length is passed
as `-e`. 8 ranks/node, one per MI250X GCD, NUMA-local.

## To run it again

1. Build into a fresh `$CMP` (`CMP_DIR=...`): clone ArborX at `51923b4`,
   apply `arborx-mods/arborx-tracked-changes.diff`, copy `arborx-mods/cosmo_fof`
   to `benchmarks/cosmo_fof`, then `scripts/build_kokkos.sh` and
   `scripts/build_arborx.sh`; likewise SWIFT at `f219f0f` with
   `swift-mods/swift-tracked-changes.diff` and `scripts/build_swift.sh`.
2. Regenerate converted inputs under `$CMP/data` from the snapshots in
   `/lustre/orion/csc710/proj-shared/` with the `src/` tools.
3. The FoF3 scripts still name the paper-era charm builds
   (`reconverse-linux-x86_64-amd-cmp2`) and binaries; for new runs point them at
   the current build and `examples/fof3/FoF3`.
