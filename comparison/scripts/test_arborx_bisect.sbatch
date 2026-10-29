#!/bin/bash
# Where in MY driver is the fault? Stock ArborX on the same points at the same
# rank counts is now correct (15/15), so the difference is something my driver
# does. Two knobs isolate it:
#
#   --no-redistribute : file-order slabs, i.e. EXACTLY what the stock loader
#                       produces. If this is correct, the Morton
#                       redistribution is at fault. If it is still wrong, the
#                       fault is downstream of it -- the device staging or the
#                       census -- and is shared by both paths.
#   DEVICE_CHECK      : a Kokkos reduction over `points` ON THE DEVICE against
#                       the host checksum of xyz. Every earlier probe validated
#                       only the HOST buffer; this validates what ArborX reads.
#
# Truth: 33933 components, max_size 26042, 27971 isolated.
#SBATCH --account=csc710
#SBATCH --job-name=axbisect
#SBATCH --time=00:40:00
#SBATCH --nodes=4
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
export ARBORX_REPORT_MERGE_PAIRS=1
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
echo "### TRUTH: components 33933  max_size 26042  isolated 27971"
echo "### driver md5=$(md5sum $BIN | cut -c1-12)"
for NP in 16 32; do
  for MODE in "--no-redistribute" ""; do
    for REP in 1 2 3; do
      printf "BISECT ranks=%-3d mode=%-18s rep=%d : " "$NP" "${MODE:-morton}" "$REP"
      o=$(timeout 240 srun --ntasks=$NP --gpus-per-node=8 "$WRAP" "$BIN" \
          -f "$NC" -e 0.150205 -m 2 -i fdbscan --verify $MODE 2>&1)
      dc=$(echo "$o"|grep -o 'DEVICE_CHECK [A-Z]*'|head -1|awk '{print $2}')
      rc2=$(echo "$o"|grep -o 'REDIST_CHECK [A-Z]*'|head -1|awk '{print $2}')
      lo=$(echo "$o"|grep -o 'LOCALITY mean_box_diag/global [0-9.]*'|head -1|awk '{print $NF}')
      v=$(echo "$o"|grep -o 'ARBORX_VERIFY [A-Z]*'|head -1|awk '{print $2}')
      c=$(echo "$o"|grep -o 'components: [0-9]* max_size [0-9]*'|head -1)
      m=$(echo "$o"|grep -o 'ARBORX_MERGE_PAIRS global [0-9]*'|head -1|awk '{print $3}')
      echo "device=${dc:-NA} redist=${rc2:-NA} locality=${lo:-NA} verify=${v:-NA} $c mp=${m:-NA}"
    done
  done
done
echo "=== BISECT COMPLETE ==="
