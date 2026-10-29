#!/bin/bash
# Is the ArborX-vs-FoF3 component gap on cosmo25 (6730729113 vs 6730729617,
# 504 apart) just the eps boundary?
#
# ArborX evaluates the neighbour test in FLOAT, so the eps it actually applies
# differs from the double FoF3 uses by ~6e-8 relative. If d(components)/d(eps)
# is of order 500 per 1e-7 of RELATIVE eps change, the whole gap is accounted
# for by that conversion and nothing else needs explaining. If the count is
# far less sensitive than that, something real is wrong and this says so.
#
# Deliberately run at ONE node count (16) -- this is a sensitivity measurement,
# not a scaling one.
#SBATCH --account=csc710
#SBATCH --job-name=epssens
#SBATCH --time=02:00:00
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
BASE=6.8897746514307755e-05
WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"
echo "### FoF3 gold for cosmo25: components 6730729617  max_size 2214117459"
echo "### base eps=$BASE  nodes=16 ranks=128 partition=balanced"
for D in -1e-6 -1e-7 0 1e-7 1e-6; do
  E=$(python3 -c "print(repr($BASE * (1.0 + $D)))")
  printf "EPSSENS rel=%-8s eps=%s : " "$D" "$E"
  o=$(timeout 1800 srun --ntasks=128 --gpus-per-node=8 "$WRAP" "$BIN" \
      -f "$INPUT" -e "$E" -m 2 -i fdbscan -p balanced 2>&1)
  c=$(echo "$o" | grep -m1 -o "HACCCOUNT components: [0-9]*" | awk '{print $3}')
  st=$(echo "$o" | grep -m1 -o "HACCSTAT components: [0-9]* max_size [0-9]*")
  w=$(echo "$o" | grep -c "WARNING census")
  echo "count=${c:-NA} ${st:-NA} warn=$w"
  [ -z "$c" ] && echo "$o" | tail -5 | sed 's/^/      /'
done
echo "=== EPSSENS COMPLETE ==="
