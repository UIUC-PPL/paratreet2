# Frontier instructions: 2B labeling A/B (+ phaseA skew split)

For Ritvik, written 2026-08-11. Two measurements in one job, at 2B on
16 or more Frontier nodes:

1. A/B of the DISTRIBUTED union-find labeling-scatter batching
   (unionfind branch `batch-labeling`). This code runs only under
   `-u dist`; the serialized-global union-find never executes it.
2. The new phaseA skew-split line (INDEPENDENT of the union-find
   mode), which decides the phaseA rebalancing design
   (design/phasea-reassignment.md). Every run prints it for free.

## Before anything: two behavior changes on main you must know

- **`-u serial` is now the fof3 default** (2026-08-10). Your sweep
  scripts pass no `-u` flag, which previously meant dist. For this A/B
  — and for any future dist sweep — add `-u dist` explicitly.
- Output strings changed with the TreePiece rename: the config key is
  `nTreePiecesMin` (short `-n` unchanged), the `[Meta]` label is
  `n_treepiece`, startup prints "Created N TreePieces". Check parsing
  scripts.

## Build (two binaries from ONE paratreet2 commit)

Use paratreet2 main at 2cd8ca6 or later. `make clean` everywhere after
pulling — headers were renamed (Subtree.h -> TreePiece.h) and the
Makefiles have no dependency tracking.

Baseline binary (NOTE 2026-08-11: Ritvik MERGED batch-labeling into
fof_with_aggregation, merge 40d7ecc — the baseline arm must therefore
pin the PRE-MERGE tip):

    cd unionfind && git checkout 4ce7dec   # pre-merge fof_with_aggregation
    make clean && make CHARM_DIR=<charm> AGGREGATION= PROFILE=
    cd ../paratreet2/src && make clean && make
    cd ../fof && make clean && make
    cd ../examples/fof3 && make clean && make
    cp FoF3 <staging>/FoF3.labelbase

Batched binary (ONLY the unionfind library differs):

    cd unionfind && git checkout fof_with_aggregation   # now includes the
    git pull                                            # merged batching
    make clean && make CHARM_DIR=<charm> AGGREGATION= PROFILE=
    # STALE-LIB TRAP: fof/ and examples/fof3 do NOT depend on the .a —
    # make clean BOTH before rebuilding, or you relink the old library.
    cd ../paratreet2/fof && make clean && make
    cd ../examples/fof3 && make clean && make
    cp FoF3 <staging>/FoF3.batchlabel
    cd ../../unionfind && git checkout fof_with_aggregation   # restore

## Run (your standard 2B line, plus -u dist)

Interleave the arms, 2+ reps each, e.g.:

    srun --mpi=cray_shasta --network=job_vni --unbuffered \
      --cpu-bind=none --distribution=block:block \
      $BIN -f <2B input> -d oct -u dist +ppn 14 \
      +pemap <your 16-per-node map> \
      +lci_ndevices 7 +backend_poll_thread 2 +traceoff

with `$BIN` alternating FoF3.labelbase / FoF3.batchlabel. Same env as
your sweeps (LCI_ATTR_BACKEND=ofi, FI_CXI_RX_MATCH_MODE=hybrid,
PMI_MAX_KVS_ENTRIES=4194304).

## Readout

- Correctness (both arms, every rep): `grep -c 'components: 424897832'`
  must be 1, and the two arms' full FOF3STAT components histograms must
  be identical.
- The A/B number: the `uf2` field of `FOF3STAT time_s: uf2_setup ...
  uf2 ... relabel ...` — that bracket contains find_components and
  therefore the labeling scatter this change batches. Also worth
  grabbing: `uf2_setup` and the boss-count line ("Number of components
  found") as a sanity check that both arms merged identically.
- The skew line (report from EVERY run, any arm): `FOF3STAT
  phaseA_skew: within W cross C global G size_r R max_piece_n N`.
  within = worst process's internal phaseA max/avg (what shared-memory
  rebalancing can recover); cross = hottest process avg over global avg
  (what only cross-process placement can recover). At 80M/480 PEs on
  Anvil we measured within 1.44-1.53 against cross 1.14-1.20; the 2B
  point at scale is what the phaseA design is gated on.

## What we expect

The batching collapses the labeling scatter from one message per
requesting vertex (~1400 from one chare at 2B on Anvil) to at most one
message per (source chare, destination chare) pair, and the
parent-cache dedup removes same-parent duplicates entirely. The uf2
bracket at 2B has been highly variable; if the means do not separate,
the per-rep numbers still bound the batching as not-harmful, which is
enough to merge the branch on correctness grounds.
