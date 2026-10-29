#!/bin/bash
# submit_sweep.sh <fof3|fof3cpu|arborx|swift> <dset> "<node counts>"
#
# One arm of the campaign. Walltime is chosen from the node count because
# Frontier's limit is tiered (<=91 nodes: 2 h; 92-183: 6 h; 184+: 12 h) and
# sbatch REFUSES a job asking for more than its tier allows -- asking for 3 h
# at 16 nodes is a submission error, not a long job.
#
# The linking length comes from scripts/linklen.env, written by linklen.sh
# from FoF3's own output. A dataset with no entry there is refused rather than
# run with a guessed b.
set -u
CMP=/lustre/orion/csc710/scratch/rrao/compare
ARM=${1:?arm}; DSET=${2:?dset}; NODES=${3:?node list}

# The FoF3 arm runs against the charm build named by CHARMB and the binary
# named by BIN, which MUST agree (liblci.so resolves by SONAME through
# LD_LIBRARY_PATH, not from the path baked into the executable, so a
# mismatched pair silently runs the wrong LCI). Defaults:
#   reconverse-linux-x86_64-amd-cmp2 + ./FoF3.cmp2   upstream LCI 06748025,
#                                                    allgather OFI bootstrap
#   reconverse-linux-x86_64-amd-cmp  + ./FoF3.cmp    the local lci-patched
#                                                    fork; every result
#                                                    through 2026-09-02

[ -f "$CMP/scripts/linklen.env" ] && . "$CMP/scripts/linklen.env"
eval "LL=\${LINKLEN_$DSET:-}"
if [ -z "${LL:-}" ] && [ "$ARM" != fof3 ] && [ "$ARM" != fof3cpu ]; then
  echo "REFUSING: no linking length known for $DSET." >&2
  echo "  Run the FoF3 arm first, then scripts/linklen.sh." >&2
  exit 1
fi

wall() {  # nodes -> walltime inside Frontier's tier
  if   [ "$1" -le 91 ];  then echo 02:00:00
  elif [ "$1" -le 183 ]; then echo 04:00:00
  else                        echo 06:00:00; fi
}

for N in $NODES; do
  W=$(wall "$N")
  case "$ARM" in
    fof3)
      sbatch --nodes=$N --time=$W --job-name=cmpfof3_${DSET}_${N}n \
        --export=ALL,DSET=$DSET,PPN=7,LEAF=128,NDEV=7,POLL=1,ITERS=1,SCAP=128,UFSIZES=0,CHARMB=${CHARMB:-reconverse-linux-x86_64-amd-cmp2},BIN=${BIN:-./FoF3.cmp2} \
        $CMP/scripts/run_fof3.sbatch ;;
    fof3cpu)
      # CPU-only arm, for the like-for-like comparison against SWIFT (which
      # has no GPU FoF). Same shape, device phase 1 off.
      sbatch --nodes=$N --time=$W --job-name=cmpf3cpu_${DSET}_${N}n \
        --export=ALL,DSET=$DSET,PPN=7,LEAF=128,NDEV=7,POLL=1,ITERS=1,SCAP=128,UFSIZES=0,CHARMB=${CHARMB:-reconverse-linux-x86_64-amd-cmp2},BIN=${BIN:-./FoF3.cmp2},NOGPU=1 \
        $CMP/scripts/run_fof3.sbatch ;;
    arborxhacc)
      # ArborX's single-rank kernel + our own brick decomposition, overload
      # halo and distributed stitch -- the way ArborX is used at scale, and
      # the only ArborX configuration that returns the correct answer here.
      sbatch --nodes=$N --time=$W --job-name=cmpaxh_${PARTITION:-uniform}_${DSET}_${N}n \
        --export=ALL,DSET=$DSET,EPS=$LL,PARTITION=${PARTITION:-uniform} \
        $CMP/scripts/run_arborx_hacc.sbatch ;;
    arborx)
      sbatch --nodes=$N --time=$W --job-name=cmpax_${DSET}_${N}n \
        --export=ALL,DSET=$DSET,EPS=$LL \
        $CMP/scripts/run_arborx.sbatch ;;
    swift)
      # 4 ranks x 14 threads = 56 cores/node, the same core count FoF3 uses
      # (8 processes x ppn 7). This shape is fixed for the whole SWIFT curve;
      # the 1x56 / 2x28 / 8x7 alternatives were measured in the shape sweep
      # (jobs 5404290-91 and 5404435-36) and 4x14 was the best of them on
      # cosmo2b at 8 nodes (280 s vs 318/362/548 s).
      RPN=${RPN:-4}; THREADS=${THREADS:-14}
      # cdim must give many more top-level cells than ranks for SWIFT's
      # ParMETIS partition to have anything to work with.
      NR=$((N * RPN))
      # TOPCELLS is NO LONGER computed here. max_top_level_cells has to
      # satisfy the 64-proxy bound (cdim >= 5 * max rank-grid side), which
      # depends on the grid run_swift.sbatch pins, so the derivation lives
      # there. Setting TOPCELLS in the environment still overrides it.
      TOP=${TOPCELLS:-}
      if false; then
        # >= 64 top-level cells per rank. At 8 cells/rank METIS failed
        # outright during SWIFT's initial domain decomposition:
        #   ***Cannot bisect a graph with 0 vertices!
        # followed by a segfault on one rank and SIGTERM everywhere else
        # (jobs 5404314-25). SWIFT partitions the top-level cell GRAPH, so it
        # needs many more cells than ranks for the recursive bisection to
        # have anything to cut. Each rank allocates the whole top-level array
        # (sizeof(cell) = 896 B), so cdim 64 costs 235 MB/rank -- affordable.
        # Target ~64 top-level cells per rank, with an EXACT cdim rather
        # than a power of two -- the power-of-two ladder overshoots badly
        # (1024 ranks landed on cdim 64 = 256 cells/rank, and SWIFT's task
        # count scales with cells, ~127 tasks/cell, which contributed to a
        # single-rank OOM kill at 128 nodes). Too FEW cells is also fatal:
        # METIS aborts with "Cannot bisect a graph with 0 vertices!" below
        # about 32/rank. 64/rank sits in the window that worked at 64 ranks.
        TOP=$(python3 -c "import math;print(max(16,int(math.ceil(($NR*64)**(1.0/3.0)))))")
      fi
      # One core per SWIFT thread, no SMT -- the same shape FoF3 uses
      # (ppn 7 on 8 processes = 56 of the 64 cores). Both --ntasks-per-node
      # and --cpus-per-task MUST be requested here, not on the srun line.
      sbatch --nodes=$N --time=$W --job-name=cmpsw_${DSET}_${N}n \
        --ntasks-per-node=$RPN --cpus-per-task=$THREADS \
        --export=ALL,DSET=$DSET,LINKLEN=$LL,RPN=$RPN,THREADS=$THREADS,MINGROUP=${MINGROUP:-32},DDTYPE=${DDTYPE:-grid},DDADAPT=${DDADAPT:-1},DDUSEMETIS=${DDUSEMETIS:-0} \
        $CMP/scripts/run_swift.sbatch ;;
    *) echo "unknown arm $ARM" >&2; exit 2 ;;
  esac
done
