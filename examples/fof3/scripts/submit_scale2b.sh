#!/bin/bash
# Submit the 2B node-count scaling sweep, 8 -> 128 nodes, one job per
# point, all from the SAME run_fof3_scale2b_gpu.sbatch so the shape cannot
# drift between points.  Config comes in as arguments so the record of what
# was swept is this command line.
#
#   ./submit_scale2b.sh LEAF NDEV POLL PPN [HELPER_MODE]
#   e.g. ./submit_scale2b.sh 128 7 1 7          (the shipping GPU config)
#
# OUTDIR (default /lustre/orion/csc710/scratch/rrao/scale2b-<date>) must not
# exist yet: one sweep, one directory, nothing mixed in. NODES overrides the
# node list (default "128 64 32 16 8"; 128 first, it waits longest).
#
# A sweep is a baseline, so it refuses a dirty paratreet2 tree and records
# the whole stack in $OUTDIR/STACK.txt, and it runs a FROZEN copy of the
# binary from $OUTDIR so a rebuild mid-sweep cannot change later points.
set -e
LEAF=${1:?leaf} NDEV=${2:?ndev} POLL=${3:?poll} PPN=${4:?ppn} HELPER_MODE=${5:-auto}
NODES=${NODES:-128 64 32 16 8}
OUTDIR=${OUTDIR:-/lustre/orion/csc710/scratch/rrao/scale2b-$(date +%F)}
PT=/ccs/home/rrao/paratreet2
CR=/ccs/home/rrao/charm_reconverse
CHARM=${FOF3_CHARM_HOME:-$CR/reconverse-linux-x86_64-amd}

if ! git -C "$PT" diff --quiet HEAD --ignore-submodules=dirty; then
  echo "ABORT: paratreet2 has uncommitted changes; a baseline must be a commit." >&2
  git -C "$PT" status --short --untracked-files=no >&2
  exit 1
fi
if [ -e "$OUTDIR" ]; then
  echo "ABORT: $OUTDIR already exists; pick a fresh OUTDIR." >&2
  exit 1
fi
mkdir -p "$OUTDIR"
cp -p "$PT/examples/fof3/FoF3" "$OUTDIR/FoF3"
FOF3_BIN=$OUTDIR/FoF3

{
  echo "date        $(date -Is)"
  echo "command     $0 $*   (NODES='$NODES')"
  echo "paratreet2  $(git -C "$PT" describe --always --dirty=-dirty) $(git -C "$PT" log -1 --format='%h %ad %s' --date=short)"
  echo "unionfind   $(git -C /ccs/home/rrao/unionfind log -1 --format='%h %ad %s' --date=short)"
  echo "htram       $(git -C /ccs/home/rrao/htram log -1 --format='%h %ad %s' --date=short)"
  echo "charm       $(git -C "$CR" log -1 --format='%h %ad %s' --date=short) ($(git -C "$CR" rev-parse --abbrev-ref HEAD))"
  echo "reconverse  $(git -C "$CR/contrib/reconverse" log -1 --format='%h %ad %s' --date=short)"
  echo "lci         $(git -C "$CHARM/_deps/lci-src" log -1 --format='%h %ad %s' --date=short 2>/dev/null || echo unknown)"
  echo "charm build $CHARM"
  echo "binary      $FOF3_BIN md5=$(md5sum "$FOF3_BIN" | cut -c1-12) built $(date -r "$PT/examples/fof3/FoF3" -Is)"
} > "$OUTDIR/STACK.txt"
cat "$OUTDIR/STACK.txt"

# NOTHING with a comma in it crosses --export -- sbatch splits the value
# on commas and drops the tail.  run_fof3_scale2b_gpu.sbatch derives PEMAP
# and FOF_HELPER_CPUS from PPN and HELPER_MODE instead.
cd "$(dirname "$0")"
for N in $NODES; do
  sbatch --nodes=$N --job-name=scale2b_$N \
    --output="$OUTDIR/%x.%j.out" --error="$OUTDIR/%x.%j.err" \
    --export=ALL,PPN=$PPN,LEAF=$LEAF,NDEV=$NDEV,POLL=$POLL,HELPER_MODE=$HELPER_MODE,REPS=3,OUTDIR=$OUTDIR,FOF3_BIN=$FOF3_BIN \
    run_fof3_scale2b_gpu.sbatch
done
squeue -u $USER -o "%.10i %.9q %.12j %.2t %.10M %.6D %R"
