#!/bin/bash
# Decisive run: cosmo_fof with the redistribution checksum, the locality
# probe, and ArborX's OWN verifier, on the 100k snapshot whose answer FoF3
# and SWIFT both give as 33933 components / max_size 26042.
#
# Reads as follows:
#   REDIST_CHECK FAIL  -> my Morton partition loses/duplicates points
#   LOCALITY ~1.0      -> the partition is not spatially compact, so the
#                         ghost exchange degenerates
#   ARBORX_VERIFY FAIL -> the labels ArborX produced are inconsistent with
#                         the eps-neighbourhood of the data I handed it
#   all three fine but census != 33933 -> my census is wrong
#SBATCH --account=csc710
#SBATCH --job-name=axprobe
#SBATCH --time=00:25:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
BIN=$CMP/arborx-build/benchmarks/cosmo_fof/ArborX_CosmoFoF.exe
NC=/lustre/orion/csc710/scratch/rrao/nctest/100k.nchilada
WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"
# Truth for this snapshot (FoF3 and SWIFT agree exactly):
echo "### TRUTH: components 33933  max_size 26042  isolated 27971  b 0.150205"
for NP in 2 4 8; do
  for REP in 1 2; do
    echo "############### ranks=$NP rep=$REP"
    timeout 300 srun --network=single_node_vni --ntasks=$NP --gpus-per-node=8 \
      "$WRAP" "$BIN" -f "$NC" -e 0.150205 -m 2 --verify 2>&1 | \
      grep -E "REDIST_CHECK|LOCALITY|ARBORX_VERIFY|ARBORXSTAT|per-rank n|TIME dbscan"
    echo "### srun_rc=${PIPESTATUS[0]}"
  done
done
echo "=== PROBE COMPLETE ==="
