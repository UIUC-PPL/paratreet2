<!--
Provenance (2026-11-22): extracted from HBT-HERONS c728ee0 by reading the
source, for step 3 of design/subhalo-checks.md ("Step 3: tracking"). These
lines were checked against the source by hand: the host vote weight
(src/subhalo_tracking.cpp:313), DecideCentrals and its 0.8 mass cut
(l.772-805), KineticDistance with the Hubble term (l.38-44), the source cut
to floor(3 Nbound) (src/subhalo_unbind.cpp:843-853), a child's unbound
particles appended to its parent (l.823), the shuffle only with sampling on
(l.523-524), and the merger threshold of 2 (src/subhalo_merge.cpp:11). The
rest is to be checked line by line before each piece of step 3 is built
from it, as step 1 did for unbinding.
-->

# HBT-HERONS (c728ee0) sub-halo tracking rules, DM-only default build

Source root: `/lustre/orion/csc710/scratch/rrao/hbt-herons-src-2026-11-11/src/`.
All citations are relative to that root (`HBT.cpp`, `src/...`, `CMakeLists.txt`).
Nothing was modified.

Conventions used below:
- "track" = one `Subhalo_t` entry. It persists for ever once created; nothing deletes a track except a
  new candidate that fails to bind in its first output (section 8).
- "source" = the track's `Particles` vector. Its first `Nbound` entries are the bound set, ordered by
  binding energy (most bound first); the rest are "unbound but kept" particles.
- `SnapshotId` is the output's id (`SnapshotIdList[index]` if given, otherwise the index;
  `src/snapshot_number.h:56-75`).
- Defaults (`src/config_parser.h:98-172`): MinNumPartOfSub 20 (l.105), MinNumTracerPartOfSub 10 (l.106),
  NumTracerHostFinding 10 (l.107), NumTracersForDescendants 10 (l.108), MergeTrappedSubhalos true unless
  `NO_STRIPPING` (l.120-124), MajorProgenitorMassRatio 0.8 (l.125), BoundMassPrecision 0.995 (l.126),
  SourceSubRelaxFactor 3 (l.127), SubCoreSizeFactor 0 (l.128), SubCoreSizeMin 20 (l.129),
  MaxSampleSizeOfPotentialEstimate 1000 (l.133), RefineMostBoundParticle true (l.135),
  ParticleNullGroupId -1 (l.140, overwritten by the SWIFT/GADGET4 readers), ReassignParticles 0 under
  `DM_ONLY` (l.166-170).

---

## 0. Compile-time branches taken by the DM-only default build

CMake maps options to macros in `CMakeLists.txt:114-135`. The build in
`/lustre/orion/csc710/scratch/rrao/hbt-herons-2026-11-11/build.sh` uses `CMAKE_BUILD_TYPE=Release`
(`-O3 -DNDEBUG`, so every `assert` is compiled out), `HBT_DM_ONLY=ON`, `HBT_MEASURE_UNBINDING_TIME=ON`,
and the defaults for the rest.

| Macro | State | Effect on tracking |
|---|---|---|
| `DM_ONLY` | defined | `IsTracer()` always returns 1 and `DoNotSubsample()` always returns 0 (`src/snapshot.h:90-105`). So "the first k tracers" means "the first k particles" everywhere. `Nbound_tracers = Nbound` in the disruption test (`src/subhalo_unbind.cpp:631-633`). `CountParticleTypes` always sets `TracerIndex = 0` (`src/subhalo.cpp:389-392`). `KickNullParticles` is a no-op, so missing ids are never removed (`src/subhalo.cpp:397-398`, `src/halo.cpp:139-140`). `UNBIND_WITH_THERMAL_ENERGY` and `HAS_THERMAL_ENERGY` are `#undef`'d even though the CMake option is ON (`src/datatypes.h:11-14`). ReassignParticles defaults to 0, so `ReassignParticles()` returns at once (`src/subhalo_reassign_gas.cpp:70-74`). |
| `NDEBUG` | defined (Release) | No asserts. This matters in: MaskTopBottom duplicate top tracers (section 3), the MergeTo particle-copy branch (section 7), and missing particle ids (section 2). |
| `INCLUSIVE_MASS` | undefined (not a CMake option; only `#ifdef`s in the code) | Exclusive branch: `MaskSubhalos()` runs (`src/subhalo_tracking.cpp:865-867`). Recursive depth-first unbinding (`src/subhalo_unbind.cpp:869-912`, not the flat loop at 862-868). Unbound particles of children go to parents. `PurgeMostBoundParticles` is not called (`src/subhalo_tracking.cpp:1506-1508`). MergeTo copies particles (`src/subhalo_merge.cpp:188-217`). |
| `NO_STRIPPING` | undefined | `RemoveUnboundParticles` runs on every iteration (`src/subhalo_unbind.cpp:625-628`). MergeTrappedSubhalos = true. |
| `MEASURE_UNBINDING_TIME` | defined in this build | Adds timing members (`src/subhalo.h:115-130`) that are written to disk (`src/io/subhalo_io.cpp:81-93`). `StartTimer` resets `NumberUnbindingIterations` (`src/subhalo_unbind.cpp:986-997`). No tracking decision reads any of these, so tracking is unchanged. |
| `CHECK_TRACER_INDEX` | undefined | No `TracerId` member. |

---

## 1. State a track carries from one output to the next

### 1.1 Members read at the next output (in memory)

`Subhalo_t` is defined at `src/subhalo.h:26-219`. Constructor defaults are at l.132-154.

