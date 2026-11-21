#!/bin/bash
# Step 2 on cosmo25 (design/subhalo-checks.md, "cosmo25 at 64 nodes"):
# unbinding inside FoF3 on the 24-billion-particle snapshot, 64 nodes (512
# processes, one per die). Submit only with the user's go (node-hours are
# quoted in the note before the run), and only after the two-node smoke
# test (run_fof3_unbind_nchilada_smoke.sbatch) passed with the same binary.
#
#   CHECK_ONLY=1 bash subhalo/run_fof3_unbind_cosmo25.sbatch
#   sbatch subhalo/run_fof3_unbind_cosmo25.sbatch
#
# Input: cosmo25PLK.2304g_dm.004096 (NChilada, 24,461,180,928 particles:
# 12,230,590,464 dark of mass 2.1287e-11 and as many in the gas family of
# mass 3.9450e-12, all at 500 K, so collisionless as on cosmo2b; decision 6).
# a = 1 - 3.9e-13 (the run uses a = 1), periodic box of side 1, softening
# 4.7219e-6 in the files. FoF at b = 0.2 with open boundaries: 6,730,729,617
# groups, largest 2,214,117,459 (above 2^31), 691 groups of 2^20 or more.
#
# Runs, each FoF3 -K -m 20 with the CPU arm for FoF itself (-l 12), 7 PEs
# per process:
#   open    no -U, open boundaries: FoF's answer against the gold value of
#           the September runs (job 5512059 and the others of
#           design/fof-comparison-campaign.md 5.8), the first run on this
#           snapshot since the min-id labels, the catalogue and the lazy
#           canopy. If it differs, the spread run is not started.
#   spread  -P 1 -U with the production defaults: cap 10,000, refinement
#           1,000, bf 0.995, gather 1,000,000 (about 690 groups stay spread)
# There is no gathered run: the largest group (74 GiB) does not fit one die.
# What is read: the FOF3STAT components and label_digest lines (open), and
# the unbind, unbind_time_s, unbind_spread, unbind_spread_largest,
# unbind_rows, unbind_spread_rounds, unbind_by_size and unbind_by_family
# lines (spread).

#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:30:00
#SBATCH --job-name=fof3unbind25
#SBATCH --nodes=64
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/fof3unbind25.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/fof3unbind25.%j.err

PT=${PT:-/ccs/home/rrao/paratreet2}
S=${S:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11}
IN=/lustre/orion/csc710/proj-shared/cosmo25PLK.2304g_dm.004096
WRAP=${WRAP:-/lustre/orion/csc710/scratch/rrao/lazycanopy-2026-11-02/v7/gcd_wrap.sh}
EPS=4.721853201772319e-06
GOLD_OPEN="6730729617 max_size 2214117459"
RUNS=${RUNS:-"open spread"}
declare -A OPT=(
  [open]=""
  [spread]="-P 1 -U eps=$EPS"
)

missing=0
for f in $IN/dark/pos $IN/dark/vel $IN/gas/pos $IN/gas/vel $WRAP $PT/examples/fof3/FoF3; do
  [ -s $f ] || { echo "### MISSING: $f"; missing=1; }
done
strings $PT/examples/fof3/FoF3 | grep -q 'unbind_spread_largest' || { echo "### FoF3 has no unbind_spread_largest line"; missing=1; }
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
export LCI_ATTR_BACKEND=ofi FI_CXI_RX_MATCH_MODE=hybrid PMI_MAX_KVS_ENTRIES=4194304
ulimit -c 0
J=${SLURM_JOB_ID:-nojob}
OUT=$S/fof3unbind25.$J
B=$OUT/bin
mkdir -p $B && cp $PT/examples/fof3/FoF3 $B/ || exit 1
echo "### runs $RUNS; $SLURM_JOB_NUM_NODES nodes; commit $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet || echo ' + uncommitted changes'); md5 FoF3 $(md5sum $B/FoF3 | cut -c1-12)"

for run in $RUNS; do
  d=$OUT/$run
  mkdir -p $d
  echo "=== $run: ${OPT[$run]:-(no -U, open boundaries)} ($(date +%T))"
  ( cd $PT/examples/fof3 && PARATREET_DEVICE_TREE=0 timeout --signal=TERM --kill-after=15 600 \
    srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
      --distribution=block:block --ntasks=$((SLURM_JOB_NUM_NODES * 8)) --gpus-per-node=8 \
      --export=ALL $WRAP $B/FoF3 -f $IN -d oct -u dist -b 0.2 -c stats -l 12 -i 1 -K -m 20 \
      ${OPT[$run]} +ppn 7 +pemap 1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63 \
      +lci_ndevices 7 +backend_poll_thread 1 > $d/fof3.log 2>&1 )
  rc=$?
  echo "  exit $rc ($(date +%T))"
  grep -E 'linking_length_exact|FOF3STAT components|label_digest|FOF3STAT catalog|FOF3STAT unbind|^Loading|Total Decomposition|^Iteration' $d/fof3.log \
    | grep -v unbind_settings | cut -c1-3000 | sed 's/^/  /'
  [ $rc = 0 ] || grep -m8 -E 'Fatal|Abort|abort|rror|subhalodev|unbind::spread|oom|OOM' $d/fof3.log | sed 's/^/  /'
  if [ $run = open ]; then
    got=$(grep -o 'FOF3STAT components: [0-9]* max_size [0-9]*' $d/fof3.log | head -1 | cut -d' ' -f3-)
    if [ $rc = 0 ] && [ "$got" = "$GOLD_OPEN" ]; then
      echo "  open: FoF's answer equals the gold value ($GOLD_OPEN)"
    else
      echo "  open: FoF's answer '$got' is not the gold value ($GOLD_OPEN): spread run not started"
      break
    fi
  fi
done
echo "=== done $(date +%T)"
sacct -j $J -o jobid,jobname%14,elapsed,nnodes,state,maxrss
