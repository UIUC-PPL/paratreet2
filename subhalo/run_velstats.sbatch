#!/bin/bash
# cosmo25: the velocity check (design/subhalo-checks.md, "cosmo25: the
# velocity check"). cosmo25 gives sub-haloes to 44% of its groups, cosmo2b
# to 63% (job 5923504). Both are 25 Mpc boxes at a = 1, so a power of a
# cannot separate their velocity conventions; a different unit system can.
# This reads evenly spaced samples of both files (subhalo/velstats.py) and
# prints per family the total mass (Omega_m if both use critical density 1)
# and the rms speeds.
#
#   CHECK_ONLY=1 bash subhalo/run_velstats.sbatch
#   sbatch subhalo/run_velstats.sbatch

#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:15:00
#SBATCH --job-name=velstats
#SBATCH --nodes=1
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/velstats.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/velstats.%j.err

PT=${PT:-/ccs/home/rrao/paratreet2}
C2B=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
C25=/lustre/orion/csc710/proj-shared/cosmo25PLK.2304g_dm.004096
missing=0
for f in $C2B $C25/dark/vel $C25/gas/vel $C25/dark/mass $C25/gas/mass $PT/subhalo/velstats.py; do
  [ -s $f ] || { echo "### MISSING: $f"; missing=1; }
done
python3 -c 'import numpy' || { echo "### no numpy"; missing=1; }
[ $missing = 0 ] || exit 2
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

echo "### commit $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet -- subhalo/velstats.py || echo ' + uncommitted velstats.py') ($(date +%T))"
J=${SLURM_JOB_ID:-nojob}
OUT=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/velstats.$J
mkdir -p $OUT
python3 -I $PT/subhalo/velstats.py $C2B > $OUT/cosmo2b.txt 2>&1 &
python3 -I $PT/subhalo/velstats.py $C25 > $OUT/cosmo25.txt 2>&1 &
wait
cat $OUT/cosmo2b.txt $OUT/cosmo25.txt
echo "### done $(date +%T)"
sacct -j $J -X -n -o elapsed,nnodes,state
