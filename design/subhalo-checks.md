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

## Open

- The per-group comparison of `unbind` with HBT-HERONS run with sampling
  off (step 1, "Not done"). The pair-term rate is measured: 2.0e11 per die.
- lambb's box size in Mpc, for physical units.
- The decision on when steps 1-2 start.
