#!/bin/bash
# Gate the HACC-style ArborX arm on the 100k set, whose answer two
# independent codes agree is 33933 components / max_size 26042.
# Repeated and swept over rank count, because the distributed-wrapper arm's
# failures were both intermittent and rank-count dependent, so a single pass
# at one width proves nothing.
#SBATCH --account=csc710
#SBATCH --job-name=hacctest
#SBATCH --time=01:00:00
#SBATCH --nodes=4
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
BIN=$CMP/arborx-build/benchmarks/cosmo_fof/ArborX_CosmoFoF_HACC.exe
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
echo "### TRUTH: components 33933  max_size 26042"
echo "### binary md5=$(md5sum $BIN | cut -c1-12)"
# Both decompositions are gated. `uniform` is the HACC-faithful primary and
# `balanced` the secondary; they share all the ownership and halo code, so a
# divergence between them would mean the shared path depends on where the cut
# planes sit, which it must not.
for PART in uniform balanced; do
for NP in 2 4 8 16 27 32; do
  for INPUT in "$NC" "$TP"; do
    for REP in 1 2; do
      printf "HACC %-8s ranks=%-3d %s rep=%d : " "$PART" "$NP" "$(basename $INPUT)" "$REP"
      o=$(timeout 240 srun --ntasks=$NP --gpus-per-node=8 "$WRAP" "$BIN" \
          -f "$INPUT" -e 0.150205 -m 2 -i fdbscan -p "$PART" 2>&1)
      cc=$(echo "$o"|grep -o "CENSUS_CHECK [A-Z]*"|head -1|awk '{print $2}')
      st=$(echo "$o"|grep -o "HACCSTAT components: [0-9]* max_size [0-9]*"|head -1|sed 's/HACCSTAT //')
      im=$(echo "$o"|grep -o "imbalance [0-9.]*"|head -1)
      ha=$(echo "$o"|grep -o "halo *: total [0-9]* ([0-9.]*%"|head -1|sed 's/.*total //')
      ro=$(echo "$o"|grep -o "rounds [0-9]*"|head -1)
      cs=$(echo "$o"|grep -o "census *: Lmax .*"|head -1|sed 's/census *: //')
      cw=$(echo "$o"|grep -c "WARNING census")
      echo "census=${cc:-NA} ${st:-components NA} ${im:-NA} halo=${ha:-NA} ${ro:-NA} [${cs:-no census line}] warn=$cw"
      if [ -z "$st" ]; then echo "$o" | tail -6 | sed 's/^/      /'; fi
    done
  done
done
done
echo "=== HACC TEST COMPLETE ==="
