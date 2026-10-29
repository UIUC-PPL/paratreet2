#!/bin/bash
# Did the three added fences fix it? The failure was INTERMITTENT (2/3 of
# runs), so a single pass proves nothing -- this repeats each rank count 8
# times and reports the distribution of verdicts and of the component
# census. The gate is: every repetition reports ARBORX_VERIFY PASS, and every
# repetition reports the SAME components/max_size, and that answer equals
# FoF3's and SWIFT's 33933 / 26042.
#SBATCH --account=csc710
#SBATCH --job-name=axfence
#SBATCH --time=00:40:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
BIN=$CMP/arborx-build/benchmarks/cosmo_fof/ArborX_CosmoFoF.exe
STOCK=$CMP/arborx-build/benchmarks/cluster/ArborX_Benchmark_DistributedDBSCAN.exe
NC=/lustre/orion/csc710/scratch/rrao/nctest/100k.nchilada
TP=/lustre/orion/csc710/scratch/rrao/nctest/100k.tipsy
WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"
echo "### TRUTH (FoF3 and SWIFT agree): components 33933  max_size 26042  isolated 27971"
echo "### binary md5=$(md5sum $BIN | cut -c1-12)"
for NP in 2 4 8; do
  for INPUT in "$NC" "$TP"; do
    for REP in 1 2 3 4 5 6 7 8; do
      printf "FENCE ranks=%d %s rep=%d : " "$NP" "$(basename $INPUT)" "$REP"
      out=$(timeout 200 srun --network=single_node_vni --ntasks=$NP \
        --gpus-per-node=8 "$WRAP" "$BIN" -f "$INPUT" -e 0.150205 -m 2 \
        --verify 2>&1)
      v=$(echo "$out"  | grep -o "ARBORX_VERIFY [A-Z]*" | head -1 | awk '{print $2}')
      c=$(echo "$out"  | grep -o "components: [0-9]* max_size [0-9]*" | head -1)
      i=$(echo "$out"  | grep -o "isolated: [0-9]*" | head -1 | awk '{print $2}')
      r=$(echo "$out"  | grep -o "REDIST_CHECK [A-Z]*" | head -1 | awk '{print $2}')
      echo "verify=${v:-NONE} redist=${r:-NONE} $c isolated=${i:-NA}"
    done
  done
done
# And the stock benchmark, repeated, to see whether the same fences also
# stabilise ArborX's own test -- evidence for the upstream bug report.
# Skipped unless the stock binary is at least as new as the patched header,
# so a stale binary cannot be mistaken for a post-fix result.
if [ "$STOCK" -nt "$CMP/arborx/src/cluster/detail/ArborX_DistributedDBSCANHelpers.hpp" ]; then
for NP in 4 8; do
  for REP in 1 2 3 4 5 6; do
    printf "STOCK ranks=%d rep=%d : " "$NP" "$REP"
    out=$(timeout 200 srun --network=single_node_vni --ntasks=$NP \
      --gpus-per-node=8 "$WRAP" "$STOCK" --n 20000 --dimension 3 \
      --num-seq 10 --spacing 2 --core-min-size 2 --eps 1.5 --verify 2>&1)
    echo "$(echo "$out" | grep -o 'Verification [a-z]*' | head -1)"
  done
done
else
  echo "STOCK SKIPPED: $STOCK predates the fence patch; rerun this job later."
fi
echo "=== FENCE COMPLETE ==="
