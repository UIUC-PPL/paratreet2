#!/bin/bash
# Is the ArborX arm trustworthy at the rank counts the campaign actually uses?
# The merge-pair fix cleared 4 and 8 ranks but 2 ranks still fails 3/4, so
# "passes at 8" cannot be extrapolated. This runs BOTH the stock benchmark and
# my driver on the 100k set at 16, 32 and 64 ranks -- the low end of the real
# sweep (4 nodes x 8 = 32 ranks) -- with repetitions, because the failure was
# intermittent and a single pass proves nothing.
#
# Gate: every repetition reports the same components/max_size, and it equals
# FoF3's and SWIFT's 33933 / 26042.
#SBATCH --account=csc710
#SBATCH --job-name=axgate
#SBATCH --time=01:00:00
#SBATCH --nodes=8
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
export ARBORX_REPORT_MERGE_PAIRS=1
BIN=$CMP/arborx-build/benchmarks/cosmo_fof/ArborX_CosmoFoF.exe
STOCK=$CMP/arborx-build/benchmarks/cluster/ArborX_Benchmark_DistributedDBSCAN.exe
NC=/lustre/orion/csc710/scratch/rrao/nctest/100k.nchilada
WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"
echo "### TRUTH: components 33933  max_size 26042  isolated 27971"
echo "### driver md5=$(md5sum $BIN | cut -c1-12)  stock md5=$(md5sum $STOCK | cut -c1-12)"

for NP in 16 32 64; do
  for REP in 1 2 3 4 5; do
    printf "GATE_STOCK ranks=%-3d rep=%d : " "$NP" "$REP"
    o=$(timeout 240 srun --ntasks=$NP --gpus-per-node=8 "$WRAP" "$STOCK" \
        -f $CMP/data/100k.arborx --eps 0.150205 --core-min-size 2 --verify 2>&1)
    echo "$(echo "$o"|grep -o 'Verification [a-z]*'|head -1) $(echo "$o"|grep -o 'ARBORX_MERGE_PAIRS.*'|head -1)"
  done
  for REP in 1 2 3 4 5; do
    printf "GATE_MINE  ranks=%-3d rep=%d : " "$NP" "$REP"
    o=$(timeout 240 srun --ntasks=$NP --gpus-per-node=8 "$WRAP" "$BIN" \
        -f "$NC" -e 0.150205 -m 2 -i fdbscan --verify 2>&1)
    v=$(echo "$o"|grep -o 'ARBORX_VERIFY [A-Z]*'|head -1|awk '{print $2}')
    c=$(echo "$o"|grep -o 'components: [0-9]* max_size [0-9]*'|head -1)
    m=$(echo "$o"|grep -o 'ARBORX_MERGE_PAIRS global [0-9]*'|head -1|awk '{print $3}')
    echo "verify=${v:-NONE} $c merge_pairs=${m:-NA}"
  done
done
echo "=== GATE COMPLETE ==="
