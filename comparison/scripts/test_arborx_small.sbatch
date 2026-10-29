#!/bin/bash
# ArborX cosmo FoF smoke test: the 100k snapshot in both formats, on one node.
# Purpose is NOT performance. It answers three questions:
#   1. does the driver run at all on MI250X under HIP;
#   2. do the Tipsy and NChilada readers give the SAME answer (the two 100k
#      files hold identical positions, so any difference is a reader bug);
#   3. what component convention does ArborX report, so it can be lined up
#      with FoF3's FOF3STAT components.
#SBATCH --account=csc710
#SBATCH --job-name=axsmoke
#SBATCH --time=00:20:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err

source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
module load boost/1.86.0-mpi >/dev/null 2>&1
BIN=$CMP/arborx-build/benchmarks/cosmo_fof/ArborX_CosmoFoF.exe
NC=/lustre/orion/csc710/scratch/rrao/nctest/100k.nchilada
TP=/lustre/orion/csc710/scratch/rrao/nctest/100k.tipsy

# One GCD per rank, NUMA-local (Frontier core->GCD map is 0-7 -> 4, 8-15 -> 5,
# 16-23 -> 2, 24-31 -> 3, 32-39 -> 6, 40-47 -> 7, 48-55 -> 0, 56-63 -> 1).
WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"

echo "### binary $BIN"
ldd "$BIN" 2>&1 | grep "not found" || echo "###   all libs resolved"

# b = 0.2 * cbrt(V/N) over the snapshot bbox, computed in-driver, so the two
# formats must agree on it too.
for NP in 1 2 8; do
  for INPUT in "$NC" "$TP"; do
    echo "=================== ranks=$NP input=$(basename $INPUT)"
    srun --network=single_node_vni --ntasks=$NP --gpus-per-node=8 \
         --cpu-bind=none "$WRAP" "$BIN" -f "$INPUT" -b 0.2 -m 2 2>&1
    echo "### rc=$?"
  done
done
echo "=== SMOKE COMPLETE ==="
