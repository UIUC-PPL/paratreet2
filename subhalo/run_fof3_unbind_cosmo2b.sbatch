#!/bin/bash
# Step 2, part 2d (design/subhalo-checks.md, "Step 2: unbinding inside
# FoF3"): the time of unbinding inside FoF3 on cosmo2b at 64 nodes (512
# processes, one per die). Submit only with the user's go (node-hours are
# quoted in the note before the run).
#
#   CHECK_ONLY=1 bash subhalo/run_fof3_unbind_cosmo2b.sbatch
#   sbatch subhalo/run_fof3_unbind_cosmo2b.sbatch
#
# Input: cosmo25cmb.768g2_dm.001024 (1,981,808,640 particles, a = 1,
# periodic box of side 1, softening 1.38e-5 in the file). 452,984,832 of the
# particles sit in Tipsy's gas slot with zero density and 500 K; their
# thermal energy is left out (decision 6: dark matter only), so every
# particle is treated as collisionless. At a = 1 the cosmology does not
# enter (E(1) = 1). FoF largest group 185,317,566 (open boundaries).
#
# Runs, each FoF3 -K -m 20 -P 1 -U <settings>, the CPU arm for FoF itself
# (-l 12), 7 PEs per process:
#   spread   the production defaults: cap 10,000, refinement 1,000, bf
#            0.995, gather 1,000,000 (about 90 groups stay spread)
#   gather   the same with gather=0: every group gathered, the largest
#            onto one die
# What is read: the FOF3STAT unbind, unbind_time_s, unbind_spread and
# unbind_rows lines; the label digest against the gold value of the
# periodic run (none yet; the open run's is 2e8eaba371ff1d31).

#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:30:00
#SBATCH --job-name=fof3unbind2b
#SBATCH --nodes=64
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/fof3unbind2b.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/fof3unbind2b.%j.err

PT=${PT:-/ccs/home/rrao/paratreet2}
S=${S:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11}
IN=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
WRAP=${WRAP:-/lustre/orion/csc710/scratch/rrao/lazycanopy-2026-11-02/v7/gcd_wrap.sh}
EPS=1.3799999578623101e-05
RUNS=${RUNS:-"spread gather"}
declare -A SET=(
  [spread]="eps=$EPS"
  [gather]="eps=$EPS,gather=0"
)

missing=0
for f in $IN $WRAP $PT/examples/fof3/FoF3; do [ -s $f ] || { echo "### MISSING: $f"; missing=1; }; done
strings $PT/examples/fof3/FoF3 | grep -q 'unbind_rows' || { echo "### FoF3 has no part 2d"; missing=1; }
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
export LCI_ATTR_BACKEND=ofi FI_CXI_RX_MATCH_MODE=hybrid PMI_MAX_KVS_ENTRIES=4194304
J=${SLURM_JOB_ID:-nojob}
OUT=$S/fof3unbind2b.$J
B=$OUT/bin
mkdir -p $B && cp $PT/examples/fof3/FoF3 $B/ || exit 1
echo "### runs $RUNS; $SLURM_JOB_NUM_NODES nodes; commit $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet || echo ' + uncommitted changes'); md5 FoF3 $(md5sum $B/FoF3 | cut -c1-12)"

for run in $RUNS; do
  d=$OUT/$run
  mkdir -p $d
  echo "=== $run: -U ${SET[$run]} ($(date +%T))"
  ( cd $PT/examples/fof3 && PARATREET_DEVICE_TREE=0 timeout --signal=TERM --kill-after=15 900 \
    srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
      --distribution=block:block --ntasks=$((SLURM_JOB_NUM_NODES * 8)) --gpus-per-node=8 \
      --export=ALL $WRAP $B/FoF3 -f $IN -d oct -u dist -b 0.2 -c stats -l 12 -i 1 -K -m 20 -P 1 \
      -U "${SET[$run]}" +ppn 7 +pemap 1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63 \
      +lci_ndevices 7 +backend_poll_thread 1 > $d/fof3.log 2>&1 )
  rc=$?
  echo "  exit $rc"
  grep -E 'label_digest|FOF3STAT catalog|FOF3STAT unbind|^Iteration' $d/fof3.log | grep -v unbind_settings | cut -c1-400 | sed 's/^/  /'
  [ $rc = 0 ] || grep -m8 -E 'Fatal|Abort|abort|rror|subhalodev|unbind::spread' $d/fof3.log | sed 's/^/  /'
done
echo "=== done $(date +%T)"
sacct -j $J -X -n -o elapsed,nnodes,state