| Member | Read at next output by | Notes |
|---|---|---|
| `Particles` (ids, in order) | everything | After `TruncateSource` this is the bound set (sorted by binding energy) followed by unbound particles, total length `min(size, floor(3*Nbound))`. It is 0 for orphans (section 6.4). Only `Id` matters: other fields are refetched from the new snapshot (section 2). |
| `Nbound` | UpdateParticles (CoM over the first `Nbound`), CleanTracks (orphan test, `Nbound` decrement), AssignHosts (tracer scan over the first `Nbound`), StoreTracerIds (`Nbound>1`) | Bound count from the last unbinding; 0 for orphans. |
| `Mbound` | recomputed in UpdateParticles (`src/subhalo.cpp:127`, `436-441`) before use | Used in the mass ordering (CompareMass) and in DecideCentrals. |
| `TracerIndex` | `AverageCoordinates` (`src/subhalo.cpp:159-160`), `MergeTo` | Always 0 in DM-only. |
| `MostBoundParticleId` | orphan tracking (UpdateMostBoundPosition, AssignHosts), CompareMass tie-break | Set when resolved: `Particles[TracerIndex].Id` = most bound particle (`src/subhalo_unbind.cpp:775-776`). Set at merger (`src/subhalo_merge.cpp:224`). Unchanged on disruption. |
| `TrackId` | everywhere; CompareMass final tie-break | |
| `Rank` | used only as a flag "`Rank==0`" (head) by CleanTracks (`src/subhalo_tracking.cpp:1475`) and LevelUpDetachedSubhalos (l.1195) | 0 for the most massive member of each FoF group and for every hostless track (section 8). |
| `NestedSubhalos` | CleanTracks (tree), NestSubhalos, MaskSubhalos, RefineParticles, MergeRecursively | Stored as TrackIds between outputs (`GlobalizeTrackReferences`, `src/subhalo_tracking.cpp:1095-1106`). Converted back to local indices by `LocalizeNestedIds` (l.1043-1093). |
| `VmaxPhysical` | CompareMass second key (`src/subhalo_tracking.cpp:134-135`) at AssignHosts | |
| `SnapshotOfDeath` | `IsAlive()` (`src/subhalo.h:182-185`) in Unbind, SetMergerInformation, FindDescendants | |
| `SinkTrackId` / `SnapshotOfSink` | `IsTrapped()` (`src/subhalo.h:178-181`) in MergeRecursively; reset in Unbind | |
| `DescendantTrackId` | FindDescendants (assigned only while Null) | Reset to Null on resurrection. |
| `SnapshotOfBirth` | Mask skip test (`src/subhalo_tracking.cpp:1268`) | |
| `ComovingMostBoundPosition` / `PhysicalMostBoundVelocity` | overwritten in UpdateParticles / UpdateMostBoundPosition before use | Retained only if an orphan's particle is not found. |
| `ComovingAveragePosition` / `PhysicalAverageVelocity` | overwritten as above | Initial reference frame of unbinding and input to KineticDistance. |
| `LastMaxMass`, `SnapshotOfLastMaxMass`, `LastMaxVmaxPhysical`, `RmaxComovingOfLastMaxVmax`, `SnapshotOfLastMaxVmax`, `SnapshotOfLastIsolation`, `SnapshotOfLastSatellite` | running bookkeeping (section 8) | No tracking decision depends on them. |
| `HostHaloId`, `Depth`, `NestedParentTrackId` | not read by tracking at the next output | Recomputed: HostHaloId in AssignHosts, Depth in FillDepth, parent in SetNestedParentIds. |
| `CoreComovingPosition`, `CorePhysicalVelocity`, `CoreComovingSigmaR`, `CorePhysicalSigmaV` | not carried | Recomputed by every `Unbind` call (`src/subhalo_unbind.cpp:476, 780`) before any merger test uses them. They are not in the HDF5 type. |
| `ParticleBindingEnergies`, `ParticlePotentialEnergies` | not carried | Cleared at save (`src/io/subhalo_io.cpp:435, 450`). |

### 1.2 Disk vs memory

- `Save` writes two files per rank (`src/io/subhalo_io.cpp:272-289`):
  - `SubSnap_XXX.<rank>.hdf5`:
    - the `Subhalos` compound dataset with every member registered in `BuildHDFDataType` (l.13-100): TrackId, Nbound, Mbound, TracerIndex, HostHaloId (global), Rank, Depth, LastMaxMass, the SnapshotOf* fields, the profile, energy, angular momentum and shape fields, both positions and velocities, MostBoundParticleId, SinkTrackId, DescendantTrackId, NestedParentTrackId, plus the timing fields under MEASURE_UNBINDING_TIME;
    - `SubhaloParticles` = the first `Nbound` ids (l.456-472);
    - `NestedSubhalos` = TrackIds (l.417-422).
  - `SrcSnap_XXX.<rank>.hdf5`: `SrchaloParticles` = all ids in `Particles` (the truncated source), in order (l.500-518).
- `Load(..., SrcParticles)` at `HBT.cpp:61`:
  - returns with no tracks if `snapshot_start-1 < MinSnapshotIndex` (`src/io/subhalo_io.cpp:119-124`). This is the fresh-start case: every FoF group then spawns a new track.
  - otherwise each rank reads whole files (l.142-156) and loads:
    - the `Subhalos` compound into the struct (l.209-215);
    - `SrchaloParticles` ids into `Particles[j].Id` (l.231-246; only `Id` is set);
    - `NestedSubhalos` (TrackIds) (l.253-265);
    - the cosmology (l.199-203).
- Between outputs in one run, the in-memory vector is exactly what was written: the same ids and order, and the same struct fields. Exceptions:
  - non-HDF5 members (Core*, energies) are recomputed before use;
  - the C library `rand()` state, which drives `std::random_shuffle` (section 10), continues in memory but restarts on reload;
  - after a reload, the placement of tracks on ranks follows the files, not AssignHosts. A tree is never split, because each file holds one writing rank's tracks and is read whole by one rank.

---

## 2. UpdateParticles and UpdateMostBoundPosition (`HBT.cpp:90-92`)

### 2.1 UpdateParticles (`src/subhalo.cpp:110-128`)

