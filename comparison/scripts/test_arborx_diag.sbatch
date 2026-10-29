#!/bin/bash
# Diagnostic: localise where ArborX_CosmoFoF stops. Every srun is wrapped in
# `timeout` so one hang cannot consume the whole job, and --verbose turns on
# ArborX's own per-step Kokkos timers.
#SBATCH --account=csc710
#SBATCH --job-name=axdiag
#SBATCH --time=00:30:00
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

# A: does the stock ArborX distributed DBSCAN benchmark work at all here?
# If its GENERATOR path also hangs, the fault is in the ArborX/HIP/MPI build,
# not in my reader or redistribution.
STOCK=$CMP/arborx-build/benchmarks/cluster/ArborX_Benchmark_DistributedDBSCAN.exe
for NP in 1 2; do
  echo "=== A: stock DistributedDBSCAN generator, ranks=$NP"
  timeout 180 srun --network=single_node_vni --ntasks=$NP --gpus-per-node=8 \
    --cpu-bind=none "$WRAP" "$STOCK" --n 20000 --dimension 3 --num-seq 4 \
    --spacing 4 --core-min-size 2 --eps 2 2>&1 | tail -20
  echo "### rc=$? (124 = timed out)"
done

# B: mine, tiny N, increasing, 1 rank then 8.
for NP in 1 8; do
  for N in 1000 20000 100000; do
    echo "=== B: cosmo_fof ranks=$NP N=$N"
    timeout 180 srun --network=single_node_vni --ntasks=$NP --gpus-per-node=8 \
      --cpu-bind=none "$WRAP" "$BIN" -f "$NC" -b 0.2 -m 2 -N $N -v 2>&1 | tail -30
    echo "### rc=$? (124 = timed out)"
  done
done
echo "=== DIAG COMPLETE ==="
