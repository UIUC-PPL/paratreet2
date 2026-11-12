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
- Capped sampling in `unbind_groups.h`: done 2026-11-11, see "Capped
  sampling: definition and pass rule". Groups at least half the box wide,
  made from real particles, and a scale factor other than 1: done
  2026-11-11 against `unbindspec`, not yet against HBT-HERONS, see "A scale
  factor other than 1, and groups at least half the box wide".

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

### Results (2026-11-11, Frontier job 5903809)

One node, 5 min 51 s (`subhalo/run_unbind_cap.sbatch`): `unbind` on one
MI250X die, `unbindspec` and `unbindcheck` on 56 threads. The 8M lambb
snapshot of job 5902394. Six forms of the kernel at each of three caps, all
run until a pass removes nothing.

**Every one of the 18 runs passes at tau = 0.00141**, under the rule as
first written and under the amended rule, with the definition's bound
particles exactly: no group has a different bound set. The 12
single-precision runs also pass with the allowance, which was not needed.

Per cap, double-precision kernel (pair terms and times: lower is better):

| Cap (sources per particle) | Groups sampled (count) | Bound particles, kernel and definition (count) | Groups with different bound sets (count; lower is better) | Largest \|dE\|/S, kernel against definition (fraction of S) | Pair terms, first pass | Pair terms, all later passes | Kernel time, double / single with rsqrt (s) | Unbinding time, double / single with rsqrt (s) |
|---|---|---|---|---|---|---|---|---|
| 1,000 | 449 | 4,214,992 | 0 | 6.2e-9 | 3.45e9 | 7.3e7 | 0.028 / 0.016 | 0.81 / 0.70 |
| 10,000 | 60 | 4,215,879 | 0 | 6.2e-9 | 2.53e10 | 5.4e8 | 0.176 / 0.086 | 0.88 / 0.68 |
| 100,000 | 3 | 4,215,955 | 0 | 6.2e-9 | 1.19e11 | 8.0e8 | 0.747 / 0.346 | 1.20 / 0.71 |
| none | 0 | 4,215,954 | 0 (against HBT-HERONS) | 1.1e-6 (against HBT-HERONS) | 1.38e11 | 7.2e8 | 0.850 / 0.382 | 1.23 / 0.69 |

Largest \|dE\|/S against the definition, by form (fraction of S; lower is
better; tau is 0.00141):

| Kernel (`unbind` option) | Cap 1,000 | Cap 10,000 | Cap 100,000 |
|---|---|---|---|
| Double precision | 6.2e-9 | 6.2e-9 | 6.2e-9 |
| Double precision, `--rsqrt` | 6.2e-9 | 6.2e-9 | 6.2e-9 |
| `--kernel f32` | 1.7e-6 | 1.3e-6 | 1.3e-6 |
| `--kernel f32 --rsqrt` | 1.7e-6 | 1.3e-6 | 1.3e-6 |
| `--kernel f32f32` | 7.6e-6 | 2.4e-5 | 6.5e-5 |
| `--kernel f32f32 --rsqrt` | 8.1e-6 | 2.4e-5 | 6.5e-5 |

The two exact conditions:

| Condition | Result |
|---|---|
| A cap above the largest group (200,000) | the dump is identical to the dump without a cap (`h5diff`) |
| Groups at or below a cap of 1,000 (14,732 groups, 1,232,469 particles) | kernel: `Bound` and `Energy` equal bit for bit to the run without a cap; `unbindspec`: records identical |
| The same at 10,000 (15,121 groups, 2,325,779 particles) | equal bit for bit; identical |
| The same at 100,000 (15,178 groups, 3,912,340 particles) | equal bit for bit; identical |

Without a cap the tools behave as before the change: `unbindspec` against
the exact reference passes with the largest difference 1.09e-6 of S, and
the double-precision kernel passes against the exact reference.

The check was tried against deliberately broken kernels, built for the
host and run at cap 1,000 on the node's cores. All seven fail; the unbroken
kernel on the same backend passes.

| Change to the kernel | Bound particles (count; the definition has 4,214,992) | Groups with different bound sets (count) | of which fail part 2 (count) | Energies within tau (%) | Verdict |
|---|---|---|---|---|---|
| none (control) | 4,214,992 | 0 | 0 | 100 | pass |
| the factor f left out | 1,843,072 | 438 | 335 | 62.4 | fail |
| a sampled particle's own term kept | 4,216,688 | 286 | 283 | 89.7 | fail |
| sample in order of particle ID, no hash | 4,066,878 | 352 | 342 | 29.0 | fail |
| removed sample members not taken out | 4,220,051 | 210 | 195 | 49.2 | fail |
| f not recomputed after a removal | 4,218,689 | 201 | 173 | 37.7 | fail |
| own term kept for a particle that joins the sample | 4,215,031 | 18 | 17 | 99.85 | fail |
| another constant in the hash (a different, equally valid sample) | 4,215,071 | 285 | 264 | 29.7 | fail |

What this says:

1. **The kernel computes the written sampled method.** At three caps and in
   six forms it selects the definition's particles exactly, and in double
   precision its energies agree to 6e-9 of S. The definition recomputes every
   pass from its sample; the kernel updates its sums by the sample's change.
   The agreement checks that update.
2. **Later passes stay cheap with a cap.** They cost 2% of the first pass at
   caps of 1,000 and 10,000 and under 1% at 100,000. Recomputing every pass,
   as `unbindspec` does, costs about four times the kernel's total pair terms
   in the sampled groups (1.20e10 against 3.2e9 at a cap of 1,000).
3. **With a cap the kernel is no longer where the time goes in `unbind`.** At
   a cap of 1,000 the kernel takes 0.02 to 0.03 s of the 0.7 to 0.8 s; the
   rest is the program's serial host work and copies.
4. **The sample must not be taken in order of particle ID.** That order ends
   148,114 bound particles below the hashed order, which is itself within 962
   of the exact result.

Seen but not judged by this rule:

- Distance from the exact result, in total bound particles: 962 fewer at a
  cap of 1,000 (0.023%), 75 fewer at 10,000, 1 more at 100,000. All 13,256
  groups keep a sub-halo at every cap. Per group and per particle this is
  not measured.
- Two equally valid samples at a cap of 1,000 (the hash with another
  constant) differ in 4,545 bound particles, 0.11%: 2,233 only in one and
  2,312 only in the other.

### How far a capped result is from the exact one (measurement, 2026-11-11)

Decided 2026-11-11: the cap is chosen from a measurement, not from a pass
rule. No new run: `subhalo/capdistance.py` on the dumps of job 5903809
(double-precision kernel). The reference is the run without a cap, whose
bound particles are the exact HBT-HERONS reference's. Only the groups above
a cap are sampled and can differ; every group at or below it is identical.

Per sampled group, N is the exact bound count. "Count" is the difference in
bound count over N; "members" is the number of particles bound in exactly
one of the two results over N; \|dE\|/S is over the particles bound in both.
The most bound particle is the one of lowest energy; its shift is the
distance between the exact one and the capped one. Lower is better in every
column except the last but one.

