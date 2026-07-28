# Cached-particle slimming: per-application cached-copy types

**STATUS: IMPLEMENTED on main (2026-07-28, commits 757d795 probe +
917ea51 slimming + d5094af accounting; Kale approved proceeding without
waiting for the 16-node OOM data — "pure optimization without
downside"). Validated: fof1 exact, all fof3 full checks (incl. PBC),
8M stats bit-identical, annotate (4 configs incl. multi-process) and
searchAlgos UNCHANGED (framework purity), both runtimes; the
FOF3STAT memory_MB line now reports process RSS (works on reconverse)
and the cache line prints cached_particle_MB with the actual stored
type (8M laptop: 19.0 MB vs 88.9 full-particle). Separate-compilation
answer (Kale's question): no impact — the mechanism rides the existing
Data-template seam; libparatreet.a (Reader/Writer/Decomposition/
concrete Particle) is untouched, and unionfind never sees particles.**
Original motivation: the ~2B dataset running out of memory at 16
nodes. Companion to the wire-side
slimming already shipped (MultiData::pupRemoteParticle, commit f0acacc):
that cut what is SENT to 20 of ~112 bytes per remote particle; this cuts
what is STORED.

## The problem

CacheManager stores every cache-shipped remote particle as a full
`Particle` (~112 B): `addCacheHelper` reconstructs full particles from
the slim wire form because cached source-tree nodes' particle arrays are
typed `Particle`, the same type local nodes use. At 80M / 8 processes
the cache held 5.3M particle copies; amplification grows with process
count, and at ~2B the cached copies (plus the cached tree nodes) become
a first-order memory term. For FoF, 92 of those 112 bytes are never
read: the walk touches only `position` and `group_number`.

## Design constraint (the point of this document)

paratreet2 is a framework. Gravity, SPH, and collision read MORE of the
particle than FoF does (velocities, masses, per-species data), and an
application that declares nothing must keep exactly today's behavior.
The mechanism must therefore be **opt-in per application, invisible when
not used, and type-checked** — an app that opts into a slim cached type
and then reads an unshipped field should fail to compile, not corrupt.

## Proposed mechanism

Follow the pattern that worked for the wire side (SFINAE opt-in via the
Data type), extended to storage:

1. **Opt-in declaration.** An application's Data type may declare
   `using CachedParticle = <struct>;`. Absent (the default — detected
   with the same member-detection idiom as pupRemoteParticle), the
   framework uses `CachedParticle = Particle` and NOTHING below changes:
   identical layouts, identical code paths after inlining.

2. **Storage.** CacheManager's particle pool for cached (source-tree)
   nodes becomes `CachedParticle` storage. Subtree-owned local trees are
   untouched — they keep full `Particle` (they own the real data;
   phase 1, relabel, and I/O all operate there).

3. **Node access.** `SpatialNode<Data>` gains a parallel accessor for
   cached particles. The traversal delivers leaves to visitors as today;
   a visitor that runs against remote data reads particles through a
   `const CachedParticle*`. For non-opting apps that type IS `Particle`,
   so every existing visitor compiles unchanged. FoF's visitor already
   reads only the two fields its CachedParticle will carry.

4. **Fill path.** `addCacheHelper` unpacks the wire form directly into
   `CachedParticle` — for FoF this makes the wire struct and the cached
   struct the same 20-byte type, deleting the reconstruct-to-112B step
   (a small CPU win on the fetch path as a side effect).

5. **Contract enforcement.** The existing MultiData contract comment
   ("an opting-in app must never read unshipped fields from cached
   particles") becomes a compile-time property: unshipped fields do not
   exist on the cached type.

## What deliberately does NOT change

- Local (Subtree-owned) particle storage, ParticleMsg exchange, Readers,
  Writers, decomposition: full `Particle` everywhere. This is a cache
  change only.
- Non-opting applications: bit-for-bit identical behavior; the only cost
  is compile-time template resolution.
- The cached TREE-NODE structures (SpatialNode/Node bookkeeping) — a
  separate, smaller memory term; measure before touching.

## Verification plan

- Framework purity: gravity/annotate/searchAlgos build and run unchanged
  (no opt-in), byte-identical outputs.
- FoF: full laptop suite (fof1 exactness, full checks incl. PBC, 8M/16M
  stats determinism) on both runtimes; the cache line's pool_MB is the
  before/after observable, plus a VmRSS-based memory probe (needed
  anyway: CmiMemoryUsage returns 0 on reconverse).
- Anvil: one 4-node 80M run — components byte-identical, cache pool_MB
  reduced by ~size(cached_particles) x 92/112.

## Sizing note for ~2B

Cached particles are one term. The others to measure with the VmRSS
probe before concluding anything: owned particles (~112 B x N/process),
tree nodes (local + cached), the CacheManager pool's allocation policy
(pool_MB was ~1 GB/process at 80M — how does it scale?), transient
decomposition copies (Reader flush), and — before today's default flip —
htram per-destination buffers. The 2B failure report should record
WHERE the OOM strikes (read, flush, build, walk) before any of these is
declared the culprit. Also standing: Tipsy's int32 header caps input at
2^31 particles; beyond that the file format, not memory, is the wall.

## 2B result (2026-07-28, job 19549364 — the OOM retry, 3 days after the OOM)

16 nodes / 128 procs x 15 PEs, current main (agg-off + slim cache +
pool + probe), untraced: **1.98B particles end-to-end, twice, 33/30 s
wall, zero OOM.** RSS 10.6-12.6 GB/process (~92 GB/node of 257).
cached_particle_MB 5,195 (216.5M copies x 24 B; would be 24.2 GB at
full Particle — ~19 GB saved machine-wide). Components 424,897,832
(max 185.3M), identical across reps — the reference line for
cosmo25cmb.768g2_dm.001024. phaseB with the pool: 2.78 s vs 9.8 s
static in the OOM run. component_histogram: 0.609 s for 425M
components. Forward signals: phaseB_maxpair 0.575 s (the depth-3
split revisit trigger fires at 2B density); phaseA skew 2.8x
(particles/chare ~29k — the -G predictor's regime; worth the A/B).
The 8-node companion run did NOT fail on memory: it hit the LCI IBV
completion assert (charm-notes/reconverse-qd-latency.md addendum) —
a runtime robustness issue, identical binary succeeded at 16 nodes.
