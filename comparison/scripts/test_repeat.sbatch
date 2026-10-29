#!/bin/bash
# Three questions in one job, on cosmo25 at 16 nodes / 128 ranks:
#  1. Is the answer REPEATABLE run-to-run with a fixed binary and config?
#     Two builds reported 6730729113 and 6730729120 for what should be the
#     same solve, and if that is run-to-run noise rather than a census bug
#     then every "the count changed between builds" conclusion so far is
#     wrong.
#  2. Do the two independent counting routes agree? HACCCOUNT needs no
#     communication and is exact by construction; HACCSTAT comes from the
#     label exchange.
#  3. Why did HACCSTAT vanish from the eps-sensitivity job? Its output was
#     discarded by that script (my mistake); here the FULL log is kept.
#SBATCH --account=csc710
#SBATCH --job-name=axrepeat
#SBATCH --time=01:30:00
#SBATCH --nodes=16
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
ulimit -c 0
BIN=$CMP/arborx-build/benchmarks/cosmo_fof/ArborX_CosmoFoF_HACC.exe
INPUT=/lustre/orion/csc710/proj-shared/cosmo25PLK.2304g_dm.004096
EPS=6.8897746514307755e-05
WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"
echo "### binary md5=$(md5sum $BIN | cut -c1-12)"
echo "### FoF3 gold: components 6730729617 max_size 2214117459"
for REP in 1 2; do
  echo "======== REP $REP ========"
  timeout 1800 srun --ntasks=128 --gpus-per-node=8 "$WRAP" "$BIN" \
      -f "$INPUT" -e "$EPS" -m 2 -i fdbscan -p balanced 2>&1 \
    | tee $CMP/logs/axrepeat_rep${REP}.$SLURM_JOB_ID.log \
    | grep -E "HACCCOUNT|HACCSTAT|CENSUS_CHECK|WARNING|TIME |rounds|per-rank own|FATAL|error|cxil"
  echo "### rep $REP srun rc=${PIPESTATUS[0]}"
done
echo "=== REPEAT TEST COMPLETE ==="