| Cap (sources per particle) | Groups sampled (count) | Bound only in the exact result / only in the capped one (particles) | Count per group: median, 99th percentile, largest (%) | Members per group: median, 99th percentile, largest (%) | \|dE\|/S: median, 99th percentile (fraction of S) | Same most bound particle (% of sampled groups; higher is better) | Shift of the most bound particle: 99th percentile, largest (softening lengths) |
|---|---|---|---|---|---|---|---|
| 1,000 | 449 | 2,190 / 1,228 | 0.009, 0.52, 1.85 | 0.033, 0.70, 1.99 | 0.020, 0.20 | 61.5 | 3.1, 218 |
| 1,000, a second sample | 449 | 2,105 / 1,222 | 0.013, 0.59, 1.64 | 0.033, 0.78, 1.64 | 0.019, 0.23 | 62.6 | 4.1, 218 |
| 10,000 | 60 | 297 / 222 | 0.009, 0.060, 0.082 | 0.019, 0.13, 0.18 | 0.0051, 0.063 | 81.7 | 1.4, 1.5 |
| 100,000 | 3 | 4 / 5 | 0.001, 0.002, 0.002 | 0.002, 0.004, 0.004 | 0.0014, 0.013 | 66.7 (2 of 3) | 0.9, 0.9 |

The second sample is the same method with another constant in the hash
(the last of the broken kernels above, which is a valid sample). The two
samples at 1,000 differ from each other by 0.054% of a group's members at
the median and 1.24% at most, and have the same most bound particle in
49% of the sampled groups.

The most bound particle at a cap of 1,000, by group size:

| Particles in group | Groups (count) | Same most bound particle (%; higher is better) | Shift: median, 99th percentile, largest (softening lengths; lower is better) |
|---|---|---|---|
| 1,001 to 10,000 | 389 | 65.6 | 0.0, 2.2, 109 |
| 10,001 to 100,000 | 57 | 36.8 | 0.9, 98, 218 |
| above 100,000 | 3 | 0 | 3.3, 4.4, 4.4 |

What this says:

1. **Bound sets hardly depend on the cap.** At 1,000 a typical sampled group
   differs from the exact result in 0.03% of its particles and the worst in
   2%. At 10,000 the worst is 0.18%. No group gains or loses its sub-halo at
   any cap.
2. **Energies are noisy, as they must be.** The median error is 2% of S at
   1,000, 0.5% at 10,000 and 0.14% at 100,000: it falls by 3.8 to 4.0 per
   decade, a little faster than the square root of the cap. Even 100,000 is
   at tau, so no capped result can pass the exact rule.
3. **The most bound particle is not reliable at a cap of 1,000.** It is a
   different particle in 38% of the sampled groups, and in the worst groups
   it lies in another part of the group: up to 218 softening lengths away
   (2.2 Mpc in the stand-in units). At 10,000 it differs in 18% of the
   groups but is never more than 1.5 softening lengths away. HBT-HERONS
   orders the most bound particles again after unbinding for this reason
   (`RefineMostBoundParticle`); `unbind` has no such step.
4. **Two samples are as far from each other as each is from the exact
   result**, so the distances above are sampling noise of the expected size.
   The mean count difference is slightly negative in all three sampled runs
   (-0.008%, -0.012%, -0.005%); two samples do not show whether that is a
   bias.

Cost, for the choice: one pass on cosmo2b at 64 nodes is 0.010 s at a cap of
1,000, 0.092 s at 10,000 and 0.83 s at 100,000 ("The cap table in seconds").

Limits of this measurement: one input with 3 groups above 100,000 particles
and none above 153,804, where the targets have groups a thousand times
larger; one or two samples per cap; scale factor 1; HBT-HERONS at the same
caps was not run.

**The cap is 10,000 (decided 2026-11-11).** On these numbers it gives bound
sets within 0.2% and centres within 1.5 softening lengths at a tenth of the
cost of FoF; 1,000 gives bound sets within 2% but needs a centre refinement
step before its most bound particles can be used. Two later measurements at
this cap are in the next section: on marginally bound groups (the a = 1.25
input) the worst sampled group differs in 2.9% of its particles, and on a
group that spans the box the capped result is not close to the exact one.

## A scale factor other than 1, and groups at least half the box wide (2026-11-11)

Frontier job 5904186, one node, 11 min 38 s
(`subhalo/run_unbind_scale_wide.sbatch`): FoF3 on the node's cores, `unbind`
on one MI250X die, `unbindspec` and `unbindcheck` on 56 threads.

**The reference in this section is `unbindspec`, the written definition. It
is not an HBT-HERONS run.** HBT-HERONS is not built on Frontier: one attempt
on 2026-11-11 was stopped by the session's permission check and was not
repeated. So this section shows that the kernel computes the definition on
these inputs, and records what was read from HBT-HERONS's source. It does
not show that the definition is what HBT-HERONS computes at a scale factor
other than 1 or on wide groups. The HBT-HERONS runs are listed at the end.

### Where the scale factor enters (read from HBT-HERONS c728ee0, not run)

| Quantity | HBT-HERONS | `unbind` and `unbindspec` |
|---|---|---|
| Velocities | file value times a to the dataset's `a-scale exponent` (`swiftsim_io.cpp:349-363`) | the same; `tipsy2hbt` writes peculiar velocities with exponent 0 |
| Masses | file value times a to its exponent (`swiftsim_io.cpp:383-398`) | the same (exponent 0) |
| Softening | the smaller of the comoving one and the largest physical one over a (`config_parser.cpp:641-648`) | the same |
| Potential | G / a times the sum over comoving distances (`gravity_tree.cpp:165`) | `Ga = G / a` |
| Hubble term | H a (x - X), H from `H [internal units]` (`snapshot.h:146`, `subhalo_unbind.cpp:130`) | `Ha = H * a` |

`tipsy2hbt` takes a from the Tipsy header's time, multiplies Tipsy
velocities by a to make them peculiar, and writes H = 100 h
sqrt(Omega_m / a^3 + Omega_Lambda + (1 - Omega_m - Omega_Lambda) / a^2).
That velocity convention is pynbody's for N-Body Shop files. It has not been
checked against a real output at a below 1. New here: `tipsy2hbt
--scale-factor a` writes a given a instead of the header's.

### Inputs

All are made from real particles (`lambb.00500`). None is a production case.

