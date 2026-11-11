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
- The full histograms of the three large inputs are in the next section.

### The three large inputs, from the full histograms (2026-11-11)

Source: the archived job logs on Frontier,

```sh
grep -rh "FOF3STAT components" /ccs/proj/csc710/rrao/fof-sc26w-archive | sort -u
```

The lines used are the ones whose counts match
`design/fof3-bigscale-2026-08-23.md`. The archive also holds lines with more
groups and a smaller largest group; those are runs from before the
under-merge fix of 2026-08-23.

```
cosmo2b    424897832 max_size 185317566 log2_histogram: 0:252506722 1:133305669 2:28366023 3:7115322 4:2181027 5:804120 6:329325 7:146123 8:69435 9:35009 10:18332 11:9655 12:5161 13:2751 14:1475 15:722 16:428 17:243 18:130 19:72 20:39 21:21 22:17 23:5 24:2 25:3 27:1
cosmo25    6730729617 max_size 2214117459 log2_histogram: 0:5656191482 1:839258563 2:169042958 3:43356737 4:13723347 5:4740789 6:2040175 7:1076708 8:597050 9:326443 10:175668 11:93708 12:49934 13:26176 14:13916 15:7398 16:4070 17:2037 18:1094 19:673 20:338 21:171 22:86 23:44 24:17 25:12 26:11 27:3 28:5 29:3 31:1
romulus25  29193922694 max_size 125856955 log2_histogram: 0:18490411374 1:9831237299 2:719366122 3:114168066 4:25047105 5:7044789 6:2901934 7:1540955 8:829182 9:543668 10:348716 11:211575 12:123892 13:69803 14:37636 15:20059 16:10390 17:5144 18:2561 19:1310 20:598 21:291 22:132 23:55 24:28 25:8 26:2
```

All numbers below are for groups of 32 or more particles, with group sizes
taken at the mid-point of each log2 bin and the exact size for the largest
group. Particle totals are good to a few percent.

| Input | Particles in input | Groups of 32+ | Particles in them (share of input) |
|---|---|---|---|
| cosmo2b | 1.98e9 | 1,423,069 | 1.17e9 (59%) |
| cosmo25 | 2.45e10 | 9,156,530 | 1.55e10 (63%) |
| romulus25 | 5.8e10 | 13,692,728 | 1.43e10 (25%) |

Where those particles sit, by group size. Each cell is the share of the
grouped particles and the number of groups holding it:

| Input | 32 to 1,023 particles | 1,024 to 65,535 | 65,536 to 1,048,575 | 1,048,576 and up |
|---|---|---|---|---|
| cosmo2b | 12% in 1,384,012 | 16% in 38,096 | 16% in 873 | 56% in 88 |
| cosmo25 | 7% in 8,781,165 | 12% in 366,800 | 11% in 7,874 | 71% in 691 |
| romulus25 | 11% in 12,860,528 | 31% in 811,681 | 27% in 19,405 | 31% in 1,114 |

Pair terms for one full pass over all of those groups, by the cap on
sources per particle. A group smaller than the cap is done exactly. Lower is
cheaper:

| Input | Cap 1,000 | Cap 10,000 | Cap 100,000 | Cap 1,000,000 | No cap |
|---|---|---|---|---|---|
| cosmo2b | 1.1e12 | 9.7e12 | 8.7e13 | 7.3e14 | 4.4e16 |
| cosmo25 | 1.5e13 | 1.4e14 | 1.3e15 | 1.2e16 | 7.6e18 |
| romulus25 | 1.3e13 | 1.2e14 | 9.0e14 | 5.9e15 | 7.6e16 |

What this says:

- On the clustered inputs most grouped particles sit in very few groups: 88
  groups hold 56% of them on cosmo2b, and 691 hold 71% on cosmo25. The path
  where a large group stays spread over processes therefore carries most of
  the particles. It is the main path, not a special case. Gathering small
  groups to an owner covers nearly all of the groups but a minority of the
  particles.
- Cost rises almost in proportion to the cap, because most particles are in
  groups larger than any practical cap: going from 1,000 to 100,000 sources
  costs about 85 times more on every input.
- No cap at all is out of reach on cosmo2b and cosmo25, where the largest
  group alone is 78% and 64% of the work. Exact potentials in the largest
  groups need a tree.
- Only cosmo25's largest group (74 GiB) is too large for one die.
- For scale, cosmo2b at 64 nodes (512 dies), where the GPU FoF iteration
  takes 1.06 s. At 1e10 pair terms per second per die, which is an
  assumption to be measured in step 1, one pass costs 0.2 s with a cap of
  1,000, 1.9 s with 10,000 and 17 s with 100,000. So a cap near 10,000 costs
  about as much as FoF itself, and beyond that unbinding sets the time per
  output. HBT-HERONS does better than these counts at cap 1,000 because it
  walks a tree over its sources.
  **Measured 2026-11-11: 2.0e11 pair terms per second per die, so these
  times are 20 times too long; see "The cap table in seconds" in step 1.**
- Proposal for step 1, from these numbers: three cases, which FoF3 can
  already tell apart. A group wholly inside one process is unbound in place
  on that process's GPU. A group that spans processes but is small is
  gathered to one owner. A large group stays spread, and its sources are sent
  to every process holding part of it.

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