1. `ExchangeSubHalos` (`src/subhalo.cpp:13-31`) sends every listed id to the rank that owns it (hash of id, `src/particle_exchanger.h:157-189`). There the id is looked up in the new snapshot (`QueryParticles`, l.192-227). A found entry is replaced by the full snapshot record `p = snap.Particles[index]` (l.208): position, velocity, mass, and `HostId` = the particle's FoF membership in this snapshot. The original per-track order is restored (`RestoreParticleOrder`, l.226, 243; `RestoreParticles`, l.247-261). **Particle order within each track is preserved.**
2. Missing ids: `GetIndices` sets `Id = NullParticleId (-1)`. Under DM_ONLY the code asserts the particle is not a tracer, which is always false (l.212-216), so a Debug build aborts. In Release the entry stays in the list with `Id=-1` and the stale position, velocity and mass it carried. `KickNullParticles` does not remove it (no-op under DM_ONLY). **The DM-only build assumes every particle id persists in every snapshot.**
3. Tracks are not moved between ranks here (l.25-30).
4. For each track with particles, `AverageCoordinates` (`src/subhalo.cpp:153-165`) sets:
   - `ComovingMostBoundPosition` and `PhysicalMostBoundVelocity` = the new position and velocity of `Particles[TracerIndex]` = `Particles[0]` (the previous most bound particle);
   - `ComovingAveragePosition` = mass-weighted mean position of `Particles[0..Nbound)`. Periodic wrapping is relative to `Particles[0]`; `Nbound` is the previous bound count (`src/snapshot.cpp:163-202`);
   - `PhysicalAverageVelocity` = mass-weighted mean velocity of the same particles (`src/snapshot.cpp:203-233`).

   These two "average" values are the initial reference frame for this output's unbinding and the input to DecideCentrals. They are computed before CleanTracks on purpose (`HBT.cpp:98-102`).
5. `CountParticles` (`src/subhalo.cpp:436-441`): `Mbound = sum of Particles[i].Mass` over `i<Nbound`, accumulated into a `float` in list order.

### 2.2 UpdateMostBoundPosition (`src/subhalo_update_mostbound.cpp:16-86`)

- It applies to every track with `Particles.size()==0` (orphans and sunk tracks). If `MostBoundParticleId == -1` the run aborts (l.34-39).
- The id is looked up in the new snapshot (l.40-66). If it is found, the code sets `ComovingMostBoundPosition`, `PhysicalMostBoundVelocity`, `ComovingAveragePosition` and `PhysicalAverageVelocity` all equal to that particle's position and velocity (l.71-85). If it is not found, the old values are kept (l.76).

---

## 3. CleanTracks (`HBT.cpp:103`; `src/subhalo_tracking.cpp:1462-1488`, masker l.1246-1440)

Purpose: remove ids that appear in more than one track of the same previous-output tree. Duplicates arise because a child passes all its unbound particles to its parent but also keeps up to `3*Nbound` of its list (section 6).

Rules:

1. **Trees processed**: every track with `Rank==0` and a non-empty `NestedSubhalos` (l.1475). After the previous UpdateTracks these are the roots of multi-member FoF groups. Each such root's NestedSubhalos holds every other head of that group (section 8), so a tree is one previous-output FoF group. Hostless tracks and single-member groups are not processed. Trees are disjoint and run in parallel. A separate exclusion set is used per tree (l.1480).
2. **Pass 1, MaskTopBottom** (l.1314-1367). Pre-order: node, then each entry of its `NestedSubhalos` in list order, recursively.
   - For each node, including orphans (which have no particles), scan `Particles` from index 0 and take the first `MinNumTracerPartOfSub` (=10) tracers (l.1322-1345). In DM-only these are `Particles[0..9]`.
   - Insert each one's id into the set and swap it to the front (`swap(Particles[i], Particles[tracer_counter++])`, l.1343). This is a no-op in DM-only.
   - Then `SetTracerIndex(0)` (l.1362).
   - If one of these ids is already in the set (the same particle is among the top 10 of two tracks in the tree), the insertion fails. The assert at l.1338 is compiled out, so **in Release both tracks keep it**. Pass 2 never touches positions 0-9.
3. **Pass 2, MaskBottomTop** (l.1371-1439). Post-order: for each node, first recurse into its nest list in order, then process the node itself, so the root comes last.
   - Skip nodes with `Nbound <= 1` (l.1380-1381). `Nbound` is the previous bound count, so this skips orphans.
   - For positions `>= 10` (l.1393-1396), keep a particle if its id is not yet in the set (inserting it); otherwise remove it (stable compaction, l.1397-1416).
   - Each removed particle at an original position `< Nbound` decrements `Nbound` (l.1407-1408, 1424).
4. **Resulting ownership rule** for an id held by several tracks of one tree:
   - (a) If it is among the first 10 of any track, every track that has it in its first 10 keeps it, and every other copy (at position >= 10) is removed.
   - (b) Otherwise the copy that survives is the one in the track visited first in post-order: the deepest, earliest-in-nest-order descendant. A parent loses every particle held by any of its descendants, and the root loses everything held by anyone else in the tree.
5. Not recomputed here: `Mbound`, `ComovingAveragePosition` and `PhysicalAverageVelocity`. Only `Nbound` is reduced.

---

## 4. AssignHosts and ConstrainToSingleHost

### 4.1 Host choice (`HBT.cpp:109`; `src/subhalo_tracking.cpp:668-696`)

1. `halo_snap.FillParticleHash()` maps particle id to the local halo index on this rank (`src/halo.cpp:84-123`). Membership comes from the halo catalogue's particle lists.
2. Tracer ids (`GetTracerIds`, l.230-258):
   - track with particles: the first `NumTracerHostFinding` (=10) tracers among `Particles[0..Nbound)`, using `Nbound` after CleanTracks. In DM-only these are `Particles[0..9]`, the ids reserved in CleanTracks pass 1. If fewer are available, the remaining slots are Null.
   - orphan (no particles): one tracer, `MostBoundParticleId` (l.238-242).
