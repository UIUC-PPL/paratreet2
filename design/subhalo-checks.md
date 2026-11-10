# Sub-halo finding: early checks

Started 2026-11-10. Records the checks that decide whether, and how, to add
sub-halo finding on top of FoF3. The direction and the reasons are in the
plan approved that day: a second application on FoF3, GPU unbinding as the
new kernel, history-based tracking (the HBT method) run in situ, HBT-HERONS
as reference. This note holds measurements and predictions only.

Terms: *unbinding* removes from a candidate set every particle whose kinetic
plus potential energy, in the set's own frame, is positive, and repeats until
the set stops changing. A *pair term* is one G m_i m_j / r evaluation.

## Check 0: how much unbinding work the data holds

No new runs on Frontier. Sources: `design/status-poolab-2026-07-27.md:238`
(lambb histogram), `design/campaign-archive/relay86.txt:13` (cosmo2b, largest
bins only), `design/fof3-bigscale-2026-08-23.md:92-93` (largest group of
cosmo25 and romulus25), `design/plugin-interface.md` (lambb catalogue count).

### lambb.00500 (80,621,568 particles, b = 0.2)

- 23,707,197 groups; 116,565 of them have 20 or more particles and hold
  46,851,449 particles (58% of the input). Largest group: 1,519,203.

Pair terms for ONE unbinding pass over every group of 20 or more particles
(group sizes taken at the mid-point of each log2 bin; lower is cheaper):

