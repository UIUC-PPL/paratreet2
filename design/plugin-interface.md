# FoF3 as an in-situ plug-in (steps 1-3 of the ChaNGa port)

Status (2026-11-01): stage A done (99a1122, gate job 5821848); stage B done
(gate job 5822088: the stand-in host reproduces FoF3's label_digest
d5262332457c155e on lambb, device and CPU arms, over two consecutive
calls, no unlabeled particles). A hosted call at 2 nodes takes 2.1 s, of
which 1.38 s is decomposition (universe 0.14, splitter search 0.42, flush
0.81): stage C removes the splitter search and the flush.
Stage C done (gate job 5822288): at FoF3's exact linking length, the host's
own adaptive octree (3851 cells, count-balanced or scrambled placement)
reproduces the digest; the flush sends 299 (balanced) / 641 (scrambled) of
80.6M particles to another PE; decomposition 0.66 s instead of 1.29 s, call
2.03 s instead of 2.17 s. The rest of that flush is local message copies.

Plan written 2026-11-01. This is Track A of the journal plan: FoF3 runs
inside a host simulation (first host: ChaNGa) on the particles in its
memory, and returns a halo label per particle and a halo catalog, without
writing a snapshot. Steps 1-3 here are the paratreet2-only part. Step 4,
the ChaNGa-side hooks, waits on Kale's go-ahead and Tom Quinn's answers
(decomposition type, label destination, upstream or fork, launch shape).

## What the code does today (mapped 2026-11-01)

- Startup: paratreet's `MainChare` (src/Paratreet.C:23) runs in ANY
  program that links the module. Charm constructs every registered
  mainchare. It parses argv with paratreet's parser, creates the readonly
  groups (`readers`, `treespec`, `thread_state_holder`), the Driver and
  the cache, and the app main's `run()` ends in `CkExit`. Inside ChaNGa,
  that would parse ChaNGa's argv and exit the program.
- Configuration is a plain object broadcast by `Driver::init` to
  `TreeSpec` (not a readonly), so it can be set in code and re-broadcast.
- Input: Reader (group) reads a file slice, bbox reduction,
  `assignKeys`, `OctDecomposition::findSplitters` (histogram rounds),
  TreePieces on a `DecompArrayMap`, then ONE particle all-to-all
  (`Reader::flush` -> `TreePiece::receive`). `-S` removes the Partition
  array but not that all-to-all. `decompose()` for iteration > 0 skips
  the file load and expects particles already in the Readers: that is
  the hook for in-memory input.
