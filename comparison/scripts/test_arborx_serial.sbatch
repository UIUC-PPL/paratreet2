#!/bin/bash
# THE BISECTION. ArborX's NON-distributed dbscan on exactly the point set my
# reader produces, one rank, no distributed layer at all -- but my reader, my
# eps and my census. Truth (FoF3 and SWIFT, independently): 33933 components,
# max_size 26042, 27971 isolated.
#
#   matches truth  -> reader, linking length and census are all correct, and
#                     the fault is confined to ArborX::Experimental::dbscan
#   does not match -> the fault is in my side, and this says which of the
#                     three, because each is exercised here
# Repeated, because the distributed path's failure was intermittent and the
# serial path's determinism is itself a datum.
#SBATCH --account=csc710
#SBATCH --job-name=axserial
#SBATCH --time=00:20:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
BIN=$CMP/arborx-build/benchmarks/cosmo_fof/ArborX_CosmoFoF.exe
WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"
echo "### TRUTH: components 33933  max_size 26042  isolated 27971"
echo "### binary md5=$(md5sum $BIN | cut -c1-12)"
for INPUT in /lustre/orion/csc710/scratch/rrao/nctest/100k.nchilada \
             /lustre/orion/csc710/scratch/rrao/nctest/100k.tipsy; do
  for REP in 1 2 3 4; do
    printf "SERIAL %s rep=%d : " "$(basename $INPUT)" "$REP"
    timeout 200 srun --network=single_node_vni --ntasks=1 --gpus-per-node=8 \
      "$WRAP" "$BIN" -f "$INPUT" -e 0.150205 -m 2 --serial 2>&1 | \
      grep -vE "^ArborX (version|hash)|^Kokkos|^=== ArborX|^impl|^minpts|^snapshot|^#MPI|^exec space" | tr '\n' ' '
    echo
  done
done
echo "=== SERIAL COMPLETE ==="