3. Per-tracer host (`GetTracerHosts`, l.263-293): the local halo index if the id is in a local FoF, else -1. The value is -1 if the id is in this rank's particle snapshot but in no local FoF, and -2 if it is in neither (l.280-289). A Null tracer slot stops the scan (l.276-277).
4. Local decision only if every tracer was found in a local FoF (`MakeDecision`, l.284-286, 401). Otherwise the track goes through the global path (`FindOtherHostsSafely`, l.615-663, `FindOtherHosts`, l.419-613):
   - every rank evaluates the tracers;
   - the results are combined with `MPI_Allreduce(MPI_MAXLOC)` on (host value, rank) (l.474-476). The largest value wins, so a FoF index (>=0) beats -1, which beats -2.
5. **Vote** (`DecideLocalHostId`, l.299-330 local, l.336-382 global):
   - candidate score = sum over tracer rankings `i = 0..9` of `1.0/(1+pow(float(i),0.5))` (l.313/350). The weight is computed in double and accumulated into a `float` map value. The weights are 1, 0.5, 0.4142, 0.3660, 0.3333, 0.3090, 0.2899, 0.2743, 0.2612, 0.25.
   - tracers with host -2 do not vote (l.309-310).
   - **-1 (hostless) is an ordinary candidate.**
   - the winner is the candidate with the largest score, compared with strict `>` starting from 0 (l.317-327). **Ties are resolved by `std::unordered_map` iteration order** (implementation-defined).
   - if nobody votes, the result is -2 (l.317).
6. A track whose global result is -2 (no tracer found anywhere) becomes hostless, `HostHaloId = -1` (`HandleTracerlessSubhalos`, l.701-725).
7. Placement: tracks are moved to the rank holding the host halo, and `HostHaloId` is set to the local halo index there (l.596-612).
   - For hostless winners, the target rank is that of the **last** hostless tracer in ranking order. The first-found branch at l.354-359 is overwritten by l.362, contrary to the comment.
   - For -2, the target is rank 0 (default `operator[]` value).
8. Membership table: `MemberTable.Build(nhalos, Subhalos, include_orphans=true)` (l.693, 203-215). Groups are formed by local `HostHaloId`, and group -1 holds the hostless tracks. Each group is sorted by `CompareMass` (l.117-144), which compares, all descending:
   1. `Mbound` (as recomputed in UpdateParticles);
   2. `VmaxPhysical` (from the previous output);
   3. `MostBoundParticleId`;
   4. `TrackId`.

   This is a strict total order.
9. Orphans take part exactly like other tracks, with their single tracer. An orphan whose particle is in no FoF becomes hostless.

### 4.2 StoreTracerIds (`HBT.cpp:118-119`; `src/merger_tree.cpp:17-27`)

- For every track with `Nbound > 1`, store `GetMostBoundTracerIds(10)` (`src/subhalo.cpp:462-488`): tracers among `Particles[0..Nbound)`, then non-tracers if needed. In DM-only this is `Particles[0..9]`.
- It runs before ConstrainToSingleHost, so it uses the post-CleanTracks list.

### 4.3 ConstrainToSingleHost (`HBT.cpp:125`; `src/subhalo_tracking.cpp:731-770`)

- It is applied to **every** track, including the ones that will become centrals and the orphans.
- Global host id = `Halos[HostHaloId].HaloId`, or `ParticleNullGroupId` if hostless (l.741-742).
- A particle is removed if `p.HostId != global host id` **and** `p.HostId != ParticleNullGroupId` (l.757-759). So a track in FoF X keeps its X particles and its particles in no FoF; a hostless track keeps only particles in no FoF.
- Removal is `std::remove_if`, which keeps the order of the survivors (l.763-766).
- Then `Nbound = Particles.size()` (l.769). From here until Unbind, `Nbound` means source size.
- For the chosen central this step has no lasting effect, because FeedCentrals replaces its list.
- `p.HostId` is the FoF id stored in the snapshot particle record. For SWIFT it is `FOFGroupIDs`, and "no FoF" is the header's `FOF:group_id_default` (`src/io/swiftsim_io/swiftsim_io.cpp:163-170, 434-446, 699`). GADGET4 and APOSTLE readers also set it. **The classic `gadget` snapshot reader never sets `Particle_t::HostId`** (see section 11).

---

## 5. PrepareCentrals (`src/subhalo_tracking.cpp:857-868`)

### 5.1 DecideCentrals (l.772-805)

For each FoF group with more than one member (l.780), using the sorted list from 4.1.8:

1. `MassLimit = Mbound(List[0]) * MajorProgenitorMassRatio` (=0.8) (l.783).
2. Candidates are the leading run `List[0..n_major)`: stop at the first member with `Mbound < MassLimit` (l.784-786).
3. If `n_major > 1`, pick the candidate with the minimum `KineticDistance` (l.787-799). Ties go to the earliest in list order, because the comparison is strict `dmin > d`. The winner is swapped into `List[0]` (l.800-801), and the old `List[0]` takes its slot.
4. If `n_major == 1`, or the group has a single member, `List[0]` is the central.
5. `KineticDistance` (l.38-44, `src/snapshot.h:135-148`) is computed as follows:
   - `dv = v_sub - v_halo + H(z)*a*NEAREST(x_sub - x_halo)`;
   - x and v are the track's `ComovingAveragePosition` and `PhysicalAverageVelocity` (section 2.1.4; for orphans, the most bound particle);
   - the halo's `ComovingAveragePosition` and `PhysicalAverageVelocity` are mass-weighted means over all FoF particles (`src/halo.cpp:53-57`). For the gadget path they are computed in `src/particle_exchanger.h:281-287`; for SWIFT, GADGET4 and APOSTLE in `src/io/comms/exchange_and_merge.cpp:229-232`;
   - the value returned is `VecNorm(dv)`, which is the **squared** magnitude (`src/mymath.h:21-22`). This does not change the argmin.
