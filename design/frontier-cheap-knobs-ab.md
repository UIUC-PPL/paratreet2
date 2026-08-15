# Frontier instructions: two zero-code experiments (compiler flags, node-pool chunk size)

Written 2026-08-14 evening for the next Frontier session. Both are
BUILD/ENV only — no source changes. Together they decide whether two
larger pieces of work (SIMD, the per-piece tree arena) are worth
building at all.

## Background in one paragraph

The campaign's straggler now sits at phaseB_s max ~1.23-1.32 s against a
0.25 s granularity floor (4.9x), and section 34 places the residual in
the hot process's OWN local walk, untouched by every S3 change. Two
cheap hypotheses about that walk have never been tested.

## Experiment A — the binary has no SIMD at all

paratreet2's makefiles pass `-g -O3` (fof/, examples/fof3) and `-g
-Ofast` (src/) with NO `-march=` anywhere. Verified on Anvil
(2026-08-14): the control binary emits **zero** AVX/VEX instructions —
the whole thing is SSE2, two float lanes — while `-march=znver3` emits
926 ymm references and 71 FMAs. The hot phaseA/phaseB inner loops are
float distance tests (`periodicDistSq`), so this is potentially large,
or potentially zero if the loops do not vectorise (they read 12 bytes of
position out of ~100-byte AoS Particles, so a gather may block it).
No makefile edit is needed: all three makefiles append `$(MAKE_OPTS)`,
and build-stack.sh takes extra charmc options.

Frontier CPUs are AMD EPYC 7A53 (Trento, Zen 3), so `-march=znver3` is
the right target — but CHECK IT COMPILES under the Cray wrappers first;
fall back to `-march=native` and say which you used.

## Experiment B — local piece trees are contiguous only in 27 KB runs

`buildTree` is a single entry method with no yields, so a piece's nodes
are already bump-allocated in depth-first PREORDER with nothing
interleaved. But each pool chunk is its own `new char[]`, and
`config.pool_elem_size` is never assigned by any app nor registered as a
config field — so `std::max(config.pool_elem_size, 128)` has ALWAYS
resolved to the 128 floor: 27 KB per chunk, ~40 separately-malloc'd
blocks for a 29k-particle piece.

`PARATREET_POOL_ELEM_SIZE` (new, at the tip) overrides it. Sized to hold
a whole piece it yields fully contiguous preorder local trees with no
structural change. The binary prints the effective value once at
startup: `PARATREET pool_elem_size: N nodes (K KB/chunk)` — check it in
every arm's output, it is the proof the knob took.
Chunks are allocated whole, so large values waste up to one chunk per
lane per piece-tail: REPORT RSS (the `PARATREET vmhwm_mb` line) per arm.

## Build

`git fetch && git checkout phaseab-campaign && git pull` — land on
**b6d2e68** or later. Clean rebuild of the whole stack (src/ changed).
Three binaries against PRODUCTION charm, staged under distinct names,
each verified untraced (`nm -C <bin> | grep -ci TraceSummary` = 0):

| name | extra charmc opts |
|---|---|
| `FoF3.2b.base` | (none) |
| `FoF3.2b.march` | `-march=znver3` |
| `FoF3.2b.fast` | `-march=znver3 -Ofast` |

Sanity-check that the flags reached the compile (grep a compile line out
of the build log) and report `objdump -d <bin> | grep -c -E
"vfmadd|vmulps|vaddps|ymm"` for each — if march/fast show no increase
over base, that is itself the finding and experiment A is answered
before any run.

## Arms — one job, 2B/16 nodes, -u serial, interleaved

Base env on EVERY arm (the current best cell):
`FOF_STEALA=1 FOF_STEALA_GEO=1 FOF_PB_PARTS=16 FOF_PB_M2KEY=1
FOF_PHASEB_SLICE_MS=2 FOF_S3=1`
Standard srun/pemap/lci_ndevices idiom, dataset
`/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024`.

A) compiler, interleaved so drift cannot masquerade as effect:
   base, march, fast, base, march, fast
B) pool sweep, on whichever binary A shows fastest (state which):
   POOL=default(128), 4096, 16384, 65536, then default again as a
   drift control.

Eleven arms at ~20 s each is a few minutes of compute; size the walltime
from `sacct` on a recent job, not a guess.

## Readout

- EXACTNESS FIRST: `components: 424897832` every arm.
- `pool_elem_size:` line per arm (proof the knob took) and
  `PARATREET vmhwm_mb` (the memory cost of big chunks).
- `time_s: phase1_stages` — phaseA and phaseB are the numbers of
  interest; `balance:` phaseB_s min/avg/max is the straggler.
- Pre-traversal and Iteration 0 wall rows.
- ALSO, and valuable independently of both experiments: the per-process
  `FOF3STAT load_model:` lines from ONE baseline arm, verbatim, all 128.
  That is the piece-level cost-model calibration data
  (design/piece-load-model.md). Anvil's copy of it gave
  sqrt(pair) vs actual phaseB at r=+0.872; Frontier is where the
  straggler is genuinely extreme (proc 55 at 14.9x the median process),
  so the same regression there is the more informative one.

## Reporting

`~/software/reports/`, with commit hash, job id, exact srun line,
per-arm tables, the objdump counts, and anything anomalous. Relay files
have been `relay<N>.txt`; continue the numbering.

## Do NOT

- Do not run the piece-migration arms (`PARATREET_PREBUILD_LB`). That
  path exists at the tip but Anvil job 19932506 measured the GLOBAL
  balancer version as catastrophic (Iteration 0 4.3 s -> 20-30 s;
  GreedyRefineLB ignores locality and scattered pieces to 0-2012 per
  process). A targeted-shedding replacement is designed but not built.
- Do not push. Corrections go in the relay, as always.