Since 2026-11-11 `hbt_8m_lambb.conf` also turns sampling off, sets the
opening angle to 0.1 and saves energies. The numbers in this section are from
the file before that change (HBT-HERONS's defaults); the run with the change
is in "Results" under "Exact reference" below. On the Mac the output of this
section's run is now `out-sampled/` and its table `hbt_reference_sampled.txt`.

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

## Step 1: the standalone unbinding kernel (2026-11-11)

Written and run on Frontier. It is not yet compared group by group with
HBT-HERONS: that needs an HBT-HERONS run with sampling off (below).

Files, all in `subhalo/`:

| File | What it is |
|---|---|
| `unbind_kernel.h` | The pair-term kernel in Kokkos. Targets and sources are separate lists and many groups go in one launch, so one kernel does the all-pairs first pass, the subtraction of removed particles, and a capped sample |
| `unbind_groups.h` | Unbinds every group by HBT-HERONS's rules with sampling off; all groups advance together, at most two launches per pass |
| `unbind.cpp` | Reads the file `tipsy2hbt` writes, unbinds every group, compares bound counts with `hbtstat`'s table (`--ref`), writes its own (`--out`, `--dump`) |
| `unbind_rate.cpp` | Self-checks and pair-term rates on synthetic particles |
| `Makefile`, `run_unbind_rate.sbatch` | Build (hipcc on Frontier; any compiler with a Kokkos install elsewhere) and the one-die rate job |
| `subsample_tipsy.py` | Every tenth particle, masses times ten: rebuilds `8m-lambb.tipsy` from `lambb.00500` |

### The rules, read from HBT-HERONS (c728ee0)

From `src/subhalo_unbind.cpp`, `src/gravity_tree.cpp`, `src/snapshot.h`,
`src/subhalo_tracking.cpp` and `src/io/swiftsim_io/swiftsim_io.cpp`:

| Item | What HBT-HERONS does |
|---|---|
| First frame | mass-weighted mean position and velocity of all the group's particles |
| Later frames | the same means over the particles still bound |
| Energy | 0.5 \|v - v_frame + H a (x - x_frame)\|^2 + (G / a) phi, x comoving, v physical |
| Potential | Gadget spline reaching h = 2.8 softening lengths, -m / r outside; a particle's own term is excluded |
| Periodic box | nearest image for every pair; centre of mass taken relative to the first bound particle |
| Bound | energy below zero |
| Stop | dead below 20 bound particles (checked first); otherwise stop when a pass keeps at least 99.5%, compared in single precision |
| Next pass | if the pass removed fewer particles than it kept, subtract the removed particles' potential and move the kinetic term to the new frame; otherwise compute in full |
| Constants | G = 43.0071 (km/s)^2 Mpc/h per 1e10 Msun/h, scaled to the file's units; H from the file |
| Precision | positions, velocities, masses and energies in single precision (built without `HBT_REAL8`) |

Its defaults do not give an all-pairs answer, so `hbt_reference.txt` from
Check 2 is not an exact target:

- groups above 1,000 particles get 1,000 randomly chosen sources
  (`MaxSampleSizeOfPotentialEstimate`), about 450 of the 15,181 groups;
- every potential is a tree walk with opening angle 0.45
  (`TreeNodeOpenAngle`).

One difference is kept on purpose. Once HBT-HERONS has switched to
subtraction it never switches back, and a later pass that removes more than
it keeps then moves the kinetic term from a frame two passes old. Here that
pass is computed in full, and the groups it happens to are counted: none on
the 8M input, 6 of 1,000 synthetic groups.

### Checks

`unbind_rate` (job 5902364, one die, all pass; also on the login node's CPUs
with Kokkos OpenMP):

| Check | Result |
|---|---|
| Kernel against a double-precision direct sum on the host: all pairs, targets apart from sources, nearest image in a small box; softening 0.02 and 2 | largest relative error 1.5e-14 in double, 9.5e-7 in single summed in double, 9.3e-6 in single summed in single |
| Unbinding loop against a separate serial implementation (absolute positions, self pair skipped, every pass in full): 1,000 synthetic groups, 348,373 particles, 690 groups with more than 2 passes | same particles selected, same pass counts, in double; also in single on this set |
| The same with nearest-image separations forced on | same particles |
| Eight targets of every rate case recomputed on the host | agree: to 3.9e-15 in double, 2.1e-8 in single summed in double |

The checks were tried against deliberately broken code. Ten changes were
caught (kinetic shift, old frame, self term, both spline branches, the
branch boundary, one nearest-image axis, the Hubble term, the stop rule,
the sign of the subtraction). The first version of the kernel check missed
a wrong spline coefficient, because at softening 0.02 no pair fell inside
the spline; the softening-2 cases were added for that and the check now
requires 1% of the pairs in each spline branch.

### Pair-term rate of one MI250X die

Job 5902364 (the same rates in job 5902340), each case sized to about 2 s
and timed three times, best time. Higher is better.

| Kernel form | Pair terms per second, 10,000 sources per group |
|---|---|
| Single precision pair terms, summed in double, HBT-HERONS's arithmetic | 2.07e11 |
| The same, nearest image per pair | 1.64e11 |
| The same, `-m * rsqrt(r2)` instead of `-m / sqrt(r2)` | 3.75e11 |
| Single precision, summed in single | 2.11e11 |
| Double precision | 1.61e11 |
| Double precision, rsqrt | 2.71e11 |

By problem shape (single precision summed in double / double precision):

| Shape | Pair terms per second |
|---|---|
| 1,000 sources per target, 1e8 targets | 2.07e11 / 1.62e11 |
| 10,000 sources | 2.07e11 / 1.61e11 |
| 100,000 sources | 2.04e11 / 1.56e11 |
| 1,000,000 sources | 1.49e11 / 1.10e11 |
| Groups of 32, all pairs, 3,125,000 groups in one launch | 1.57e11 |
| Groups of 128 | 1.81e11 |
| Groups of 1,024 | 2.04e11 / 1.59e11 |
| Groups of 8,192 | 2.07e11 |
| Groups of 65,536 | 2.01e11 |
| Groups of 262,144 | 1.90e11 |

What this says:

- One die does 2.0e11 pair terms per second, 20 times the 1e10 assumed in
  Check 0.
- The rate holds from groups of 1,000 to 100,000 sources. It falls by a
  quarter at 1,000,000 sources (16 MB of sources) and by a quarter for
  groups of 32. Rates beyond 1,000,000 sources per group are not measured.
- Double precision costs 22%. Nearest-image separations, needed when a
  group is at least half the box wide, cost 21%. The rsqrt form is 1.8
  times faster and changed no bound count on the 8M input.
- The kernel is the plain one: one thread per target, sources read in
  order. No tiling or shared-memory blocking was tried.

### The cap table in seconds

Pair terms from Check 0 divided by the measured rate (single precision
summed in double; 2.07e11, 2.07e11, 2.04e11 and 1.49e11 for the four caps).
Lower is better.

| Input | Cap 1,000 | Cap 10,000 | Cap 100,000 | Cap 1,000,000 |
|---|---|---|---|---|
| cosmo2b, die-seconds per pass | 5.3 | 47 | 426 | 4,900 |
| cosmo2b on 512 dies (64 nodes), seconds | 0.010 | 0.092 | 0.83 | 9.6 |
| cosmo25, die-seconds per pass | 73 | 676 | 6,370 | 80,500 |
| romulus25, die-seconds per pass | 63 | 580 | 4,410 | 39,600 |

- These replace the estimate in Check 0 (0.2 s, 1.9 s and 17 s for cosmo2b).
  Against the 1.06 s GPU FoF iteration at 64 nodes, a cap of 100,000 costs
  about as much as FoF, and a cap of 10,000 costs a tenth of it.
- The seconds assume the work spreads evenly over the dies, and count one
  pass. On the 8M input all later passes together cost 0.2% of the first.
- With no cap cosmo2b needs about 3e5 die-seconds (10 minutes on 512 dies)
  at the 1,000,000-source rate, which is an extrapolation. Exact potentials
  in the largest groups still need a tree.
- Full lambb, every group of 20 or more, all pairs (1.4e13 pair terms): 70 to
  95 seconds on one die, by the same division. Not run.

### The 8M lambb sub-sample on one die

Job 5902394. `subsample_tipsy.py` on `lambb.00500` gives the Mac's input:
FoF3 on it (one node, `-K -m 20 -P 1 -L`) prints `label_digest`
c4f41054eacf794c, 15,181 groups holding 4,343,999 particles, largest
153,804, catalogue digest 57c1e840e74a8aae, all equal to the Mac's values
above. The label files hold 8,062,157 records with that digest and
3,718,158 written as -1, which is the first Frontier run of the `-K -m`
rule of `-L`. Then `tipsy2hbt --box-mpc 100 --h 0.7` and `unbind`.

| Quantity | `unbind`, all pairs, one die | HBT-HERONS on the Mac, defaults (Check 2) |
|---|---|---|
| Groups with a self-bound sub-halo | 13,256 of 15,181 | 13,257 of 15,181 |
| Bound particles | 4,217,368 (97.1%) | 4,216,629 (97.1%) |
| Passes per sub-halo | mean 1.52, largest 24 | mean 1.52, largest 24 |
| Largest group: bound of 153,804 | 153,524 (99.8%) | 153,390 (99.7%) |
| Sub-haloes in groups of 20-63 / 64-1,023 / 1,024-16,383 / 16,384 and up | 8,415 / 4,397 / 407 / 37 | 8,416 / 4,397 / 407 / 37 |
| Unbinding (s) | 1.07 in double (kernel 0.86), 0.86 in single (kernel 0.68) | 1.97 wall on 2 processes x 4 threads |

| Quantity | Value |
|---|---|
| Pair terms, first pass | 1.381e11 (93% of it in the 37 largest groups) |
| Pair terms, all later passes | 3.4e8 = 0.2% of the first, nearly all by subtraction |
| Rate in the first pass (pair terms per second) | 1.61e11 in double, 2.04e11 in single |
| Single against double precision | the same bound count and pass count in all 15,181 groups; also with rsqrt, and with positions rounded to single precision first |
| Open boundaries instead of periodic | 137 groups differ, 87 lose their sub-halo, 3.1% fewer bound particles |

What this says:

- The totals agree with HBT-HERONS's default run to 1 group in 15,181 and
  0.02% of the bound particles, with the same pass statistics. An error in
  the frame, the Hubble term or the periodic handling moves hundreds of
  groups (the broken-code runs above, and the open-boundary row), so these
  rules are very likely right. It is not the per-group comparison: the
  default run sampled its large groups, and its per-group table is on the
  Mac.
- Single precision is enough on this input.
- The timing column is not a speed claim: different machines, and
  HBT-HERONS samples and walks a tree where this is exact.

### Not done

Update, 2026-11-11: the HBT-HERONS reruns of the first two items are done
on the Mac and `hbtref` reads `SubhaloParticles` and `BindingEnergies`. The
three settings alone turned out not to be enough for a per-particle target
(the default early stop). Results, the pass rule and the commands for
Frontier are in "Exact reference for the unbinding kernel" below. The two
`unbind` runs against those files are done as well (job 5903512, "Results"
at the end of that section): the first two items below are complete, the
third is not started.

- The per-group comparison. On the Mac, add to `hbt_8m_lambb.conf`

  ```
  MaxSampleSizeOfPotentialEstimate    0
  TreeNodeOpenAngle                   0.1
  SaveBoundParticleBindingEnergies    1
  ```

  rerun HBT-HERONS and `hbtstat`, then either copy the table to Frontier or
  build `unbind` there (`make` line in `subhalo/Makefile`; it needs a
  Kokkos install with OpenMP) and run
  `unbind snap_0000.hdf5 --ref <table> --float-positions`. Decide the pass
  rule before the run. Expect a few particles of difference in some groups:
  HBT-HERONS stores energies in single precision and its tree is still an
  approximation at opening angle 0.1.
- A particle-by-particle comparison. `unbind --dump` writes every grouped
  particle's ID, group, bound flag and energy; nothing reads HBT-HERONS's
  `SubhaloParticles` and `BindingEnergies` yet.
- Capped sampling in `unbind_groups.h`, and groups at least half the box
  wide on real data (the nearest-image kernel is checked on synthetic
  particles only; the 8M input has no such group).

## Exact reference for the unbinding kernel (2026-11-11)

The run above is not a fair reference for a kernel: groups above 1,000
particles got 1,000 sampled sources, and every group used a tree with opening
angle 0.45. This section replaces it with a reference that has neither, and
that saves every bound particle's energy.

### What HBT-HERONS computes (read from c728ee0)

For one group with particles i (comoving position x_i in Mpc, peculiar
velocity v_i in km/s, mass m_i in 1e10 Msun), at expansion factor a:

- Frame: position X and velocity V. On the first pass they are the
  mass-weighted means over all of the group's particles; after each pass they
  are the mass-weighted means over the particles still bound. Positions are
  averaged as nearest-image offsets from one member, then shifted back.
- Kinetic term: K_i = 1/2 |(v_i - V) + H a (x_i - X)|^2, with x_i - X the
  nearest image, H in km/s/Mpc (70 here) and a = 1.
- Potential: U_i = (G / a) * sum over the other bound particles j of
  m_j * phi(r_ij), r_ij the nearest-image comoving distance, G = 43.0071
  (km/s)^2 Mpc / 1e10 Msun. With eps the softening (0.01 Mpc here) and
  h = 2.8 eps: phi = -1/r for r >= h; otherwise, with u = r/h,
  phi = (1/h) * (-2.8 + u^2 (5.333333333333 + u^2 (6.4 u - 9.6))) for u < 0.5
  and phi = (1/h) * (-3.2 + 0.066666666667/u + u^2 (10.666666666667 +
  u (-16 + u (9.6 - 2.133333333333 u)))) for u >= 0.5.
- Energy: E_i = K_i + U_i, per unit mass, in (km/s)^2. Bound means E_i < 0.
- One pass removes every particle with E_i >= 0, then updates X and V. If
  fewer than 20 particles remain, the group has no sub-halo.
- It stops when N_bound >= `BoundMassPrecision` * N_before. The default is
  0.995, so it can stop while up to 0.5% of the particles are still
  removable, and the saved energies are the ones computed BEFORE the last
  removal and frame update. With `BoundMassPrecision 1` it stops only when a
  pass removes nothing, and the saved energies are those of the final bound
  set in its own frame. Only that is a fixed point a kernel can be held to.
- `TreeNodeOpenAngle 0` opens every tree node, so the potential is the plain
  pairwise sum. `MaxSampleSizeOfPotentialEstimate 0` turns sampling off, and
  also turns off the separate centre refinement.
- Precision: potentials are summed in double, but positions are single (a
  default build) and each particle's energy is stored and tested as a single.
  When less than half of the particles are removed, the next pass does not
  recompute: it subtracts the removed particles' potential and adjusts the
  kinetic term for the frame change, in single precision.

### Runs

All on `8m-lambb` with `-P 1`, the snapshot of the Mac check above:

| Name | Build | Settings beyond no sampling, angle 0, energies saved |
|---|---|---|
| exact-double | `HBT_REAL8=ON` | `BoundMassPrecision 1`. **The reference.** |
| exact-single | default | `BoundMassPrecision 1`. Measures the reference's own rounding. |
| exact-single-p995 | default | default precision 0.995: the three settings alone. Shows what the early stop costs. |
| rerun (`hbt_8m_lambb.conf`) | default | angle 0.1 instead of 0, default precision 0.995 |
| rerun with full stop | default | angle 0.1 instead of 0, `BoundMassPrecision 1` |

exact-single-p995 was stopped one minute in and not rerun: the two
opening-angle-0.1 runs in "Results" below isolate the early stop at a
fraction of the cost.

### Pass rule, written before any of these runs

For a group g, B_ref and B_k are the bound sets of the reference and of the
kernel, and S_g is the median of |E_ref| over B_ref.

The tolerance tau is set from the reference alone, before any kernel output
is looked at: tau = max(1e-4, 10 * n), where n is the 99.9th percentile, over
particles bound in both exact-single and exact-double, of
|E_single - E_double| / S_g.

A kernel passes if all three hold:

1. Energies. Of the particles in both B_ref and B_k, at least 99.9% have
   |E_k - E_ref| <= tau * S_g, and none exceeds 10 * tau * S_g.
2. Membership. Every particle bound in one and not the other lies at the
   zero-energy boundary: |E| <= tau * S_g, taking E from whichever side has
   it bound. The number of such particles is at most the number of reference
   particles with |E_ref| <= tau * S_g.
3. Groups. A group may have a sub-halo in one and not the other only if its
   bound count in either is within 2 of the 20-particle minimum.

Counts are expected to be near-equal, not identical.

### Results (2026-11-11)

All runs: 2 processes x 4 threads on the Mac, the same 15,181 groups. Lower
time is better. "Stop" is `BoundMassPrecision`.

| Run | Precision | Sources | Opening angle | Stop | Unbinding, wall (s) | CPU (s) | Groups with a sub-halo | Bound particles |
|---|---|---|---|---|---|---|---|---|
| sampled (2026-11-10) | single | 1,000 sampled | 0.45 | 0.995 | 1.97 | not kept | 13,257 | 4,216,629 |
| rerun, `hbt_8m_lambb.conf` | single | all | 0.1 | 0.995 | 416 | 2,195 | 13,256 | 4,217,367 |
| the rerun with full stop | single | all | 0.1 | 1 | 428 | 2,311 | 13,256 | 4,215,955 |
| exact-single | single | all | 0 | 1 | 1,884 | 8,634 | 13,256 | 4,215,954 |
| exact-double (**reference**) | double | all | 0 | 1 | 2,047 | 9,787 | 13,256 | 4,215,954 |
| `unbindspec` (plain loops, 10 threads) | double | all | all pairs | nothing removed | 137 (whole program) | 987 | 13,256 | 4,215,954 |

`hbtstat` on the rerun (`hbt_8m_lambb.conf` as it now stands; table in
`~/hbt-test/8m-lambb/hbt_reference.txt`, the sampled run's table is now
`hbt_reference_sampled.txt` and its output `out-sampled/`):

| Quantity | Sampled run | Rerun |
|---|---|---|
| Whole output (s) | 3.37 | 418.5 |
| Unbinding step, wall (s) | 1.97 | 416.2 |
| Unbinding time summed over sub-haloes (s) | 10.2 | 2,437 |
| Groups with a self-bound sub-halo | 13,257 | 13,256 |
| Bound particles | 4,216,629 | 4,217,367 |
| Unbinding passes per sub-halo | mean 1.52, largest 24 | mean 1.52, largest 24 |
| Largest group: bound / passes / time (s) | 153,390 / 1 / 0.42 | 153,524 / 1 / 400.8 |

| Particles in group | Sub-haloes, rerun | Bound, rerun (%) | Mean passes, rerun | Time, rerun (s) | Time, sampled run (s) |
|---|---|---|---|---|---|
| 16-63 | 8,415 | 96.9 | 1.41 | 0.2 | 0.14 |
| 64-1,023 | 4,397 | 96.9 | 1.70 | 3.3 | 1.36 |
| 1,024-16,383 | 407 | 98.5 | 1.75 | 88.5 | 3.73 |
| 16,384 and up | 37 | 99.4 | 1.49 | 2,345.5 | 4.98 |

- Without sampling the 37 largest groups hold 96% of the time. HBT-HERONS
  unbinds one group on one thread, so the step's wall time is one group's
  time: the second-largest group took 416.15 s of the 416.17 s.
- The largest group costs 2.6 ms per particle, against 2.7 microseconds with
  sampling: a factor of about 1,000.

Tolerance, from exact-single against exact-double, as the rule says: the
same 4,215,954 particles are bound in both, |dE|/S is 2.8e-6 at the median,
1.41e-4 at the 99.9th percentile and 4.6e-4 at most. So **tau = 0.00141**.
248 reference particles have |E| <= tau * S.

Each result against the reference at that tau (|dE|/S over the particles
bound in both; lower is better):

| Candidate | Only in candidate | Only in reference | Median | 99.9% | Largest | Verdict |
|---|---|---|---|---|---|---|
| exact-single | 0 | 0 | 2.8e-6 | 1.4e-4 | 4.6e-4 | pass (sets tau) |
| `unbindspec` | 0 | 0 | 2.6e-8 | 4.0e-7 | 1.1e-6 | **pass** |
| rerun with full stop (angle 0.1) | 1 | 0 | 6.6e-6 | 1.5e-4 | 0.016 | fail: one group |
| rerun (angle 0.1, default stop) | 1,413 | 0 | 1.0e-3 | 5.0e-2 | 0.54 | fail |
| `unbindspec --g 43.0091733` | 14 | 0 | 7.2e-5 | 5.3e-4 | 0.23 | fail: 7 groups |

What this says:

1. **The written definition is what HBT-HERONS computes.** A plain
   double-precision all-pairs loop reproduces every bound set and every
   energy to 1.1e-6 of S, which is the single-precision storage of the
   energies. The rule can be passed.
2. **The default early stop is not a reference.** With `BoundMassPrecision`
   left at 0.995 the rerun keeps 1,413 particles that further passes remove,
   and only 58% of its energies are within tau. With full stop the same
   settings are one particle from the reference. So the three settings alone
   are not enough: the reference needs `BoundMassPrecision 1`
   (`hbt_8m_lambb_exact.conf`).
3. **Opening angle 0.1 is small enough for the energies** (99.9th percentile
   1.5e-4, the same as single-precision rounding) and 4.4 times cheaper than
   angle 0 (428 s against 1,884 s).
4. **The result is not unique at the level of tau.** Removing particles until
   none has E >= 0 can end on different bound sets. In group 2536 (136
   particles) particle 6008911 has E = +0.20 (km/s)^2 on the first pass, 9e-6
   of S. The reference removes it and ends with 129 bound. The angle-0.1 run
   computes it just below zero, keeps it, and ends with 130 bound, where that
   particle has E = -354 (km/s)^2 (0.016 of S) and the other particles are
   lower by its potential, 111 of them by more than tau. Both end states
   satisfy the definition. `unbindspec --trace 6008911` shows the two paths.
5. **The rule as first written does not allow for that.** It assumes a particle
   that changes sides is at the boundary in the final state. A particle that
   changes sides at an earlier pass can end well inside, and take its
   neighbours' energies with it. HBT-HERONS itself fails the rule this way in
   one group of 13,256.
6. **The gravitational constant matters.** N-Body Shop code units (G = 1,
   rho_crit = 1) imply G = 43.0091733 in the snapshot's units, 4.8e-5 above
   HBT-HERONS's 43.0071. That alone moves 14 particles across the boundary
   and puts 311 particles in 7 groups beyond tau, up to 0.23 of S in a
   40-particle group. A kernel working in code units must multiply its
   potential by 43.0071 / 43.0091733 = 0.9999518 before the comparison.

### Pass rule, amended (adopted 2026-11-11, before any kernel output was compared)

Items 4 and 5 are about HBT-HERONS against itself, so the rule is amended
without having looked at a kernel result. S is still the median of |E_ref|
over a group's bound particles in the reference; tau is unchanged (0.00141).

1. Energies. In a group whose bound sets are equal, each particle is
   compared with the reference. In a group whose bound sets differ, each of
   the kernel's bound particles is compared with the definition evaluated in
   double precision on the kernel's own bound set. Over all of them, at
   least 99.9% within tau * S and none beyond 10 * tau * S.
2. Groups whose bound sets differ. In each one:
   a. the kernel's set is an end state: evaluated on that set, no particle
      of it has E >= tau * S;
   b. the difference starts at the boundary. The definition is replayed from
      the whole group. P is the first pass at which a differing particle is
      within tau * S of zero: one the reference removes and the kernel keeps
      (0 <= E <= tau * S when removed), or one the reference keeps and the
      kernel removes (-tau * S <= E < 0 at that pass). No particle the
      kernel keeps may be removed before P, or at P with E > tau * S.
3. Groups. Unchanged: a sub-halo on one side only is allowed only within 2
   of the 20-particle minimum.

Both sides must run until a pass removes nothing (HBT-HERONS
`BoundMassPrecision 1`, `unbind --bound-fraction 1`); a result that stopped
early fails 2a.

Known limit: after the first boundary difference at pass P, later
differences in that group are held only by 1 and 2a, not judged one by one.
A stricter version would follow the kernel's path pass by pass and require
every decision that departs from the definition to be within tau * S. Not
built; the listing below shows each differing group's counts, so a group
with many differing particles is visible.

`subhalo/unbindcheck` applies it (and prints the verdict of the rule as
first written). It reads the file `unbind --dump` writes, or a
`.particles.bin` file; the dump reader was tested with files written in
that layout from two of the results below, and it read the real dumps of
Frontier job 5903512. Cases with known answers, all on 8m-lambb against
exact-double:

| Candidate | Should | Rule as written | Amended rule | Groups with different bound sets |
|---|---|---|---|---|
| exact-single | pass | pass | pass | 0 |
| `unbindspec` (after it was moved onto `unbindspec.h`) | pass | pass | pass | 0 |
| HBT-HERONS, angle 0.1, full stop | pass | fail | pass | 1: group 2536, particle 6008911 at 9.1e-6 of S on pass 1 |
| `unbindspec --g 43.0091733` (code-unit constant) | pass | fail | pass | 12, each from a particle within 1.1e-4 of S of zero |
| HBT-HERONS, angle 0.1, default early stop | fail | fail | fail | 220, of which 183 fail (not end states) |
| `unbindspec --no-hubble` | fail | fail | fail | 2,549, of which 2,507 fail; 6.7% of energies within tau |
| `unbindspec --g 43.5` (1.1% too large) | fail | fail | fail | 773, of which 705 fail; no energy within tau |
| reference with the most bound particle of group 2 taken out | fail | fail | fail | 1: does not start at the boundary |
| reference with group 3's energies times 1.01 | fail | fail | fail | 0; 34,595 energies beyond 10 tau |

The checker takes 0.7 s when no group differs and 41 s for the 12 groups of
the code-unit case, which include the largest (Mac, 10 threads).

### Tolerance for single-precision kernels (decided 2026-11-11, before any kernel output was compared)

Decision: a single-precision kernel that differs from the reference by its
own rounding gets a looser tolerance; it is not treated as needing a fix.

tau was set as ten times the difference between HBT-HERONS's single- and
double-precision builds. A kernel's single-precision rounding is measured
the same way, against the same kernel in double precision on the same
input, and ten times it is added:

    tau_single = min(tau + 10 * n_k, 10 * tau)

n_k is the 99.9th percentile, over the particles bound in both runs, of
|E_single - E_double| / S. Neither measurement involves the comparison
being judged. Conditions:

- the double-precision kernel passes at tau by itself first;
- the allowance stops at 10 * tau (0.0141), so that an error cannot buy its
  own tolerance;
- it applies to `--kernel f32` and `--kernel f32f32`, with or without
  `--rsqrt`. Double-precision variants are held to tau.

`unbindcheck --single-against <double result>` measures n_k, prints it and
applies the wider tolerance. Checks: HBT-HERONS's single-precision run
against its double one gives n = 1.41e-4 and tolerance 0.00282, and passes;
the run without the Hubble term and the run with G 1.1% too large reach the
10 * tau stop and still fail (47% and 38% of energies within the tolerance).

What the kernel's arithmetic predicts. Its self-checks give a relative
error in the potential of 9.5e-7 (single, summed in double) and 9.3e-6
(single, summed in single), and `unbindspec --potential-ratio` gives
|U| / S = 1.46 at the median, 4.04 at the 99.9th percentile and 4.96 at
most on this input, so those errors move an energy by about 4e-6 and 4e-5
of S at the 99.9th percentile.

Outcome on 8m-lambb (Frontier job 5903512, run before this allowance was
on `main`): it was not needed. All three single-precision forms pass at
tau itself, with the reference's bound particles exactly. Against the
reference their 99.9th percentiles are 5.3e-7 (`f32`), 5.0e-5 (`f32f32`,
which matches the 4e-5 predicted above) and 5.4e-7 (`f32 --rsqrt`), so the
allowance would have set tolerances of about 0.00142, 0.0019 and 0.00142.
Those were read from the distributions against the reference. Measured with
`--single-against` in Frontier job 5903660 ("Rerun with the allowance"
below): n_k is 4.3e-7 for `f32`, 4.4e-7 for `f32 --rsqrt` and 5.0e-5 for
`f32f32` with or without `--rsqrt`, which gives tolerances of 0.001414,
0.001414 and 0.00191. The allowance stays as the rule for other inputs.

### Comparing `unbind` with the references

Tools, all in `subhalo/` (`make compare`):

| Tool | What it does | Needs |
|---|---|---|
| `hbtref` | HBT-HERONS output (`SubhaloParticles`, `BindingEnergies`) to a `.particles.bin` reference; with the snapshot as third argument also a copy in FoF3's numbering | HDF5 |
| `unbindcmp` | two `.particles.bin` files under the rule as first written; without `--tau` it measures the noise that sets tau | nothing |
| `unbindcheck` | the amended rule; reads `unbind --dump` directly; `--single-against` for single-precision kernels | HDF5, OpenMP |
| `unbindspec` | the written definition on every group, in plain double-precision loops (`--trace`, `--g`, `--no-hubble`, `--potential-ratio`) | HDF5, OpenMP |
| `unbindrec.h`, `unbindspec.h` | the 24-byte record and its reader; the definition shared by `unbindspec` and `unbindcheck` | |

A `.particles.bin` file has one 24-byte record per bound particle, native
byte order, no header: int64 particle id, int64 group id, float64 energy in
(km/s)^2.

Files on the Mac, in `~/hbt-test/8m-lambb/`:

| File | Size | What it is |
|---|---|---|
| `hbt_reference.txt` | 0.3 MB | per-group table of the rerun with the three settings (angle 0.1, default early stop): the input of `unbind --ref` |
| `exact-double.particles.bin` | 101 MB | the exact reference in the snapshot's numbering (ParticleIDs, FOFGroupIDs), which is also what `unbind --dump` writes |
| `exact-double.fof3.particles.bin` | 101 MB | the same in FoF3's numbering: particle id = order in the Tipsy file, group id = FoF3's label. Checked against FoF3's `-L` files: 0 of 4,215,954 records differ. For a later kernel inside FoF3 |
| `exact-double.groups.txt` | 1 MB | per group: bound count, passes, final frame position and velocity |
| `open0.1.particles.bin` | 101 MB | per-particle output of the rerun with default early stop |

On Frontier, with the first two files copied next to the snapshot (the
snapshot built there has the same particle and group numbers: `tipsy2hbt`
numbers groups by size, ties by label):

```sh
make compare                     # in subhalo/, same HDF5 as unbind
# 1. per-group counts, both sides stopping early at 0.995
./unbind snap_0000.hdf5 --ref hbt_reference.txt --float-positions
# 2. per particle, both sides running to the end
./unbind snap_0000.hdf5 --bound-fraction 1 --dump kernel_f64.hdf5
./unbindcheck snap_0000.hdf5 exact-double.particles.bin kernel_f64.hdf5 --tau 0.00141
# 3. the single-precision kernels, each against the double-precision run
./unbind snap_0000.hdf5 --bound-fraction 1 --kernel f32 --dump kernel_f32.hdf5
./unbindcheck snap_0000.hdf5 exact-double.particles.bin kernel_f32.hdf5 --tau 0.00141 \
              --single-against kernel_f64.hdf5
```

Expected in 1: 13,256 groups with a sub-halo on both sides, 4,217,368 bound
particles against HBT-HERONS's 4,217,367. Expected in 2: 13,256 groups and
about 4,215,954 bound particles. Run 2 without `--float-positions`: the
reference is the double-precision build. Run 3 only after 2 passes; repeat
it for `--kernel f32f32` and for each with `--rsqrt`.
`run_unbind_compare.sbatch` does all of it in one job. It measures a form
with `--rsqrt` against the double-precision run with `--rsqrt`, the same
kernel in double precision, which must itself pass at tau.

`unbind` reads the snapshot's units, as HBT-HERONS does, so item 6 does not
apply to it. A later kernel in code units (box 1, G = 1, H = 2.894405, the
Tipsy file's softening, 1e-4 stored as a single, so h = 2.8e-4) converts,
for the 100 Mpc, h = 0.7 stand-in, as E in (km/s)^2 = E_code * 5,848,944.23
(the velocity unit, 2,418.459062 km/s, squared), with the potential first
multiplied by 0.9999518.

For scale: `unbindspec` does the 1.381e11 pair terms of this input's first
pass at about 1.0e9 pair terms/s on the Mac's 10 cores in double precision,
against 1.6e11 per second on one MI250X die (step 1).

#### Results (2026-11-11, Frontier job 5903512)

One node, 33 s: `unbind` on one MI250X die and `unbindcheck` on 56 threads,
both inside the job (`subhalo/run_unbind_compare.sbatch`). Binaries from
12158ce. `make compare` built with the default `c++` (GCC 7.5.0) and no
source change. The snapshot is the one of job 5902394; `hbt_reference.txt`
and `exact-double.particles.bin` were copied from the Mac.

Per group, both sides stopping early at 0.995
(`unbind snap_0000.hdf5 --ref hbt_reference.txt --float-positions`,
double-precision kernel):

| Quantity | `unbind` | HBT-HERONS rerun (angle 0.1) |
|---|---|---|
| Groups with a sub-halo (count) | 13,256 | 13,256 |
| Bound particles (count) | 4,217,368 | 4,217,367 |
| Groups with the same bound count (count; higher is better) | 15,178 of 15,181 | |
| Sub-haloes with a different pass count (count; lower is better) | 0 | |

The three groups that differ do so by one particle each:

| Group | Particles in group (count) | Bound, `unbind` (count) | Bound, rerun (count) |
|---|---|---|---|
| 41 | 15,406 | 15,311 | 15,310 |
| 48 | 12,936 | 12,825 | 12,824 |
| 2536 | 136 | 129 | 130 |

Group 2536 is the group of item 4 above: `unbind` ends on the exact
reference's 129, the angle-0.1 run on 130.

Per particle, both sides running until a pass removes nothing
(`unbind --bound-fraction 1 --dump`, without `--float-positions`, then
`unbindcheck --tau 0.00141` against `exact-double.particles.bin`):

| Kernel (`unbind` option) | Groups with a sub-halo (count; the reference has 13,256) | Bound particles (count; the reference has 4,215,954) | Rule as first written (verdict) | Amended rule (verdict) | Groups with different bound sets (count; lower is better) | Largest \|dE\|/S (fraction of S; lower is better; tau is 0.00141) | Unbinding time (s; lower is better) |
|---|---|---|---|---|---|---|---|
| Double precision (default) | 13,256 | 4,215,954 | pass | pass | 0 | 1.09e-6 | 1.23 |
| Single precision pair terms, summed in double (`--kernel f32`) | 13,256 | 4,215,954 | pass | pass | 0 | 1.60e-6 | 0.99 |
| Single precision, summed in single (`--kernel f32f32`) | 13,256 | 4,215,954 | pass | pass | 0 | 1.04e-4 | 0.98 |
| Single precision with rsqrt (`--kernel f32 --rsqrt`) | 13,256 | 4,215,954 | pass | pass | 0 | 1.62e-6 | 0.69 |

- Every variant selects exactly the reference's particles: none bound only
  in the reference, none only in the kernel, in all 13,256 groups. Part 2 of
  the amended rule therefore had no group to judge, and all energies are
  within tau * S.
- Unbinding time is `unbind`'s own figure for the whole unbinding (kernel,
  host work and copies to and from the die; not reading or grouping, 1.1 s).
  The kernel part is 0.85, 0.68, 0.67 and 0.38 s, in table order.
- `unbindcheck` as run here did not print the largest difference (it does
  since the commit that added `--single-against`). It comes from
  `unbindcmp` on the bound particles of each dump, written as a
  `.particles.bin` file with `h5dump` and numpy (no tool in the repo does
  that conversion). The rest of the distribution of \|dE\|/S, lower is
  better:

  | Kernel | Median (fraction of S) | 99.9th percentile (fraction of S) |
  |---|---|---|
  | Double precision | 2.6e-8 | 4.0e-7 |
  | `--kernel f32` | 3.2e-8 | 5.3e-7 |
  | `--kernel f32f32` | 1.0e-6 | 5.0e-5 |
  | `--kernel f32 --rsqrt` | 3.4e-8 | 5.4e-7 |

- In every dump, no group has between 1 and 19 bound particles: the 1,925
  groups that end below 20 have `Bound` = 0 for all their particles, and no
  bound particle has an energy of zero or above.
- Running to the end instead of stopping at 0.995: 1,414 fewer bound
  particles, passes per sub-halo mean 1.61 and largest 27 (1.52 and 24), and
  later passes cost 0.5% of the first pass's pair terms (0.2%).

What this says:

1. **`unbind` reproduces HBT-HERONS's exact result particle for particle**
   on this input, in all four forms of the kernel. The double-precision
   kernel is as close to the reference as `unbindspec` (largest difference
   1.1e-6 of S for both).
2. **Precision is not the limit at this tolerance.** Summing in single
   precision is the only form that moves the energies visibly, and its
   largest difference is 13 times below tau. The rsqrt form with single
   precision pair terms summed in double changes nothing that the rule
   measures and is the fastest, 0.69 s.
3. **The three per-group differences at the early stop are not kernel
   errors.** With both sides run to the end those groups have the reference's
   bound sets. Group 2536 is the known non-unique case; groups 41 and 48 were
   not traced.

#### Rerun with the allowance (2026-11-11, Frontier job 5903660)

One node, 44 s, tools from 94b1018 (`unbindcheck` with `--single-against`,
rebuilt on Frontier with the default `c++`, no source change); `unbind` is
unchanged. Six forms of the kernel, each run until a pass removes nothing.
The two double-precision forms are held to tau. Each single-precision form
is checked at tau and again with the allowance, its rounding n_k measured
against the double-precision run of the same form. The per-group result
above was repeated and is the same.

| Kernel (`unbind` option) | Bound particles (count; the reference has 4,215,954) | Groups with different bound sets (count; lower is better) | Amended rule at tau = 0.00141 (verdict) | n_k (fraction of S; lower is better) | Tolerance with the allowance (fraction of S) | Amended rule with the allowance (verdict) | Largest \|dE\|/S against the reference (fraction of S; lower is better) | Unbinding time (s; lower is better) |
|---|---|---|---|---|---|---|---|---|
| Double precision (default) | 4,215,954 | 0 | pass | not applicable | 0.00141 | not applicable | 1.09e-6 | 1.24 |
| Double precision with rsqrt (`--rsqrt`) | 4,215,954 | 0 | pass | not applicable | 0.00141 | not applicable | 1.09e-6 | 0.89 |
| `--kernel f32` | 4,215,954 | 0 | pass | 4.3e-7 | 0.001414 | pass | 1.60e-6 | 0.99 |
| `--kernel f32 --rsqrt` | 4,215,954 | 0 | pass | 4.4e-7 | 0.001414 | pass | 1.62e-6 | 0.69 |
| `--kernel f32f32` | 4,215,954 | 0 | pass | 5.0e-5 | 0.00191 | pass | 1.04e-4 | 0.98 |
| `--kernel f32f32 --rsqrt` | 4,215,954 | 0 | pass | 5.0e-5 | 0.00191 | pass | 1.04e-4 | 0.68 |

- All six have 13,256 groups with a sub-halo and pass the rule as first
  written as well, at tau and with the allowance. No dump has a group with
  between 1 and 19 bound particles.
- Two forms are new here: double precision with rsqrt, and single precision
  summed in single with rsqrt. The other four repeat job 5903512 with the
  same counts and the same largest differences.
- `unbindcheck` now prints the distribution of \|dE\|/S itself; its figures
  equal the ones obtained through `unbindcmp` for job 5903512.
- The allowance changes no verdict on this input: every form already
  passes at tau. It widens the tolerance by 0.3% for the forms summed in
  double and by 36% for the forms summed in single.
- Summing in single precision sets the error of those forms: with and
  without rsqrt they have the same distribution (median 1.0e-6, 99.9th
  percentile 5.0e-5, largest 1.04e-4).

Not covered by this comparison: an input whose scale factor is not 1 (the
factors G / a and H a), more than one softening or particle type, a group
at least half the box wide, and capped sampling.

## Capped sampling: definition and pass rule (2026-11-11)

Decided 2026-11-11, before any capped kernel output existed:

- the sample is chosen by particle ID;
- the pass rule is that the kernel is held to a written definition of the
  sampled method, under the existing tau (0.00141) and the amended rule,
  unchanged.

HBT-HERONS's own sampled output cannot be a per-particle target, because its
sample is a random shuffle. Other rules were considered and not adopted
here: a statistical comparison with the exact result limited by
HBT-HERONS's own sampling error, a fixed accuracy requirement, and a test
for bias over several samples. They answer a different question, how
accurate a capped result is, which this rule does not.

### Definition

C is the cap: the most sources that enter one particle's potential. No cap
(C = 0) is the definition of "What HBT-HERONS computes".

- Order. key(ID) is the 64-bit value z computed, with unsigned wrap-around,
  as z = ID + 0x9e3779b97f4a7c15; z = (z xor (z >> 30)) * 0xbf58476d1ce4e5b9;
  z = (z xor (z >> 27)) * 0x94d049bb133111eb; z = z xor (z >> 31). A group's
  particles are ordered by increasing key, ties by ID. ID is the snapshot's
  `ParticleIDs` value (FoF3's particle order plus 1).
- A pass with bound set B of N_b particles:
  - N_b <= C: nothing changes. The sources are all of B.
  - N_b > C: the sample S is the first C particles of B in the order, and
    f = M(B) / M(S), the ratio of the summed masses. The potential is
    U_i = (G / a) * f * sum over j in S, j not i, of m_j * phi(r_ij).
- Everything else is as before: the frame is the mass-weighted mean over
  all of B with the true masses, K_i, E_i = K_i + U_i, bound means E_i < 0,
  no sub-halo below 20 bound particles, and the passes end when one removes
  nothing.

So the energies at a pass depend only on B. Whether a set is an end state
is then well defined, which the amended rule needs (2a).

What is HBT-HERONS's (read from c728ee0) and what is not:

| Item | HBT-HERONS with `MaxSampleSizeOfPotentialEstimate` C | This definition |
|---|---|---|
| When a pass samples | N_b >= C (at N_b = C the sample is all of B) | N_b > C: the same |
| Factor on the sample's masses | M(B) / M(S) | the same |
| A sampled particle's own term | excluded | the same |
| Frame | all bound particles, true masses | the same |
| Order of the sample | one random shuffle of the group | by key(ID) |
| Later passes | if a pass removed fewer than C particles and fewer than it kept, the removed particles' exact potential is subtracted from the sampled estimate, and no pass is computed from a sample again | every pass is defined from the current sample |
| Centre refinement after the last pass | reorders the most bound particles | not part of the bound set; not defined here |

The sixth row is the one real difference in method. HBT-HERONS's energies
after the first pass depend on the path taken; here they do not.

### How it may be computed

The definition says what a pass's energies are, not how to get them.
`unbindspec --cap C` recomputes every pass from its sample: N_b * C pair
terms per pass. `unbind --cap C` keeps each particle's sum over the sample
without the factor. After a pass it takes out the sample members that were
removed and adds the particles that replace them: N_b * 2d pair terms when d
members were replaced. It recomputes in full when 2d >= C, and goes over to
the exact method once N_b <= C.

### Pass rule

`unbind --cap C` against `unbindspec --cap C`, both run until a pass removes
nothing, judged by `unbindcheck --cap C --tau 0.00141`: the amended rule as
it stands, with the capped definition wherever the rule evaluates or
replays the definition. A single-precision form gets the allowance of
"Tolerance for single-precision kernels", measured against the
double-precision kernel at the same cap.

Two conditions hold exactly and are checked as well:

- a group at or below the cap is not sampled, so its result equals the
  result without a cap;
- a cap above the largest group changes nothing.

Not shown by this rule: how far a capped result is from the exact one, and
whether that distance matches HBT-HERONS's at the same cap.

## Open

- Done 2026-11-11: the comparison of `unbind` with HBT-HERONS. All six
  forms of the kernel select the exact reference's 4,215,954 particles in
  13,256 groups and pass both rules at tau (jobs 5903512 and 5903660); the
  single-precision allowance was measured and changes no verdict. The
  pair-term rate is measured: 2.0e11 per die.
- Capped sampling in `unbind_groups.h`, and a group at least half the box
  wide on real data. Not started.
- The comparison at a scale factor other than 1.
- lambb's box size in Mpc, for physical units.
- The decision on when steps 1-2 start.