6. Orphans are members with `Mbound = 0`. They can be candidates only if `List[0]` also has `Mbound = 0`, that is, if every member is an orphan.

### 5.2 FeedCentrals (l.807-855)

- **Group with no members** (no track, not even an orphan, voted for it; l.825-839): a new track is created with:
  - `HostHaloId` = this halo;
  - average position and velocity = the halo's;
  - `Particles` = all FoF particles (swapped from the halo);
  - `Nbound` = its size;
  - `SnapshotOfBirth = SnapshotId`;
  - `TrackId` still Null, Rank 0, Depth 0, empty nest.

  Slots are taken with `Npro++` inside an `omp critical` (l.828-831), so the vector order of new tracks depends on thread timing.
- **Group with members** (l.840-853): `Particles` of `List[0]` (the chosen central) is swapped with the FoF particle list, and its own previous source is discarded. `Nbound` = FoF size. Its average position and velocity are **not** reset; they stay the CoM of its previous bound set.
- Consequence: **a FoF group whose only members are orphans does not get a new track.** The chosen orphan receives the FoF particles and can be resurrected under its old TrackId (section 6.4).
- FoF particle order: the swiftsim, gadget4 and apostle readers sort each FoF's particles by ascending id (`src/io/comms/exchange_and_merge.cpp:233-237`). The classic gadget path keeps the group-file order.

### 5.3 NestSubhalos (l.1108-1127) and MaskSubhalos (l.1442-1460)

1. `LocalizeNestedIds` (l.1043-1093) converts nest TrackIds to local indices. Entries not found on this rank are dropped from the list, and the rank that holds that track sets its `Rank = 0`. A child in the same FoF as its parent is always on the same rank, so this affects only children that are detached anyway (but see 10.3).
2. `LevelUpDetachedSubhalos` (l.1184-1218):
   - (a) record `IsHeadSub = (Rank==0)` using the old ranks (l.1193-1195). These are the previous roots, the hostless tracks, and tracks levelled up in step 1;
   - (b) set `Rank=0` for the current `List[0]` of every group and for every hostless track (l.1198-1210);
   - (c) for each recorded head, call `LevelUpDetachedMembers` recursively (l.1213-1216, 1220-1244). For each child in the node's nest list, in order:
     - if `child.HostHaloId != node.HostHaloId` or `child.Rank == 0`, the child is detached: `Rank=0` and it is removed from the list. The order of the remaining entries is kept;
     - the recursion continues into **every** child, detached or not.
3. Heads of a group = its members with `Rank==0`, in the group's sorted order after the DecideCentrals swap (l.1116-1126). So `heads[0]` = the chosen central C.
4. **Tree used for this output** (also used by RefineParticles):
   - root C;
   - C's children = C's carried, filtered nest list, followed by `heads[1..]`. The heads are appended temporarily (l.1455 and `src/subhalo_unbind.cpp:883`) and removed afterwards;
   - every other node keeps its carried, filtered nest list.

   The previous root, if it is in the group but was not chosen, keeps `Rank==0`. It becomes one of C's appended heads, with its remaining satellites below it.
5. **MaskSubhalos** (exclusive mass). For each group with members (groups from AssignHosts, so new tracks are excluded), one exclusion set is used, and `Mask(C)` runs (l.1257-1283):
   - post-order: each child in list order is processed fully before its parent, and C comes last;
   - skip a node if `Nbound <= 1` (here `Nbound` = source size after ConstrainToSingleHost, or FoF size for C) (l.1265-1266);
   - skip a node if `SnapshotOfBirth == SnapshotId` (l.1268-1269);
   - otherwise keep each particle whose id is not yet in the set (insert it), and remove the rest (stable compaction).

   **A particle in several sources stays with the first node in post-order.** C keeps only FoF particles not claimed by any track in its tree.

---

## 6. RefineParticles and the recursion (`src/subhalo_unbind.cpp:855-919`)

### 6.1 Order

1. **FoF groups** (l.870-886), each independent (OpenMP dynamic schedule if `ParallelizeHaloes`):
   - append `heads[1..]` to C's nest;
   - call `C.RecursiveUnbind`;
   - restore C's nest.
2. **`RecursiveUnbind(node)`** (l.810-841):
   1. for `i` in the node's `NestedSubhalos` order:
      1. `child.RecursiveUnbind()`;
      2. **immediately append `child.Particles[child.Nbound .. end)` to the end of the node's `Particles`** (l.823). This is everything the child did not bind after its final Unbind; for a disrupted child it is all its particles.
   2. `node.Unbind()` (l.827);
   3. `MergeRecursively(node subtree)` (l.832, section 7);
   4. if a resolved descendant merged, `node.Unbind()` once more (l.836-837).
3. Then **hostless tracks**: plain `Unbind`, with no recursion, no merger test, and no parent (l.888-896).
4. Then **new tracks** (indices >= the count of table members): plain `Unbind` (l.897-903).
5. Then `TruncateSource` for **all** tracks (l.904-911).

### 6.2 What each Unbind starts from

- **Starting set**: the whole `Particles` vector (`Nbound = Particles.size()`, l.515). A vector of size 0 is treated as an existing orphan and skipped (l.471-489; see 6.4).
- **Contents of the starting list**:
  - satellite: `[carried source, after CleanTracks, ConstrainToSingleHost and Mask, order kept][unbound blocks of its children, in nest order]`;
  - root C: `[FoF particles not claimed by its tree, FoF order][unbound blocks of its children]`;
  - second Unbind after a merger: `[bound, sorted][unbound][bound particles of the merged tracks, appended in MergeRecursively visiting order]`.
