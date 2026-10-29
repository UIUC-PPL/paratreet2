#!/bin/bash
# Does ArborX's distributed DBSCAN stay correct when the clusters PERCOLATE?
#
# Hypothesis under test: ArborX's cross-rank label reconciliation
# (sortAndFilterMergePairs + relabel, a single pass, carrying two of the
# author's own FIXMEs) is complete for shallow merge graphs but not for deep
# ones. Its own benchmark generator makes well-separated clusters -- the help
# text says "If eps is larger than spacing, all the clusters will be merged
# together" -- so sweeping eps past `spacing` turns the SAME generator from
# the easy case into the percolating case that cosmological FoF at b=0.2 is.
#
# NOTHING of mine is involved: stock benchmark, stock generator, ArborX's own
# Details::verifyDBSCAN. If verification fails here, the defect is upstream.
#SBATCH --account=csc710
#SBATCH --job-name=axperc
#SBATCH --time=00:40:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
B=$CMP/arborx-build/benchmarks/cluster
WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"

# spacing is fixed at 2; eps sweeps from well below it (separated clusters)
# to well above it (one percolating cluster).
for NP in 4 8; do
  for EPS in 0.5 1.0 1.5 2.0 3.0 5.0 8.0; do
    printf "PERC ranks=%d eps=%-4s : " "$NP" "$EPS"
    out=$(timeout 200 srun --network=single_node_vni --ntasks=$NP \
      --gpus-per-node=8 "$WRAP" \
      $B/ArborX_Benchmark_DistributedDBSCAN.exe --n 20000 --dimension 3 \
      --num-seq 10 --spacing 2 --core-min-size 2 --eps $EPS --verify 2>&1)
    rc=$?
    v=$(echo "$out" | grep -o "Verification [a-z]*" | head -1)
    g=$(echo "$out" | grep -o "n (global) *: *[0-9]*" | head -1 | awk '{print $NF}')
    echo "${v:-NO-VERIFY-LINE} (n=$g rc=$rc)"
    if [ -z "$v" ] || [ "$v" = "Verification failed" ]; then
      echo "$out" | tail -12 | sed 's/^/      /'
    fi
  done
done
echo "=== PERCOLATE COMPLETE ==="