| Name | Particles | How it is made | Groups of 20 or more | Largest group (particles) | Groups at least half the box wide: particles, extent along x, y, z (box lengths) |
|---|---|---|---|---|---|
| a0.5 | 8,062,157 | 8m-lambb and its FoF3 groups, written with `--scale-factor 0.5`: H = 123.25 km/s/Mpc, softening 0.01 Mpc | 15,181 | 153,804 | none |
| a1.25 | 8,062,157 | the same with `--scale-factor 1.25`: H = 64.67 km/s/Mpc, softening 0.008 Mpc (the physical maximum over a) | 15,181 | 153,804 | none |
| w565 | 806,216 | every tenth particle of 8m-lambb (`subsample_tipsy.py`), FoF3 `-b 0.565 -P 1` | 1,844 | 82,206 | 82,206: 0.40, 0.48, 0.55; and 61,195: 0.29, 0.35, 0.56 |
| w580 | 806,216 | the same particles, FoF3 `-b 0.58 -P 1` | 1,804 | 191,492 | 191,492: 0.58, 1.00, 0.76 |
| cube45 | 92,719 | a cube of side 0.045 around the 79,173-particle halo of 8m-lambb, treated as a periodic box of 4.5 Mpc (`subcube_tipsy.py`), FoF3 at 8m-lambb's linking length | 47 | 79,096 | 79,096: 0.71, 1.00, 1.00 |
| cube40 | 89,558 | the same with side 0.04 (4 Mpc) | 36 | 78,823 | 78,823: 0.995, 1.00, 1.00 |

- a0.5 and a1.25 are not real epochs: the positions and Tipsy velocities
  are those of the a = 1 output and only the scale factor differs. At 0.5
  the groups are more bound than at 1 (98.9% of grouped particles bound). At
  1.25 they are marginally bound (58.4%, up to 67 passes), and the softening
  takes the physical branch. The only snapshot on Frontier at a real scale
  factor other than 1 is `h34462gs.000616` (a = 0.2447, a zoom run of 3.7e9
  particles in NChilada format). It was not used: it needs an NChilada
  reader in the converter and a choice of linking length in a zoom region.
- 8m-lambb has no wide group: its widest is 8% of the box. A group at least
  half the box wide appears only when FoF percolates (w565 and w580, at 2.8
  and 2.9 times the usual linking length) or when the box is as small as
  one halo (the cubes, whose periodic boundary is artificial). An extent of
  1.00 means the group reaches all the way round the box.
- FoF3 `label_digest`: w565 c6db3522b8591d53, w580 904d16defaeb8f0a, cube45
  317c06d80c306124, cube40 3e2c10066f6666ec. Snapshots are in
  `/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/scalewide/run.5904186/<name>/snap_0000.hdf5`;
  md5 a0.5 5f8a3bc7a5ea, a1.25 a65a4daa1f59, w565 3da22001928d, w580
  2ac86ac1dbb0, cube45 7e3d4d54475c, cube40 c9891feedee4.

### Pass rule

Unchanged: the amended rule at tau = 0.00141, with the single-precision
allowance. tau was measured on 8m-lambb at a = 1 from HBT-HERONS's two
builds; it should be measured again on these inputs when their HBT-HERONS
runs exist. The kernel's distance from the definition is far below tau
everywhere except the one failure, so the value does not decide anything
here.

### Results

Without a cap. Six kernel forms: double, double with rsqrt, single summed
in double with and without rsqrt, single summed in single with and without
rsqrt. \|dE\|/S is against the definition.

| Input | Groups with a sub-halo (definition) | Bound particles (definition) | Passes per sub-halo: mean, largest | Forms that pass at tau (of 6; higher is better) | Forms whose bound sets all equal the definition's (of 6; higher is better) | Largest \|dE\|/S: double / single summed in double / single summed in single (lower is better) | Unbinding: double / single with rsqrt (s; lower is better) |
|---|---|---|---|---|---|---|---|
| a0.5 | 13,930 | 4,294,867 | 1.19, 12 | 6 | 6 | 4.7e-9 / 1.5e-6 / 7.6e-5 | 1.01 / 0.51 |
| a1.25 | 10,385 | 2,536,941 | 5.42, 67 | 6 | 4 | 8.3e-9 / 2.7e-6 / 0.018 | 2.35 / 1.42 |
| w565 | 1,561 | 514,690 | 2.35, 23 | 6 | 6 | 1.3e-8 / 2.2e-5 / 2.4e-4 | 0.23 / 0.17 |
| w580 | 1,522 | 389,629 | 2.45, 35 | 6 | 6 | 2.0e-8 / 2.2e-5 / 8.2e-4 | 0.60 / 0.42 |
| cube45 | 11 | 79,559 | 2.45, 3 | 6 | 6 | 2.2e-9 / 4.2e-7 / 5.7e-5 | 0.09 / 0.07 |
| cube40 | 9 | 79,066 | 2.67, 5 | **0** | 6 | 0.090 in every form | 0.09 / 0.07 |

- **Scale factor.** At a = 0.5 and at a = 1.25 the kernel selects the
  definition's particles and passes at tau in all six forms. The two forms
  that sum in single precision differ at a = 1.25 in 9 groups, by 11
  particles one way and 9 the other. Each of the 9 starts from a particle
  within tau S of zero and passes the amended rule; they fail the rule as
  first written. Their measured rounding is n_k = 0.0016, so the allowance
  stops at 10 tau. At a = 1 and a = 0.5 these forms had the definition's
  particles exactly: marginally bound groups are where summing in single
  precision shows.
- **Wide groups.** On w565, w580 and cube45 all six forms select the
  definition's particles and pass. On cube40 all six fail (next heading).
- **Nothing changed at a = 1.** With this section's changes `tipsy2hbt`
  rebuilds the 8m-lambb snapshot (`h5diff`: identical) and `unbind` rebuilds
  the dump of job 5903809 (`h5diff`: identical) and passes against the exact
  HBT-HERONS reference.

At the chosen cap of 10,000 sources, against `unbindspec --cap 10000`
(three forms: double, double with rsqrt, single summed in double with
rsqrt). The last column is `capdistance.py` on the kernel's own two results.