- **Initial reference frame**: the track's current `ComovingAveragePosition` and `PhysicalAverageVelocity` (`src/subhalo_unbind.cpp:510-511`):
  - satellites and C: CoM of the previous bound set at the new positions (section 2.1.4);
  - new tracks: the FoF CoM;
  - an orphan acting as central: its most bound particle;
  - second Unbind: the result of the first.
- Subsampling: if the starting size is greater than 1000, `PrepareParticlesForSubsampling` partitions (a no-op in DM-only) and then `std::random_shuffle`s the list (l.430-443, 523-524).

### 6.3 What the list looks like after Unbind, and source truncation

- **Resolved** (converged with `Nbound >= Nlast*0.995`, l.690). `Particles` is permuted to follow `Elist` (l.744-750):
  - `[0, Nbound)`: bound particles sorted by energy (l.710), with centre refinement of the leading part;
  - `[Nbound, end)`: unbound particles. The block unbound in the final iteration comes first, sorted ascending by energy (l.673). Blocks unbound in earlier iterations follow, latest first. This follows from the `stable_partition` at l.35-44, 626.
- **`TruncateSource`** (l.843-853): `Nsource = Nbound` if `Nbound <= 1`, else `(HBTInt)(Nbound * SourceSubRelaxFactor)` = `floor(3*Nbound)`, capped at `Particles.size()`; then `Particles.resize(Nsource)`. **The kept source is the first `Nsource` entries of that list:** all bound particles, then the leading unbound particles in the order above.
- The child's unbound particles were already appended to the parent (step 6.1.2) before truncation. So the particles at positions `[Nbound, 3*Nbound)` of a child are both in its next-output source and in its parent's list. This is the duplication that CleanTracks removes.
- Hostless tracks, roots and new tracks have no parent. Their unbound particles beyond `3*Nbound` are dropped.

### 6.4 Disruption and the orphan state

- **Test**, after each removal step: `Nbound < MinNumPartOfSub (20)` **or** `Nbound_tracers < MinNumTracerPartOfSub (10)` (l.647). In DM-only `Nbound_tracers = Nbound`, so the test is `Nbound < 20`.
- **On disruption** (l.653-669):
  - if `IsAlive()`, set `SnapshotOfDeath = SnapshotId`;
  - `ComovingAveragePosition := ComovingMostBoundPosition` and `PhysicalAverageVelocity := PhysicalMostBoundVelocity`. These are the position and velocity of the previous most bound particle at this output, from UpdateParticles, or from the first Unbind if this is the second one;
  - `Nbound = 0`, `Mbound = 0`;
  - `Particles` is left as it was (possibly shuffled), not permuted.
- **After the loop**:
  - `CountParticleTypes` sets `TracerIndex = 0` (l.771);
  - `MostBoundParticleId` is **not** updated because the track is no longer alive (l.775-776), so it keeps the id from the last output in which the track was resolved;
  - the core is set to the most bound position and velocity with zero dispersions (`src/subhalo_merge.cpp:98-105`).
- **Particles released**: all of them go to the parent's list through l.823. If there is no parent (root, hostless, new track), they go to nobody. `TruncateSource` then empties the list (`Nbound <= 1` gives `Nsource = 0`).
- **Fields kept**: TrackId, nest membership (it stays a node in the tree), SinkTrackId (Null unless sunk), DescendantTrackId, MostBoundParticleId, and the bookkeeping maxima.
- **Existing orphans** (empty list) at Unbind (l.471-489): `Nbound = 0`, `CountParticles`, core = most bound position, return. `SnapshotOfDeath` is untouched.
- **Resurrection**: a dead track that gets particles (in practice only as a FoF central, section 5.2) and converges gets `SnapshotOfDeath := Null` and `DescendantTrackId := Null`. If it was trapped, it also gets `SnapshotOfSink := Null` and `SinkTrackId := Null` (l.698-707). `SnapshotOfBirth` is unchanged.
- **New tracks** that disrupt are deleted in UpdateTracks (section 8).

---

## 7. Merging of trapped tracks (`src/subhalo_merge.cpp`)

- **When it runs**: right after each tree node R's first Unbind, from inside RecursiveUnbind (`src/subhalo_unbind.cpp:832`). Every node therefore tests its **whole subtree**, not only its direct children. It is not run for hostless tracks or new tracks, which have no subtree.
- **Skip**: if `R.Nbound <= 1` after its Unbind, no test is made (l.52-53).
- **Traversal** (l.57-86): post-order over R's subtree. For each child in nest order, recurse first, then test the node itself.
  - A node is tested if it is not R and not already trapped (`SinkTrackId == Null`, l.70).
  - The recursion also enters trapped nodes, so their descendants are still tested.
- **Criterion** (l.11, 15-26):
  - `d = |x_S - x_R|_periodic / sigmaR_R + |v_S - v_R| / sigmaV_R`;
  - merge if `d < 2`;
  - the velocity difference is a plain Euclidean difference of physical particle velocities, with no Hubble term (`src/mymath.h:224-247`).
- **Core quantities** (`GetCorePhaseSpaceProperties`, l.94-181; `GetCoreSize`, `src/subhalo.cpp:493-496`):
  - `CoreSize = max((HBTInt)(Nbound*SubCoreSizeFactor), SubCoreSizeMin) = max(0, 20) = 20`;
  - take the first `CoreSize` bound particles, tracers first then non-tracers (in DM-only, the 20 most bound in list order). If `Nbound < 20`, all bound particles are used;
  - `x` = mass-weighted mean comoving position (periodic, relative to `Particles[0]`);
  - `sigmaR` = sqrt of the sum over the three axes of the mass-weighted variance of position (a 3-D dispersion, not 1-D);
  - `v` and `sigmaV` are defined the same way from `PhysicalVelocity`;
  - for a track with `Nbound == 0` (orphan or just disrupted): `x`, `v` = most bound position and velocity, `sigma = 0`;
  - R's sigmas come from R's first Unbind. S's values come from S's latest Unbind in this output.