| Potential from | Pair terms per pass |
|---|---|
| at most 1,000 sampled sources per group (HBT-HERONS's default) | 4.1e10 |
| at most 100,000 sampled sources per group | 2.6e12 |
| all pairs, no sampling | 1.4e13 (48% of it in the 3 largest groups) |

### Largest group of each input

| Input | Particles in largest group | Pair terms per pass, 1,000 sources | 100,000 sources | All pairs | Memory at 36 B per particle (GiB) |
|---|---|---|---|---|---|
| lambb.00500 | 1,519,203 | 1.5e9 | 1.5e11 | 2.3e12 | 0.1 |
| romulus25 | 125,856,955 | 1.3e11 | 1.3e13 | 1.6e16 | 4.2 |
| cosmo2b | 185,317,566 | 1.9e11 | 1.9e13 | 3.4e16 | 6.2 |
| cosmo25 | 2,214,117,459 | 2.2e12 | 2.2e14 | 4.9e18 | 74.2 |

36 B = position, velocity, mass, ID in single precision with a 64-bit ID.

What this says:

- All-pairs unbinding of the largest groups is out of reach on any machine;
  those groups need sampling or a tree. Every group below about 1 M
  particles can be done exactly.
- cosmo25's largest group does not fit on one MI250X die (64 GB), so it has
  to stay spread over processes. The others fit on one die.
- Still missing: the middle of the cosmo2b histogram and all of cosmo25's.
  They are in the job logs on Frontier, so this needs a `grep` on a login
  node, not a job:

  ```sh
  grep -rh "FOF3STAT components" /ccs/proj/csc710/rrao/fof-sc26w-archive | sort -u
  ```

## The Mac as the no-cluster machine (verified 2026-11-10)

- Build: `examples/fof3/scripts/build_mac.sh` (CPU only, against
  `~/charm_reconverse`, Reconverse `darwin-arm8`). It rebuilds unionfind,
  `src`, `fof` and FoF3 with `SLIM_PARTICLE=1`, and uses the existing
  `~/htram/libhtram_group_unionfind.a` without touching htram.
- Tests: 6 single-process runs (`+p1`, `+p2`, `+p4` on the 100, 1k and 10k
  inputs) and 3 multi-process runs (2 and 4 processes) all print
  `FOF3 TEST PASSED` with identical component lines. `make test` itself does
  not run here: its multi-process lines use `charmrun`, which Reconverse does
  not have. Multi-process form on the Mac, one launch at a time:

  ```sh
  ~/charm_reconverse/reconverse-darwin-arm8/_deps/lci-src/lcrun -n 2 env DYLD_LIBRARY_PATH=$HOME/charm_reconverse/lib ./FoF3 -f ../../inputs/10k.tipsy -d oct +pe 4
  ```

- Input: `inputs/8m-lambb.tipsy` (ignored by git), every tenth particle of
  `lambb.00500` with masses multiplied by 10: 8,062,157 particles.

  | Quantity | Value |
  |---|---|
  | Groups | 2,848,768 |
  | Groups of 20 or more particles | 15,230, holding 4,341,388 particles (54%) |
  | Largest group (particles) | 153,305 |
  | `label_digest` | b16661be7fc431ad |
  | Catalogue digest (`-K -m 20`) | e8363207521bba50 |
  | Wall time, whole run, `+p8` (s) | 1.99 |
  | Iteration (s) / decomposition (s) / catalogue (s) | 1.37 / 0.56 / 0.69 |
  | Peak resident memory (GB) | 2.66 |

  With `-P 1` (lambb is a periodic box of side 1): 2,847,156 groups, 15,181
  of 20 or more particles holding 4,343,999, largest 153,804,
  `label_digest` c4f41054eacf794c, catalogue digest 57c1e840e74a8aae.

- The full `lambb.00500` will not fit: 2.66 GB for 8.06 M particles scales
  to about 27 GB, and the Mac has 16 GB. Not run.
- `~/Downloads/cosmo25cmb.768g.00144` is NOT usable. Its header says
  940,256,485 particles (39.5 GB); the file is 583 MB, a truncated download.

## HBT-HERONS, read from source (commit c728ee0, cloned to ~/HBT-HERONS)

- Build needs CMake, MPI, OpenMP and HDF5. The Mac has all but HDF5.
- Inputs: snapshot formats `swiftsim`, `gadget`, `gadget4`, `apostle`. For
  SWIFT format it reads, per particle type: `Coordinates`, `Velocities`,
  `Masses`, `ParticleIDs` and **`FOFGroupIDs`**, plus the `Header`, `Units`,
  `Cosmology` and `Parameters` groups.
- Unbinding (`src/subhalo_unbind.cpp`):
  - sources are at most `MaxSampleSizeOfPotentialEstimate` (default 1,000)
    randomly chosen particles with masses scaled up; a tree is built over
    them, and each particle's energy is one tree walk;
  - OpenMP over the particles of one sub-halo; removing unbound particles is
    serial;
  - without sampling, only the first pass computes full energies. Later
    passes build a tree over the particles just removed and subtract their
    contribution from the particles that remain.

Consequences:

- Feeding HBT-HERONS needs (1) a label output in FoF3 and (2) a converter
  that writes `FOFGroupIDs` next to the particle data. (1) is done, see
  below; (2) is not.
- Our own kernel should use the same correction idea: all pairs once, then
  subtract the removed particles. Total cost is then close to one pass.

What the converter still needs to know about lambb
(`~/Downloads/lambb.param`): it is a periodic box (`dPeriod = 1.0`) with
Omega0 = 0.3, Lambda = 0.7, in N-Body Shop units (`dHubble0 = 2.894405`).
SWIFT-format files carry physical units, so the box size in Mpc has to come
from whoever made the run. The Mac runs above used open boundaries; a
comparison with HBT-HERONS must use `-P 1`.

### Prediction for Check 2, written before any run

HBT-HERONS will unbind cosmo2b's 185 M-particle group in seconds to tens of
seconds per pass on one Frontier node, a few minutes in total, and will not
run out of memory. Reason: each pass is one tree walk over at most 1,000
sources per particle, not 1,000 pair terms.

If that holds, there is no speed claim against HBT-HERONS on these inputs.
What remains is running in situ (no stored outputs, finer cadence) and
potentials without sampling.

## Label output: FoF3 `-L <prefix>` (added 2026-11-10)

Each PE writes `<prefix>.<pe>.lbl`: one record per particle it holds, two
native 8-byte integers (order, label). The label is the group's minimum
particle order; after `-K -m M`, groups below M get -1. No gather, so the
cost does not grow with one PE's memory.

Checked on `8m-lambb`, 8 PEs:

| Check | Result |
|---|---|
| Records written | 8,062,157, every order from 0 to N-1 exactly once |
| Digest of the files against `label_digest` | equal (b16661be7fc431ad), one process and two processes |
| With `-K -m 20`: groups / grouped particles / smallest group | 15,230 / 4,341,388 / 20, equal to the catalogue line |
| Write time (s) | 0.05 |
| Small tests after the change | pass |

## Questions for the collaborator

1. Which runs do you want sub-haloes for, and how many particles?
2. How many outputs does each run keep, starting at what redshift, and are
   they evenly spaced in time?
3. Do particle IDs (`iord`) persist across outputs, including for gas that
   turns into stars?
4. What do you run today for haloes and sub-haloes, and what goes wrong
   with it at scale?
5. Which catalogue fields do you use (bound mass, centre, Vmax, host,
   merger tree), and which reader (pynbody, Tangos, other)?
6. Dark matter only, or gas and stars as well?
7. Would you help judge the catalogues against HBT-HERONS on one small run?

## Check 1: prior work on GPU unbinding and in-situ GPU sub-halo tracking

Web search, 2026-11-10. "Read" means the paper or source was read; otherwise
only the abstract was.

| Code | What runs on GPU | Unbinding | Inside the simulation | How checked |
|---|---|---|---|---|
| CRK-HACC, Frontier run (arXiv:2510.03557, 2511.21921) | FoF and DBSCAN galaxy finding through ArborX; states that all in-situ analysis runs on GPUs | no; "subhalo" and "unbind" do not occur | yes | read (second paper: first two-thirds) |
| HACC on Aurora (arXiv:2503.05947) | halo finder; sub-structure by core tracking | no | yes | read |
| HACC + PISTON/VTK-m, SC15 (OSTI 1244184) | halo centre: direct-sum potential of every halo particle | removes no particles; its sub-halo finder "does not take advantage of GPUs" | yes, haloes under 300,000 particles | read |
| OpenGadget3 (arXiv:2609.17658) | gravity, SPH, conduction | SubFind on CPU; several MPI ranks per large halo | yes, CPU | read |
| PKDGRAV3 | gravity | unbinding loop present but compiled out | at outputs, CPU | source read |
| RAMSES PHEW + ACACIA (arXiv:1812.06708) | nothing | yes, iterative | yes, with merger trees, MPI | read |
| jFoF (arXiv:2510.26851), JZ-Tree (arXiv:2604.05885) | FoF; jFoF adds a 6D FoF sub-halo demonstration | no | no | read / abstract |
| VELOCIraptor, ROCKSTAR | nothing in release | CPU | CPU | source / paper |
| SWIFT, AREPO-RT, Abacus GPU work | SPH; radiative transfer; near-field gravity | no GPU finder | CompaSO on CPU | abstracts |

Verdicts:

- **Unbinding on GPUs: not found.** Closest: HACC's 2015 GPU centre finder,
  which computes the potential of every halo particle on the GPU in situ but
  removes none.
- **A sub-halo finder with unbinding on GPUs: not found.**
- **History-based tracking with unbinding, in situ, on GPUs: not found.** On
  CPUs it exists: GADGET-4 Subfind-HBT, and RAMSES ACACIA builds merger trees
  on the fly with unbinding. HACC's core tracking is in situ on GPUs but has
  no unbinding.
- **One very large group spread over nodes: exists on CPUs** (GADGET-4 and
  OpenGadget3 SubFind; RAMSES PHEW across domains). Claim it only for GPUs.

Not verified: whether HACC's CosmoTools sub-halo finder moved to GPUs after
2015 (no paper says so); whether RAMSES unbinds one clump across domains;
unpublished branches of any code. Finding nothing is not proof.

What this means: every novelty claim has to say "on GPUs". Sub-halo finding
inside the simulation, with unbinding and merger trees, is established on
CPUs. HACC is the group most likely to have GPU work that is not yet
published.

## Check 2 on the Mac: HBT-HERONS on FoF3's groups (2026-11-10)

The whole chain runs: FoF3 labels, converter, HBT-HERONS, summary.

```sh
R=~/hbt-test/8m-lambb                      # labels/, snap/, out/
cd examples/fof3
./FoF3 -f ../../inputs/8m-lambb.tipsy -d oct -c stats -K -m 20 -P 1 -L $R/labels/8m +p8
../../subhalo/tipsy2hbt -i ../../inputs/8m-lambb.tipsy -l $R/labels/8m -o $R/snap/snap_0000.hdf5 --box-mpc 100 --h 0.7
OMP_NUM_THREADS=4 mpirun -np 2 ~/HBT-HERONS/build/HBT ../../subhalo/hbt_8m_lambb.conf
../../subhalo/hbtstat $R/snap/snap_0000.hdf5 $R/out/000 $R/hbt_reference.txt
```

- HBT-HERONS: commit c728ee0, built with GCC 16.2, Open MPI 5.0.8, HDF5
  2.2.0, `HBT_DM_ONLY=ON`, `HBT_MEASURE_UNBINDING_TIME=ON`; its 12 unit tests
  pass.
- `subhalo/tipsy2hbt.cpp` writes the SWIFT-layout file. Group ids run 1..G by
  decreasing size; no group is 2147483647. Its total mass is 0.298 in code
  units against Omega_m = 0.3, which checks the unit conversion.
- The 100 Mpc box is a stand-in: lambb's real box size is not on this
  machine. The scaling is exact, so which particles are bound does not
  depend on it; masses and velocities in physical units do.
- With one output, HBT-HERONS treats every FoF group as a new sub-halo and
  unbinds it. That is exactly "unbind every FoF group", the first product of
  our step 1.

Result (2 processes x 4 threads; HBT-HERONS read the same 15,181 groups and
153,804-particle largest group that FoF3's catalogue reports):

| Quantity | Value |
|---|---|
| Whole output (s) | 3.37 |
| Unbinding step, wall (s) | 1.97 |
| Reading and exchanging particles and groups (s) | 0.97 |
| Unbinding time summed over sub-haloes (s) | 10.2 |
| Groups with a self-bound sub-halo | 13,257 of 15,181 (1,924 have none) |
| Bound particles | 4,216,629 = 97.1% of the particles in groups |
| Unbinding passes per sub-halo | mean 1.52, largest 24 |
| Largest group: particles / bound / time (s) | 153,804 / 153,390 (99.7%) / 0.42 |
| Peak memory per process (GB) | 0.87 |

Unbinding time by group size (summed over the sub-haloes in each bin):

| Particles in group | Sub-haloes | Bound (%) | Mean passes | Time (s) |
|---|---|---|---|---|
| 16-63 | 8,416 | 96.4-97.8 | 1.3-1.5 | 0.14 |
| 64-1,023 | 4,397 | 96.4-97.4 | 1.6-1.8 | 1.36 |
| 1,024-16,383 | 407 | 97.2-99.2 | 1.6-2.0 | 3.73 |
| 16,384 and up | 37 | 99.0-99.7 | 1.0-1.6 | 4.98 |

What this says:

- No group dominates here: the time is spread over all sizes.
- The largest group costs 2.7 microseconds per particle. Sampling makes the
  cost linear in group size, so cosmo2b's 185 M-particle group scales to
  about 500 s of one thread's time, well under a minute on one Frontier node
  if the threads scale. This is an extrapolation from the Mac, not a
  measurement, and it agrees with the prediction above.
- 13% of FoF groups of 20 or more particles have no self-bound sub-halo.
  This input keeps one particle in ten, so the figure will differ on the
  full data.
- `hbt_reference.txt` (one line per sub-halo: group, particles, bound
  particles, passes, time) is the reference for our own unbinding stage.

Recommendation: treat Check 2 as answered. A Frontier run would need
HBT-HERONS built there, NChilada velocities in the converter, and cosmo2b's
box size, to confirm a number the Mac already bounds. Do it only if a
measured Frontier figure is needed for a paper.

## Open

- Check 0: the rest of the cosmo2b and cosmo25 histograms, from the logs on
  Frontier (the `grep` above).
- lambb's box size in Mpc, for physical units.
- The decision on when steps 1-2 start.