| Input | Forms that pass at tau (of 3; higher is better) | Bound particles with the cap | Bound particles without | Groups sampled (count) | Particles bound in one result only, per sampled group: median, largest (% of the group's bound count without a cap; lower is better) |
|---|---|---|---|---|---|
| a0.5 | 3 | 4,294,867 | 4,294,867 | 60 | 0, 0 |
| a1.25 | 3 | 2,538,527 | 2,536,941 | 60, of which 59 have a sub-halo | 0.23, 2.9 |
| w565 | 3 | 514,744 | 514,690 | 9 | 0.15, 0.38 |
| w580 | 3 | 422,173 | 389,629 | 7 | 0.12, 98 |
| cube45 | 3 | 79,536 | 79,559 | 1 | 0.04 |
| cube40 | 3 | 79,053 | 79,066 | 1 | 0.02 |

- The capped kernel is the capped definition on every input, cube40
  included.
- How far the cap is from the exact result depends on the input. Deeply
  bound groups (a0.5) do not change at all. Marginally bound groups (a1.25)
  change ten times more than at a = 1: up to 2.9% of a group. The group
  that spans the box in w580 has 35,117 bound particles without the cap and
  67,703 with it. That group's result is not stable in any case (below).

### The failure on cube40

`unbindcheck` on the double-precision kernel, in full. The other five forms
print the same counts and the same largest value.

```
reference: 79066 bound particles in 9 groups
candidate: 79066 bound particles in 9 groups
bound in both: 79066; only in reference: 0; only in candidate: 0
groups: 9 with equal bound sets, 0 with different bound sets, 0 with a sub-halo on one side only
|dE|/S over particles bound in both: median 4.8e-13, 99% 1.26e-10, 99.9% 8.91e-10, largest 0.0903

rule as first written, tau = 0.00141: FAIL (energies FAIL: 99.9911% within, 6 beyond 10 tau; membership pass: 0 of 0 one-sided particles outside the band; groups pass)

amended rule, tau = 0.00141
1. energies:  99.9911% of 79066 within tau*S, 6 beyond 10*tau*S: FAIL
2. groups with different bound sets: 0, of which 0 fail (0 not an end state, 0 do not start at the boundary, 0 where the replay does not end on the reference's set): pass
3. groups with a sub-halo on one side only: 0, of which 0 have more than 22 bound particles: pass
FAIL
```

Nothing was changed to make it pass: tau, the rule, `unbindcheck` and
`unbindspec` are as they were.

- **It is not precision.** The double-precision kernel gives the same
  result as the single-precision ones. The bound sets are equal, and all
  but 7 of the 79,066 energies agree to 9e-10 of S at the 99.9th percentile.
- **Where.** The 7 particles (ids 6892, 54413, 56554, 64005, 64306, 75437,
  77101) all belong to the wide group, and each lies between 0.49960 and
  0.49999 box lengths from the group's frame along one axis. Half a box
  from the frame, the Hubble term H a (x - X) jumps from +H a L / 2 to
  -H a L / 2, here from +140 to -140 km/s.
- **Why the two differ there.** A pass that removes fewer particles than it
  keeps is a correction in HBT-HERONS (`subhalo_unbind.cpp:563-589`): the
  energy gets dv . shift + shift^2 / 2 minus the removed particles'
  potential, where dv is the particle's velocity in the old frame and shift
  the old frame's velocity in the new one. The particle's nearest image is
  taken with respect to the old frame and not again with respect to the new
  one. `unbind` does the same. `unbindspec` computes every pass afresh from
  the current frame and takes the nearest image again. The two agree unless
  a particle's nearest image changes when the frame moves, which needs the
  particle to be within the frame's movement of that half-box plane. That
  can only happen in a group that reaches half a box from its own centre.
- **Settled, 2026-11-11.** HBT-HERONS's own run on cube40 has 79,094 bound
  particles. With its first particle and carried passes the definition
  gives the same sets, with energies to 7e-7 of S; computing every pass
  afresh does not. The kernel is right here, and `unbindspec --carried` is
  the reference for wide groups ("HBT-HERONS on the Mac", "Groups at
  least half the box wide").
- **What it means.** In this corner the kernel does what HBT-HERONS's
  source does and the written definition does not. That is a reading, not a
  run. HBT-HERONS's own result on cube40 decides it: if its energies for
  these 7 particles are the kernel's, `unbindspec.h` needs the correction
  form for later passes; if they are `unbindspec`'s, the kernel is wrong.
- With the cap of 10,000 this group is sampled, a sampled pass is always
  computed in full by both the kernel and the capped definition, and the
  same input passes. The corner is reachable only by a wide group at or
  below the cap.
- cube45 has the same geometry along two axes and passes. Why none of its
  particles is affected was not looked into.

### Broken kernels

Five deliberately wrong copies of `unbind` (built by
`scalewide/mutants/build.sh` on scratch), and `unbind --open`. "Caught"
means `unbindcheck` fails.

| Wrong kernel | Inputs where it is caught | Inputs where it is not |
|---|---|---|
| G instead of G / a | a0.5, a1.25 (no energy within tau) | |
| H instead of H a | a0.5 (33% of energies within tau), a1.25 (14%) | |
| Comoving softening whatever a is | a1.25 (73% within tau) | a0.5, as it must be: below a = 1 the softening is the comoving one |
| Pair separations without the nearest image | w580, cube45, cube40 | w565 |
| Frames without the nearest image | cube45, cube40 | w565, w580 |
| Periodic box ignored (`--open`) | w565, w580, cube45, cube40 | |

- The scale-factor checks see each of the three places where a enters the
  kernel. The velocity and mass exponents cannot be tested with these
  files, which write exponent 0.
- **w565 does not test the wide-group code.** Both wrong kernels give the
  definition's particles on it. Its groups are a little over half the box
  wide and their lowest-id particle is near the middle, so every particle
  is within half a box of it and the stored offsets are consistent without
  any wrapping. Only pairs more than half a box apart differ, and they move
  no energy by tau. `--open` fails there because of ordinary groups that
  cross the box edge, not because of the wide ones.
- w580 tests the pair separations but not the frames: the bound part of its
  spanning group is narrow.
- The two cubes test both. They are the wide-group check with teeth.

### The first frame of a wide group depends on its first particle

Read from HBT-HERONS c728ee0, not run:

- The first frame of a new sub-halo is its FoF group's mean position
  (`subhalo_tracking.cpp:834`), computed by `AveragePosition`
  (`snapshot.cpp:163-200`) as nearest-image offsets from the group's
  particle number 0.
- That is done in `MergeHalos` (`io/comms/exchange_and_merge.cpp:231`) one
  line before the group's particles are sorted by id (line 237). The order
  before the sort comes from an unstable sort by group
  (`swiftsim_io.cpp:975`) and from the order in which the pieces of a group
  arrive from the MPI ranks. So particle number 0 depends on the number of
  ranks and on the C++ library, not on the input alone.
- Later frames take their offsets from the first bound particle in id order
  (`subhalo_unbind.cpp:151-198`, with a stable partition), which is what
  `unbind` and `unbindspec` do for every frame.
- For a group narrower than half the box every origin gives the same frame,
  which is why this never showed on 8m-lambb.

Measured: `unbind --first-origin` (new) with six other origins per wide
group (the highest id, the two ends of the widest axis, the middle of it,
two at random; `subhalo/wideorigins.py`), each against the lowest-id origin.

| Input, group | Particles | First frame moves by up to (box lengths) | Hubble term of that move (km/s) / velocity dispersion per axis (km/s) | Bound particles, lowest-id origin | Bound particles over the six other origins: fewest to most | Most particles bound in one result only (lower is better) |
|---|---|---|---|---|---|---|
| w565, group 1 | 82,206 | 0.043 | 303 / 597 | 63,785 | 63,593 to 63,897 | 196 |
| w565, group 2 | 61,195 | 0.045 | 312 / 603 | 54,861 | 53,398 to 54,900 | 1,681 |
| w580 | 191,492 | 0.48 | 3,344 / 586 | 35,117 | 23,328 to 68,485 | 88,438 |
| cube45 | 79,096 | 0.13 | 41 / 767 | 78,874 | 78,854 to 78,867 | 48 |
| cube40 | 78,823 | 0.23 | 64 / 768 | 78,528 | 78,551 to 78,561 | 95 |

What this says:

1. **For a group that spans a real box, the first particle decides the
   answer.** On w580 the bound count runs from 23,328 to 68,485. HBT-HERONS's
   result for such a group cannot be reproduced from the input alone, and
   no kernel can be held to it particle for particle without knowing which
   particle it started from.
2. For groups a little over half the box wide (w565), four of the six other
   origins give exactly the lowest-id result, and the two at the ends of the
   group differ by up to 1,681 particles. In the cubes the Hubble term is
   small against the velocities and the choice moves under 100 particles.
3. For the comparison with HBT-HERONS on wide groups, HBT-HERONS has to
   print the id of that particle (below). `unbind --first-origin` takes it;
   `unbindspec` and `unbindcheck` do not have the option yet.
4. For our own catalogues the frame of a wide group is our choice, and none
   is made yet. Whether any production input has such a group at b = 0.2 is
   not known.

### Not done: the HBT-HERONS runs

Update, 2026-11-11: these runs are made, see "HBT-HERONS on the Mac: the
cap, the scale factors and the wide groups". The inputs were rebuilt on
the Mac instead of copied. a0.5, a1.25 and w580 were run at opening angle
0.1 instead of 0, and the single-precision runs were not made. The text
below is kept as written before the runs.

On the Mac, per input X of a0.5, a1.25, w565, w580, cube45, cube40, with
the snapshot copied from Frontier to `~/hbt-test/X/snap/snap_0000.hdf5`
(check the md5 above) and the `HBT_REAL8=ON` build of HBT-HERONS:

```sh
sed -e "s#8m-lambb/snap#X/snap#" -e "s#8m-lambb/out-exact-double#X/out-exact-double#" \
    subhalo/hbt_8m_lambb_exact.conf > ~/hbt-test/X/hbt_exact.conf
OMP_NUM_THREADS=4 mpirun -np 2 <HBT, HBT_REAL8=ON build> ~/hbt-test/X/hbt_exact.conf
subhalo/hbtref ~/hbt-test/X/out-exact-double/000 ~/hbt-test/X/exact-double ~/hbt-test/X/snap/snap_0000.hdf5
```

Then copy `exact-double.particles.bin` next to the snapshot on Frontier and
run, inside a job,

```sh
./unbindcheck <X>/snap_0000.hdf5 <X>/exact-double.particles.bin kernel_<X>_f64.hdf5 --tau 0.00141
```

on the kernel dumps of job 5904186 (same directory as the snapshots).

- a0.5 and a1.25 need nothing else. They cost about as much as the a = 1
  reference (34 minutes there); a1.25 has more passes.
- For the four wide inputs the result depends on HBT-HERONS's first
  particle, so that run also needs one added line that prints it, in
  `MergeHalos` right after `h.AverageCoordinates();`:

  ```cpp
  std::cout << "FIRSTPARTICLE " << h.Particles.size() << " " << h.Particles[0].Id << std::endl;
  ```

  It changes no result. With those lines the kernel is rerun with
  `--first-origin`. cube40 and cube45 are the ones worth running first
  (about 79,000 particles each, minutes); cube40 also settles the failure
  above. w580's group has 191,492 particles and 35 passes.
- The single-precision build's run on each input, to measure tau there.

## HBT-HERONS on the Mac: the cap, the scale factors and the wide groups (2026-11-11)

The HBT-HERONS runs the two sections above list as not done, made on the
Mac. HBT-HERONS c728ee0, the `HBT_REAL8=ON` build (GCC 16.2, Open MPI,
`HBT_DM_ONLY=ON`), 2 MPI ranks of 4 threads, the settings of
`subhalo/hbt_8m_lambb_exact.conf` (no sampling unless a cap is given,
`BoundMassPrecision 1`, energies saved). The kernel was not run here: Kokkos
is not installed on the Mac. Everything below compares HBT-HERONS with
`unbindspec`.

What it shows, in short:

- At the same cap our capped definition is as close to the exact result as
  HBT-HERONS's own sampled result.
- At a = 0.5 HBT-HERONS selects exactly the definition's particles. At
  a = 1.25 it agrees under the amended rule; that run used a tree, so it is
  agreement within tau and not an identity.
- On a group at least half the box wide HBT-HERONS computes what its source
  says: the first frame from the first particle of its list, and energies
  carried from pass to pass. With both, the definition reproduces it
  exactly on the two cubes. The cube40 failure is settled: the kernel is
  right there and `unbindspec`, as it was, is not.
- On the group that spans the box in w580, HBT-HERONS's result also depends
  on the stale-frame case that the kernel leaves out on purpose. The
  kernel's rule gives 17,597 bound particles where HBT-HERONS has 6,815.

### Runs

`subhalo/hbt_mac_run.sh <input> <tag> <cap>` writes the configuration from
`hbt_8m_lambb_exact.conf`, runs HBT-HERONS and then `hbtref`. The patched
build is the same commit with `subhalo/hbt_firstparticle.patch`, which
prints the particle each group's first frame starts from and changes no
result.

| Input | Run (tag) | Cap (sources per particle) | Tree opening angle | Build | Unbinding (s; lower is better) | Groups with a sub-halo (count) | Bound particles (count) |
|---|---|---|---|---|---|---|---|
| 8m-lambb | cap1000 | 1,000 | 0 | as cloned | 13.4 | 13,256 | 4,215,523 |
| 8m-lambb | cap1000b | 1,000 | 0 | as cloned | 12.5 | 13,255 | 4,215,188 |
| 8m-lambb | cap1000c | 1,000 | 0 | as cloned | 8.2 | 13,256 | 4,215,502 |
| 8m-lambb | cap10000 | 10,000 | 0 | as cloned | 84.1 | 13,256 | 4,216,006 |
| 8m-lambb | cap100000-open0.1 | 100,000 | 0.1 | as cloned | 538.6 | 13,256 | 4,215,958 |
| a0.5 | open0.1-double | none | 0.1 | as cloned | 939.8 | 13,930 | 4,294,867 |
| a1.25 | open0.1-double | none | 0.1 | as cloned | 899.5 | 10,386 | 2,536,961 |
| w565 | exact-double | none | 0 | patched | 922.0 | 1,561 | 514,690 |
| w580 | open0.1-double | none | 0.1 | patched | 335.5 | 1,522 | 361,326 |
| cube45 | exact-double | none | 0 | patched | 697.7 | 11 | 79,552 |
| cube40 | exact-double | none | 0 | patched | 668.4 | 9 | 79,094 |

- The times are not timing measurements. All but cap1000c shared the Mac
  with another run or with `unbindspec`.
- Opening angle 0 makes the tree walk visit every particle: all pairs.
  HBT-HERONS gives each group to one thread, at about 1e7 pair terms per
  second here (cube40's group of 78,823 particles: 6.2e9 pair terms in
  668 s), so the largest group sets the run time. The angle-0 runs of
  a0.5, a1.25 and w580 were estimated at 35, 50 to 60 and over 70 minutes.
  The a0.5 one was stopped 41 minutes in and those three inputs, and the
  cap of 100,000, were run at opening angle 0.1 instead (decided
  2026-11-11). At equal precision that angle is 4.4 times faster on
  8m-lambb (430 s against 1,886 s, single-precision build).
- What opening angle 0.1 costs is known at a = 1 ("Results", item 2 of the
  exact reference): energies within 1.5e-4 of S at the 99.9th percentile,
  and one group of 13,256 with one particle more. Such a run is not an
  exact reference; it is judged by the amended rule.
- `MaxSampleSizeOfPotentialEstimate` shuffles each group at random, so two
  capped runs of the same input differ. cap1000, cap1000b and cap1000c are
  three samples.

### Inputs

`subhalo/hbt_mac_inputs.sh` rebuilds the six inputs on the Mac with the
commands of `run_unbind_scale_wide.sbatch` (FoF3 in one process with 8
threads). Nothing was copied from Frontier.

- FoF3's `label_digest` equals Frontier's on all four FoF3 runs (w565,
  w580, cube45, cube40), and so do the group counts and largest groups.
- `unbindspec` gives Frontier's counts on all six: a0.5 13,930 groups and
  4,294,867 bound particles, a1.25 10,385 and 2,536,941, w565 1,561 and
  514,690, w580 1,522 and 389,629, cube45 11 and 79,559, cube40 9 and
  79,066.
- The snapshots' md5 values do not equal the ones recorded above, and
  cannot: the same input built twice on the Mac has two md5 values and
  `h5diff` finds no difference. `tipsy2hbt`'s files are not byte-for-byte
  reproducible, so the md5 is not a check of the input. The digests and
  `h5diff` are.

### Tools added

| File in `subhalo/` | What it does |
|---|---|
| `unbindspec --first-origin <file>` | lines "group id, particle id": the particle a group's first frame takes its nearest-image offsets from (the format of `unbind --first-origin`) |
| `unbindspec --carried` | the passes as HBT-HERONS's source carries them (`ubs::unbindCarried` in `unbindspec.h`): after the first pass that removes fewer particles than it keeps, every pass corrects the stored energies, and the stale-frame case is kept as the source has it. It prints the groups that case happens in |
| `unbindspec --carried-kernel` | the same with the kernel's one difference: the pass after one that removed at least as many particles as it kept is computed in full |
| `hbt_firstparticle.patch` | the print added to HBT-HERONS's `MergeHalos` |
| `first_origins.py` | the `FIRSTPARTICLE` lines of a log as a `--first-origin` file |
| `capdist_bin.py` | the measures of `capdistance.py` on two `.particles.bin` files; `--first` also judges the particle HBT-HERONS reports as most bound (a group's first record) |
| `centre_rank.py`, `group_diff.py` | the rank of the exact most bound particle in a capped energy order; the groups in which two results differ |
| `hbt_mac_run.sh`, `hbt_mac_inputs.sh`, `hbt_mac_compare.sh` | the runs, the inputs and the comparisons of this section |

- Without the new options `unbindspec` writes the same bytes as before on
  8m-lambb. `unbindcheck` has none of them: it replays the definition as
  written. So a wide input is judged here by `unbindcmp` (the rule as first
  written) and listed group by group.
- `--carried` on 8m-lambb at a = 1 gives the exact reference's bound sets
  with the largest \|dE\|/S 1.09e-6, the same as without it, and the
  stale-frame case in no group.
- `capdist_bin.py` gives the table of `capdistance.py` for our definition
  at a cap of 10,000, figure for figure.
- mpirun prints the two ranks' `FIRSTPARTICLE` lines into each other now
  and then. `first_origins.py` uses a line only if it is complete and the
  particle's group has the printed size. On w565 that left 19 groups of at
  most 307 particles without an origin, on w580 one of 33 particles, on the
  cubes none. Those groups then start from their lowest id; every form
  agrees with HBT-HERONS on w565, so it made no difference there.

### HBT-HERONS with a cap, against the exact result

The exact HBT-HERONS reference is the same program with the same settings
and no cap, so every difference is the cap's (and, at 100,000, the opening
angle's: one particle in group 2536). Groups at or below the cap are
identical to the exact result in every run. The columns are those of "How
far a capped result is from the exact one", per sampled group, with the
rows of our capped definition repeated for comparison. Lower is better
except in the columns marked.

| Result | Cap | Bound only in the exact result / only in the capped one (particles) | Members per group: median, 99th percentile, largest (%) | \|dE\|/S: median, 99th percentile (fraction of S) | Lowest capped energy is the exact most bound particle (% of sampled groups; higher is better) | Its largest shift (softening lengths) | Reported most bound particle is the exact one (%; higher is better) | Its largest shift (softening lengths) |
|---|---|---|---|---|---|---|---|---|
| HBT-HERONS, run 1 | 1,000 | 1,878 / 1,447 | 0.037, 1.13, 3.77 | 0.019, 0.21 | 60.1 | 217 | 94.7 | 2.4 |
| HBT-HERONS, run 2 | 1,000 | 2,197 / 1,431 | 0.041, 1.39, 100 | 0.020, 0.23 | 58.9 | 148 | 93.5 | 2.4 |
| HBT-HERONS, run 3 | 1,000 | 1,980 / 1,528 | 0.036, 1.40, 5.98 | 0.019, 0.23 | 64.1 | 218 | 93.5 | 217 |
| Our definition | 1,000 | 2,190 / 1,228 | 0.033, 0.70, 1.99 | 0.020, 0.20 | 61.5 | 218 | no such step | no such step |
| Our definition, second sample | 1,000 | 2,105 / 1,222 | 0.033, 0.78, 1.64 | 0.019, 0.23 | 62.6 | 218 | no such step | no such step |
| HBT-HERONS | 10,000 | 278 / 330 | 0.019, 0.24, 0.27 | 0.0056, 0.066 | 75.0 | 1.5 | 98.3 | 1.5 |
| Our definition | 10,000 | 297 / 222 | 0.019, 0.13, 0.18 | 0.0051, 0.063 | 81.7 | 1.5 | no such step | no such step |
| HBT-HERONS, opening angle 0.1 | 100,000 | 4 / 8 | 0.003, 0.003, 0.003 | 0.0013, 0.012 | 33.3 (1 of 3) | 0.9 | 100 (3 of 3) | 0 |
| Our definition | 100,000 | 4 / 5 | 0.002, 0.004, 0.004 | 0.0014, 0.013 | 66.7 (2 of 3) | 0.9 | no such step | no such step |

The "reported most bound particle" is HBT-HERONS's own, after its centre
refinement (`RefineBindingEnergyOrder`, `subhalo_unbind.cpp:395-425` and
`715-733`), read from the source: after the last pass the bound particles
are sorted by their sampled energies; the max(cap, 10% of the bound count)
lowest of them get a new energy from a tree of those particles alone, in
the same frame, and are sorted again. The saved energies are not updated.

Where the exact most bound particle ranks in a capped result's energy order
(rank 0 is the lowest capped energy; lower is better):

| Result | Cap | Sampled groups (bound count above the cap) | Rank 0 (% of groups; higher is better) | Below rank 10 (%) | Largest rank |
|---|---|---|---|---|---|
| HBT-HERONS, runs 1, 2, 3 | 1,000 | 439 | 59.7, 59.0, 63.3 | 96.1, 96.8, 97.0 | 46, 54, 109 |
| Our definition | 1,000 | 439 | 60.6 | 97.0 | 66 |
| HBT-HERONS | 10,000 | 58 | 74.1 | 100 | 4 |
| Our definition | 10,000 | 58 | 81.0 | 100 | 6 |
| HBT-HERONS / our definition | 100,000 | 3 | 33.3 / 66.7 | 100 / 100 | 1 / 1 |

What this says:

1. **Our capped result is as close to the exact one as HBT-HERONS's.** At
   10,000 the worst sampled group differs in 0.18% of its particles with our
   sample and 0.27% with HBT-HERONS's; the energy errors are the same size
   (median 0.5% of S). At 1,000 ours is the closer of the two in the worst
   groups. A sample chosen by a hash of the particle id costs nothing in
   accuracy against HBT-HERONS's shuffle, and it is the same on every run.
2. **HBT-HERONS's result at its default cap changes from run to run.** The
   three runs at 1,000 differ from the exact result by 3,325, 3,628 and
   3,508 particles, and in run 2 one group (395: 1,183 particles, 24 bound
   in the exact result) has no sub-halo. Runs 1 and 2 differ from each
   other in 0.06% of a sampled group's particles at the median and 2.3% at
   the 99th percentile.
3. **The centre needs the refinement, and the refinement is not always
   enough.** Without it the lowest capped energy is the exact most bound
   particle in about 60% of the sampled groups at 1,000, for HBT-HERONS as
   for us, and it can lie in another part of the group. With it HBT-HERONS
   has the exact particle in 94 to 95%, and in runs 1 and 2 it is never more
   than 2.4 softening lengths away. In run 3 two groups (11 and 25, of
   54,350 and 25,246 particles) have it 165 and 217 softening lengths away.
   Both groups have a second density peak whose most bound particle is at
   rank 8 and rank 20 of the exact energy order, 0.08 and 0.13 of S above
   the lowest, and the refinement took that one. Its energies come from the
   inner 10% alone, so they are not the energies the exact order is in.
   The refined centres of two runs agree in 96 to 97% of the groups. At
   10,000 the refined centre is the exact one in 59 of 60 groups and within
   1.5 softening lengths in the other.
4. **The exact most bound particle is always near the top of the capped
   order**: among the lowest 110 capped energies at 1,000 in all four
   results, and among the lowest 7 at 10,000. A refinement that gives the
   few hundred lowest particles their energy from all the bound particles
   would find it on this input, at (bound count) times (a few hundred) pair
   terms: for a group of 185 million particles and 1,000 candidates, 1.9e11,
   about one die-second. HBT-HERONS's step on that group is a tree of 18.5
   million particles; by all pairs it would be 3.4e14 pair terms. This is a
   proposal read off these files. It is not built or tested.

Limits: one input whose largest group has 153,804 particles; one to three
samples per cap; scale factor 1; the 100,000 run used a tree.

### A scale factor other than 1

HBT-HERONS at opening angle 0.1 against the definition
(`hbt_mac_compare.sh a0.5 open0.1-double`, the same for a1.25). The
definition is the reference and HBT-HERONS the candidate, so the amended
rule replays the definition.

| Input | Bound particles: HBT-HERONS / definition | Groups with different bound sets (count; lower is better) | Particles bound only in the definition / only in HBT-HERONS | \|dE\|/S over particles bound in both: 99.9th percentile, largest | Rule as first written | Amended rule |
|---|---|---|---|---|---|---|
| a0.5 | 4,294,867 / 4,294,867 | 0 of 13,930 | 0 / 0 | 4.6e-5, 3.8e-4 | pass | pass |
| a1.25 | 2,536,961 / 2,536,941 | 18 of 10,385, and one group with a sub-halo only in HBT-HERONS | 39 / 59 | 0.0042, 0.106 | fail | pass |

- **a = 0.5: HBT-HERONS selects the definition's particles, all 4,294,867
  of them, group by group.** The energy differences are the tree's.
- **a = 1.25: 18 groups differ**, by 1 to 24 particles, in groups of 70 to
  37,305 bound particles. Each starts from a particle within tau S of zero
  at the pass where the two part, the definition's own energies on
  HBT-HERONS's sets are all within tau S, and every one of HBT-HERONS's
  sets is an end state. The group on one side only (11994) has 21 bound
  particles in HBT-HERONS and none in the definition, inside the allowance
  of 2 above the minimum. It fails the rule as first written, as the
  opening-angle-0.1 run at a = 1 did with its one group. Marginally bound
  groups have many particles near zero energy, so the tree's error moves
  more of them.
- The carried passes equal the fresh ones on both inputs (largest
  difference 1.7e-14 of S, the same bound sets), and the stale-frame case
  happens in no group.
- So where a enters (G / a, H a, the softening, the velocities), the
  definition is what HBT-HERONS computes: exactly in the bound sets at 0.5,
  and to within the amended rule at 1.25. An identity at 1.25, of the kind
  shown at a = 1, needs the opening-angle-0 run (about an hour on the Mac).
- tau was not measured again on these inputs.

### Groups at least half the box wide

`hbt_mac_compare.sh <input> <tag> wide` runs the definition four ways and
compares each with HBT-HERONS. "First particle" is the one HBT-HERONS
printed for the group.

| Input (HBT-HERONS's bound particles) | As written: lowest id, every pass afresh | First particle, every pass afresh | First particle, carried as in HBT-HERONS (`--carried`) | First particle, carried with the kernel's rule (`--carried-kernel`) |
|---|---|---|---|---|
| cube40 (79,094) | 79,066 bound; 84 particles differ; fails | 79,038; 56 differ; fails | 79,094; identical sets; largest \|dE\|/S 6.8e-7; passes | the same as `--carried` |
| cube45 (79,552) | 79,559; 35 differ; fails | 79,538; 14 differ; fails | 79,552; identical sets; 5.9e-7; passes | the same as `--carried` |
| w565 (514,690) | 514,690; identical sets; 1.7e-6; passes | the same | the same | the same |
| w580 (361,326; opening angle 0.1) | 389,629; 34,431 differ; fails | 365,414; 13,458 differ; fails | 361,328; 2 differ; fails on the 24 energies of one group of 29 | 372,109; 19,601 differ; fails |

"Differ" counts the particles bound in exactly one of the two results.
"Fails" and "passes" are the rule as first written at tau = 0.00141.

The group that spans the box in w580 (191,492 particles; HBT-HERONS's first
particle is id 90774):

| How its passes are computed | Bound particles |
|---|---|
| HBT-HERONS (opening angle 0.1) | 6,815 |
| First particle, carried as in HBT-HERONS's source | 6,816 |
| First particle, carried with the kernel's rule | 17,597 |
| First particle, every pass afresh | 10,902 |
| The definition as written (lowest id, every pass afresh) | 35,117 |

What this says:

1. **HBT-HERONS computes what its source says on wide groups.** With its
   first particle and its carried passes the definition gives its bound
   sets exactly on cube40, cube45 and w565, with energies to 7e-7 of S, and
   on w580 to two particles: one in the spanning group (6,816 against
   6,815) and one in a group of 29 (group 1073, where 24 energies are then
   beyond 10 tau). Both are single particles at the boundary in a run that
   used a tree.
2. **The cube40 failure is settled.** The kernel carries energies as
   HBT-HERONS does, and that is what HBT-HERONS's own result on cube40
   agrees with. `unbindspec` as written, which computes every pass afresh,
   is not what HBT-HERONS computes on a wide group. On narrow groups the
   two forms are the same (a = 1, 0.5, 1.25 and w565 above). For wide groups
   the reference to hold a kernel to is `unbindspec --carried` with
   HBT-HERONS's first particles, not the plain `unbindspec` of job 5904186.
3. **cube45 is affected too.** With the lowest id as the first particle the
   kernel and the definition agreed there. With HBT-HERONS's first particle
   (id 29658) the fresh and the carried passes differ in 14 particles.
4. **The kernel's rule equals HBT-HERONS's on the cubes and on w565**: the
   stale-frame case happens in none of their groups. So `unbind
   --first-origin` should pass against these three references. That is a
   prediction from the CPU form of the rule; the kernel has not been run
   against them.
5. **On the spanning group of w580 the stale-frame case decides the
   answer.** It happens in that group and in no other. HBT-HERONS, which
   corrects from a frame two passes old there, ends with 6,815 bound
   particles; the kernel's rule, which computes that pass in full, ends
   with 17,597. The kernel cannot reproduce HBT-HERONS on this group unless
   it copies that case. Whether it should is not decided. The group's
   result is not stable in any case: the five ways above run from 6,815 to
   35,117.
6. A reference and its first particles belong together. The first particle
   depends on the number of ranks and on the C++ library, so an HBT-HERONS
   run on Frontier prints its own.

First particles of the wide groups in these runs: cube40's group of 78,823
particles id 71505, cube45's of 79,096 id 29658, w565's of 82,206 id 577276
and of 61,195 id 72138, w580's of 191,492 id 90774.

### Files (on the Mac, not in the repo)

In `~/hbt-test/<input>/`:

| File | What it is |
|---|---|
| `snap/snap_0000.hdf5` | the input, rebuilt here |
| `<tag>.particles.bin`, `<tag>.fof3.particles.bin`, `<tag>.groups.txt` | HBT-HERONS's result for the run `<tag>` of the table of runs, in `hbtref`'s layouts |
| `hbt_<tag>.conf`, `hbt_<tag>.log` | its configuration and log; on the wide inputs the log has the `FIRSTPARTICLE` lines |
| `origins-<tag>.txt` | the first particles of that run, for `--first-origin` |
| `spec*.particles.bin` | the definition's results |
| `compare-<tag>.log` | the output of `hbt_mac_compare.sh` |

`~/hbt-test/8m-lambb/` also has the capped definition's results
(`spec-cap1000`, `spec-cap10000`, `spec-cap100000`), which equal the
kernel's bound sets of job 5903809 in count.

To hold the kernel to these on Frontier, copy an input's
`<tag>.particles.bin` and `origins-<tag>.txt`, run `unbind --first-origin
origins-<tag>.txt --bound-fraction 1 --dump`, then `unbindcheck` on the
dump. On a wide input its verdict holds as long as no group's bound set
differs: only differing groups are replayed, and the replay is the
definition as written.

### Not done

- The opening-angle-0 runs of a0.5, a1.25 and w580, and tau measured again
  on the six inputs (the single-precision build's runs).
- The kernel against these references. It needs Frontier.
- `--first-origin` and the carried passes in `unbindcheck`, so that a wide
  input can be judged by the amended rule.
- A decision on the stale-frame case: copy HBT-HERONS there or keep the
  full pass, and whether to report the case to HBT-HERONS's authors.
- A centre refinement step in `unbind`, and the test of item 4 under the
  cap.

## Open

- Done 2026-11-11: the comparison of `unbind` with HBT-HERONS. All six
  forms of the kernel select the exact reference's 4,215,954 particles in
  13,256 groups and pass both rules at tau (jobs 5903512 and 5903660); the
  single-precision allowance was measured and changes no verdict. The
  pair-term rate is measured: 2.0e11 per die.
- Done 2026-11-11: capped sampling in `unbind_groups.h` (`unbind --cap`),
  held to a written definition: 18 runs at three caps pass at tau with the
  definition's particles exactly (job 5903809).
- Measured 2026-11-11: how far a capped result is from the exact one, per
  group ("How far a capped result is from the exact one"). **The cap is
  10,000 (decided 2026-11-11).** HBT-HERONS at the same caps: done
  2026-11-11 on the Mac. Our capped result is as close to the exact one
  as HBT-HERONS's own, and its centre refinement gives the exact most
  bound particle in 59 of 60 sampled groups at 10,000 ("HBT-HERONS with a
  cap, against the exact result"). Not done: a refinement step in
  `unbind`.
- Done 2026-11-11 against the written definition only (job 5904186, "A
  scale factor other than 1, and groups at least half the box wide"): the
  kernel at a = 0.5 and a = 1.25, and on four inputs with groups at least
  half the box wide, with and without the cap. Everything passes except
  one input. Open from it:
  - the HBT-HERONS runs on the six inputs: done 2026-11-11 on the Mac
    ("HBT-HERONS on the Mac"). At a = 0.5 HBT-HERONS has the definition's
    particles exactly; at a = 1.25 it passes the amended rule (18 groups
    differ, a tree run). Not done: the opening-angle-0 runs of a0.5, a1.25
    and w580, tau on these inputs, and the kernel against these
    references;
  - cube40: settled. HBT-HERONS's own result is the carried form's, so
    the kernel is right and the reference for wide groups is `unbindspec
    --first-origin --carried`;
  - HBT-HERONS's first frame of a wide group depends on a particle the
    input does not fix. It is printed now (`hbt_firstparticle.patch`) and
    `unbindspec` takes it. Not done: the same in `unbindcheck`;
  - the group that spans the box in w580: HBT-HERONS's 6,815 bound
    particles come from its stale-frame case; the kernel's rule gives
    17,597. Whether the kernel copies that case, and whether it is
    reported to HBT-HERONS's authors, is not decided;
  - which frame our own catalogues give a wide group, and whether any
    production input has one at b = 0.2 (8m-lambb's widest group is 8% of
    the box);
  - a real epoch: `h34462gs.000616` (a = 0.2447) is on Frontier but needs
    an NChilada reader in the converter; `tipsy2hbt`'s velocity convention
    is unchecked at a below 1;
  - marginally bound groups are where the cap and single-precision sums
    cost most (a1.25: up to 2.9% of a sampled group; 9 groups differ when
    summing in single precision). Real high-redshift groups may or may not
    be like that.
- lambb's box size in Mpc, for physical units.
- The decision on when steps 1-2 start.