- **On a merge** (l.29-40, 74-84):
  - `SnapshotOfSink = SnapshotId`;
  - `SinkTrackId = R.TrackId`;
  - `SnapshotOfDeath = SnapshotId` if S was alive;
  - the flag "resolved merger" is set if `S.Nbound > 1` (l.80);
  - if MergeTrappedSubhalos is true (the default), `S.MergeTo(R)` (l.183-238):
    - orphans (`Nbound <= 1`) return at once and keep their fields;
    - otherwise, in Release (NDEBUG), `R.Particles.insert(end, S.Particles[0..Nbound))`: the bound set only, with no duplicate check (l.211-214). The unbound part already went to S's parent in 6.1.2;
    - S's `MostBoundParticleId` and position fields are set from `Particles[TracerIndex] = Particles[0]` (its current most bound particle); its average position and velocity are set equal to those;
    - S gets `Nbound = 0`, an empty `Particles`, and `Mbound = 0`.
- **Re-unbinding**: if at least one resolved track merged, R runs `Unbind` once more (`src/subhalo_unbind.cpp:836-837`). No further merger test follows. R's unbound particles after this second pass are what its parent receives.
- **Persistence**: a trapped track is never tested again (l.70) unless it is resurrected (6.4). Its descendants stay in its nest list.

---

## 8. UpdateTracks (`src/subhalo_tracking.cpp:1490-1535`)

1. **RemoveFakeTracks** (l.871-896):
   - new tracks with `Nbound > 1` are kept, in vector order; the others are deleted;
   - `MemberTable.Build(..., include_orphans=true)` is called again;
   - `NBirth` = number kept, `NFake` = number deleted.
2. **AssignNewTrackIds** (l.899-963):
   - `base` = total number of pre-existing tracks summed over ranks (l.902-903). Tracks are never deleted, so this is the next unused id;
   - the global FoF `HaloId` of each new track's host is gathered on rank 0;
   - new track k gets `TrackId = base + (rank of its host HaloId in ascending order among all new tracks' hosts)` (l.946-953);
   - each FoF group has at most one new track, so there are no ties;
   - TrackIds are therefore consecutive over the whole run and independent of decomposition.
3. **CalculateProfileProperties** for all tracks (l.1496-1498; `src/subhalo.cpp:176-244`):
   - if `Nbound <= 1`, all profile values are 0;
   - otherwise the centre is `ComovingMostBoundPosition`. Take the periodic radius of each bound particle, sort ascending, and accumulate the mass;
   - floor each radius at `GetCurrentSoftening(a)` (`src/config_parser.cpp:641-649`; `min(SofteningHalo, MaxPhysicalSofteningHalo/a)` if a maximum is set);
   - `v2 = Mcum/r`. `VmaxPhysical = sqrt(max v2 * G/a)`, using the first maximum. `RmaxComoving` is its radius;
   - `RHalfComoving = r[Nbound/2]` and `REncloseComoving = r[Nbound-1]`, both floored;
   - `BoundM200Crit` and `BoundR200CritComoving` are also computed;
   - `LastMaxVmaxPhysical`, `RmaxComovingOfLastMaxVmax` and `SnapshotOfLastMaxVmax` are updated if `Vmax >= LastMax`.

   `VmaxPhysical` feeds back as the second key of CompareMass.