- The tree machinery (canopy, starter pack, cache, fetch routing, the
  dual walk's source) treats a node key as a global identity. It assumes
  the TreePieces' cells tile ONE octree under key 1, with all 8 children
  present at every split.
- Phase 1 (CPU and GPU) uses only boxes and distances: any assignment of
  pieces to processes is correct.
- Link clashes with ChaNGa (nm, 2026-11-01): the `Writer` group (both
  define one) and `peanoKey` (shared global of the N-BodyShop `structures`
  library; each program's main defines it). Everything else (`TreePiece`,
  `Main`) clashes only in a source file that includes both codes'
  generated headers, which a thin plug-in header avoids. Both codes build
  the same `utility` commit (a97fe5d).

## Plan, in build order

Each stage ends with the 2-node gate (lambb.00500, exact in both
iterations) on the existing FoF3 binary, so the offline path never
regresses, plus that stage's own check.

### Stage A: decomposition-independent labels (step 3, part)

Re-encode the UF_2 vertex id as `(order << 20) | process` instead of
`(process << 43) | order`. The union-find boss is the minimum id and
the serial path unions by the minimum id, so the boss then carries the
group's minimum particle order, with no extra communication. Every
particle's final label decodes to the group's minimum particle ID,
the usual FoF convention. Touches `uf2EncodeTip`, `uf2LocationFromID`
and the hand-written decodes (FoFPhase1.h, FoFPhase3.h).

New always-on line `FOF3STAT label_digest`: an order-independent hash of
(order, min-ID label) over all particles. It is identical for any
decomposition, process count, CPU/GPU arm or input mode, so a single
number checks every later stage per particle, not just by component
count.

### Stage B: library entry and in-memory input, "copy-in" (step 1)

- Rename paratreet's `Writer` group (`ParticleWriter`).
- Hosted startup: the registered main can declare itself hosted. Then
  `MainChare` does not parse argv, and `run()` neither runs iterations nor
  exits. The groups and the Driver are still created at startup, as
  readonlies require.
- Plug-in API (`fof/FoFPlugin.h`, POD types only, no paratreet headers):
  per PE, `stage(particles, n)` copies (x, y, z, id[, mass, v]) into
  this PE's Reader. Particles carry an origin tag (PE, index) in the dead
  `Particle::vertex_id`, renamed `origin`. Then, on PE 0 in a threaded
  entry, `run(params)`: universe from a bbox reduction (or the host's),
  `decompose()` through the in-memory hook, the FoF3 iteration, and
  labels routed back to their origin PE. The host then reads one label
  per staged particle, in staging order.
- Per-call parameters (linking length, period, leaf size, GPU) update the
  configuration and re-broadcast it, so the host does not need to know
  them at startup.
- The FoF3 iteration (now in examples/fof3/FoF3.C) moves into a reusable
  function in fof/, which both FoF3 and the plug-in call.
- Contract with the host: call FoF at a quiescent point. FoF uses global
  `CkWaitQD`, which waits on the whole program.
- Test host `examples/fofhost/`: its own mainchare, reads a tipsy file
  into its own chare array with a deliberately different placement (a
  scrambled round-robin), calls the plug-in, checks `label_digest`
  against an offline FoF3 run of the same file.

### Stage C: host decomposition, "host cells" (step 2)

The host stages particles grouped into octree cells. Any SFC-range
decomposition can be cut into octree cells, and ChaNGa's `-D 1` Oct
decomposition is cells already. FoF3 then skips its splitter search and
its particle all-to-all:
- allgather of (cell key, count, process), sorted by key;
- verify the cells do not overlap and cover every particle;
- add empty sibling cells so every split has all 8 children (the
  canopy requires it);
- TreePieces on a map that keeps each cell on its host process;
- particles delivered from local staging only.

Everything above the TreePieces (canopy, cache, walk) is unchanged. The
test host cuts the universe into cells itself and places them on
processes in scrambled order. Check: same `label_digest`, and zero
particle bytes sent during decomposition.

A general forest mode, where each process holds an arbitrary particle set
with its own root, would mean replacing key-addressed cache and fetch
routing. It is out of scope; non-octree hosts use Stage B.

### Stage D: minimum group size and catalog (step 3)

Widen the routed size census from (label, count) to a record: count,
mass, mass-weighted position (unwrapped by minimum image against a
per-fragment reference), mass-weighted velocity, bounding box. It is
filled once per representative, untouched groups locally and touched
groups at the boss owner. A reverse route from the boss owner tells
contributing processes which labels are below the minimum size, so
those particles get label 0 (no group). Catalog rows (groups at or above
the minimum) are returned to the plug-in caller and printable by FoF3
(`-m` becomes a real filter, behind a flag so today's runs are
unchanged).

### Stage E: periodic boundaries on the GPU arm (step 3)

Thread the period into `Device::runPhase1` / `deviceStage0`. Add a
periodic `boxMinDist2` and minimum-image leaf and grid-residual
distances, disable the `boxMaxDist2` certificate under a period, and add
the CPU's "node extent > L/2: no grid" guard. Then remove the abort at
FoFPhase1.h:4914. Gate: `FOF_GPU_VERIFY=1` with `-P` on 100k-uniform
(98,264 components) and a periodic lambb run, CPU vs GPU `label_digest`.
Also add a `-P` case to `make test`, and fix the per-axis grid guard
(FoFPhase1.h:4359 tests only x).

### Stage F: slimmer particle record (step 3)

Measure first (bytes per particle FoF holds beyond the host's copy).
Candidates: the gravity/SPH fields FoF never reads (density, potential,
soft, acceleration, velocity_predicted, pressure_dVolume, u_predicted,
about 44 B of 120) under a build option, and freeing the 28 B per
particle of FoF phase-1 scratch as soon as labels are final.
