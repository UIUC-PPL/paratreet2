#!/bin/bash
# THE CLEAN UPSTREAM TEST. The real cosmology point set, in ArborX's OWN
# binary format, read by ArborX's OWN loader, clustered by ArborX's OWN
# distributed DBSCAN, checked by ArborX's OWN verifier. The only code of mine
# anywhere near it is a 30-line writer that emits
#   int num_points; int dim; float xyz[]
# and whose output is trivially checkable (1200008 bytes = 8 + 12 * 100000).
#
# Truth for this point set, from two independent codes: 33933 components,
# max_size 26042, 27971 isolated.
#
# If verification fails here, the defect is upstream in ArborX and this job is
# the reproducer to send them. If it passes, the defect is in my driver and
# the stock loader's file-order slab is the thing mine does differently.
#SBATCH --account=csc710
#SBATCH --job-name=axupstream
#SBATCH --time=00:30:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
STOCK=$CMP/arborx-build/benchmarks/cluster/ArborX_Benchmark_DistributedDBSCAN.exe
DATA=$CMP/data/100k.arborx
WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"
echo "### TRUTH: 33933 components, max_size 26042, 27971 isolated"
echo "### stock binary md5=$(md5sum $STOCK | cut -c1-12)"
echo "### data $DATA ($(stat -c %s $DATA) bytes; expect 1200008)"
for NP in 2 4 8; do
  for REP in 1 2 3 4; do
    printf "UPSTREAM ranks=%d rep=%d : " "$NP" "$REP"
    out=$(timeout 250 srun --network=single_node_vni --ntasks=$NP \
      --gpus-per-node=8 "$WRAP" "$STOCK" -f "$DATA" --eps 0.150205 \
      --core-min-size 2 --verify 2>&1)
    rc=$?
    v=$(echo "$out" | grep -o "Verification [a-z]*" | head -1)
    r=$(echo "$out" | grep -o "read in [0-9]* 3D points" | head -1)
    echo "${v:-NO-VERIFY-LINE} rc=$rc ($r)"
    if [ -z "$v" ]; then echo "$out" | tail -8 | sed 's/^/      /'; fi
  done
done
echo "=== UPSTREAM COMPLETE ==="