4. **SortMemberLists** by CompareMass (l.1502), so `List[0]` = the most massive member by the new `Mbound`. **This becomes the output central (Rank 0). It need not be the DecideCentrals central C that received the FoF particles.**
5. **ExtendCentralNest** (l.1155-1182), for groups with more than one member. Let R be the new `List[0]` and heads be the list from 5.3.3:
   - if R has `Rank==0` (it is a head), append to R's nest every head except R;
   - otherwise set `R.Rank=0`, run `LevelUpDetachedMembers` on every head (this removes R from its former parent's list), and append all heads to R's nest.

   **Result: the nest tree carried to the next output is one tree per FoF group, rooted at the most massive member. The root's children are its own previous children followed by the other heads.**
6. **AssignRanks** (l.162-178):
   - hostless tracks get `Rank = 0`;
   - in each FoF group, `Rank` = position in the sorted list (0 = most massive). Orphans are included and usually come last.
7. **FillDepth** (l.1129-1153):
   - hostless tracks get `Depth = 0`;
   - otherwise the depth is counted recursively from `List[0]` along the nest lists (root 0, its children 1, and so on).
8. **UpdateTrack** (l.13-36):
   - Rank 0: `SnapshotOfLastIsolation := SnapshotId` only if it is not Null;
   - Rank > 0: `SnapshotOfLastSatellite := SnapshotId`;
   - `if Mbound >= LastMaxMass`, set `LastMaxMass := Mbound` and `SnapshotOfLastMaxMass := SnapshotId`.
9. `HostHaloId` is converted to the global HaloId, or -1 (l.1513-1517).
10. `GlobalizeTrackReferences`: nest entries become TrackIds (l.1095-1106).
11. `SetNestedParentIds` (l.1016-1041):
    - `NestedParentTrackId` = TrackId of the track whose nest list contains it, otherwise Null;
    - a nested track with `SnapshotOfLastIsolation == Null` gets the previous output's SnapshotId.
12. `CalculateShape` gives `InertialTensor` and `InertialTensorWeighted` (`src/subhalo.cpp:246-310`).
13. `ComovingAveragePosition` is wrapped into the box (l.1531-1533).

Of all of these, only `Rank` (as the head flag), `NestedSubhalos`, `VmaxPhysical` (tie-break), `TrackId` and `MostBoundParticleId` feed back into tracking.

---

## 9. FindDescendants (`src/merger_tree.cpp:29-235`)

- **Candidate pool**: the tracks stored in 4.2, those with `Nbound > 1` at the start of this output.
- **Counting**: each stored tracer id (10 per track) is searched across all ranks among the bound particles (`Particles[0..Nbound)`) of every track with `Nbound > 1` at the end of this output (l.57-121). Each hit adds one to the count of the TrackId that holds it. A particle bound to several tracks counts once for each.
- **Choice**: the descendant is the TrackId with the largest count. Ties go to the **smallest TrackId**, because `std::map` iterates in ascending order and the comparison is strict `>` (l.141-178). If nothing is found the result is -1.
- **Assignment**: written only if the progenitor is now not alive (disrupted or sunk in this output) and its `DescendantTrackId` is still Null (l.212-219). Alive tracks keep `DescendantTrackId = Null`.

---

## 10. Dependence on MPI ranks, threads, or record order

1. **`std::random_shuffle`** (`src/subhalo_unbind.cpp:440`) uses the unseeded C `rand()` state of each process (no `srand` in the source).
   - It is used for every Unbind call whose starting list has more than 1000 particles.
   - The random sequence a track receives therefore depends on how many earlier shuffles that process has made: over all groups, hostless tracks, new tracks, second Unbinds and earlier outputs.
   - With more than one OpenMP thread, the interleaving is also run-to-run nondeterministic.
   - A restart resets the state, so a restarted run differs from a continuous one.
2. **Host vote ties** (4.1.5) depend on `std::unordered_map` iteration order.
3. **Global host search keys by local halo index only** (`src/subhalo_tracking.cpp:336-382`).
   - Two tracers in different FoF groups that happen to have the same local index on different ranks are scored as one candidate.
   - The target rank is that of the last such tracer.
   - With one rank this cannot happen. With several ranks it can change the chosen host, so the result depends on the decomposition.
4. Placement of hostless tracks (last hostless tracer's rank) and of tracerless tracks (rank 0) affects only which process unbinds them, and through item 1 the shuffle.
5. **Vector order of new tracks** depends on thread timing (`omp critical`, l.828-831). TrackIds do not (they are sorted by HaloId), but the output record order and the shuffle order do.
6. **Halo centre rounding**: in the SWIFT, GADGET4 and APOSTLE path, `AverageCoordinates` runs before the id sort (`src/io/comms/exchange_and_merge.cpp:229-238`). The periodic origin (first particle) and the summation order therefore depend on how the FoF pieces arrived from the ranks. This gives last-bit differences in the halo CoM. That CoM is used in KineticDistance and as the initial frame for new tracks.
7. **Deterministic given the inputs**:
   - CompareMass order (total order ending in TrackId);
   - nest-list order (history-dependent but deterministic);
   - CleanTracks and Mask orderings;
   - TrackId assignment;
   - FindDescendants tie-breaks;
   - per-track float sums (list order).
8. OpenMP reductions affect only output-only quantities (for example `CalculateShape`) and anything inside Unbind that is already reproduced.
9. `SetNestedParentIds` is executed by every thread of the enclosing parallel region (no worksharing, `src/subhalo_tracking.cpp:1500-1521`). The race is benign: all threads write the same values.

---

## 11. Ambiguities and points not determined from the source

1. **Classic `gadget` format**: no reader assigns `Particle_t::HostId` (only `src/io/swiftsim_io`, `gadget4_io`, `apostle_io` do; `Particle_t()` has an empty constructor, `src/snapshot.h:76`). `RemoveOtherHostParticles` then compares indeterminate values. I could not determine what HBT-HERONS actually does with `SnapshotFormat gadget`. The rules in 4.3 hold for the SWIFT, GADGET4 and APOSTLE readers.
2. **Missing particle ids in DM-only**: a Release build silently keeps an entry with `Id=-1` and stale properties (section 2.1.2). Its downstream effect was not followed further. The code clearly assumes it never happens.
3. **Duplicate reserved tracers** (section 3, rule 2): in Release both tracks keep the id. Debug would abort. How often this happens is unknown.
4. **"Alive orphan"**: a resolved track whose entire source is removed by Mask reaches Unbind with an empty list. It takes the existing-orphan branch: `Nbound=0`, but `SnapshotOfDeath` is not set and `MostBoundParticleId` is unchanged. I found no guard against this. It needs all of its particles to be claimed earlier in post-order by tracks from another old tree.
5. **Tracks with exactly one source particle** are skipped by Mask (`Nbound <= 1` at l.1265 means source size there). Their particle is not inserted into the exclusion set, so the root can also hold it.
6. **Reachability**: RefineParticles unbinds only nodes reachable from C through nest lists. I argued that every group member is reachable (every non-head is in exactly one nest list of a same-host parent on the same rank), but the code does not check it. An unreachable member would get only `TruncateSource` in that output.
7. **Hostless particles** can sit in the sources of a hostless track and of a satellite in some FoF at once. Neither CleanTracks nor Mask removes such cross-tree duplicates. Hostless tracks are never merger-tested and never receive particles from anyone.
8. **Comment versus code** in the global vote: the comment says the most bound hostless tracer's rank; the code uses the last one's. Only placement is affected.
9. **Division by zero** in the merger test if `CoreComovingSigmaR` or `CorePhysicalSigmaV` of R is 0 (for example, all core particles coincident). The result is inf or NaN, so no merger. Not reachable with ordinary data.
10. The `CompareMass` sort inside RemoveFakeTracks reads `VmaxPhysical` of new tracks before it is initialised (the constructor does not set it, `src/subhalo.h:132-154`). That sort is redone after `CalculateProfileProperties`, so I believe it has no effect, but strictly it is a read of an indeterminate value.
11. I did not trace `GetIndices` / `MappedIndexTable_t` for duplicate particle ids in the input. The code assumes particle ids are unique.
