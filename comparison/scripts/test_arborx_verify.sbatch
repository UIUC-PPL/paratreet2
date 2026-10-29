#!/bin/bash
# Does ArborX's OWN distributed DBSCAN pass ArborX's OWN verifier on ArborX's
# OWN generated data? Nothing of mine is involved: this is the stock
# benchmark, the stock generator and Details::verifyDBSCAN. It decides
# whether the wrong component counts my driver reports are a defect in my
# driver or in this build of ArborX's distributed DBSCAN.
#SBATCH --account=csc710
#SBATCH --job-name=axverify
#SBATCH --time=00:25:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
B=$CMP/arborx-build/benchmarks
WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"

# This is the exact command ArborX's own CTest entry for
# ArborX_Benchmark_DistributedDBSCAN runs, plus --verify.
for NP in 2 4 8; do
  echo "############### stock DistributedDBSCAN --verify ranks=$NP"
  timeout 300 srun --network=single_node_vni --ntasks=$NP --gpus-per-node=8 \
    "$WRAP" $B/cluster/ArborX_Benchmark_DistributedDBSCAN.exe \
    --n 1000 --dimension 3 --num-seq 4 --spacing 4 --core-min-size 2 \
    --eps 2 --verify 2>&1 | grep -vE "^ArborX (version|hash)|^Kokkos"
  echo "### srun_rc=${PIPESTATUS[0]}"
done
# And with minpts=2 on a denser generated set, closer to the FoF regime.
for NP in 2 8; do
  echo "############### stock, denser, ranks=$NP"
  timeout 300 srun --network=single_node_vni --ntasks=$NP --gpus-per-node=8 \
    "$WRAP" $B/cluster/ArborX_Benchmark_DistributedDBSCAN.exe \
    --n 20000 --dimension 3 --num-seq 10 --spacing 2 --core-min-size 2 \
    --eps 1.5 --verify 2>&1 | grep -vE "^ArborX (version|hash)|^Kokkos"
  echo "### srun_rc=${PIPESTATUS[0]}"
done
echo "=== VERIFY COMPLETE ==="
