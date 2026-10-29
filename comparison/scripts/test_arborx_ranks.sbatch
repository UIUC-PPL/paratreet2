#!/bin/bash
# cosmo_fof at REALISTIC rank counts on the 100k snapshot, both formats.
# The 1-rank case is excluded on purpose: ArborX's distributed DBSCAN hangs
# there (computeRanksTo filters out the local box, so comm_size==1 leaves it
# querying an EMPTY bounding-volume hierarchy -- the case ArborX's own
# "FIXME: not 100% sure what's going on" in DistributedDBSCAN.hpp:309 marks).
# It is not a case this campaign needs: every measured point is >= 32 ranks.
#SBATCH --account=csc710
#SBATCH --job-name=axranks
#SBATCH --time=00:25:00
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
TP=/lustre/orion/csc710/scratch/rrao/nctest/100k.tipsy
WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"

# FoF3's exact b on this snapshot, so the gate is on the identical linking
# length rather than on two independent derivations of it.
# (FoF3 printed b = 0.150205 from a universe box of V = 42360.3; the raw
# particle bbox gives 0.148620, which is why -e is passed explicitly.)
BEXACT=0.150205
for NP in 2 4 8; do
  for INPUT in "$NC" "$TP"; do
    echo "############### ranks=$NP input=$(basename $INPUT) eps=$BEXACT"
    timeout 240 srun --network=single_node_vni --ntasks=$NP --gpus-per-node=8 \
      "$WRAP" "$BIN" -f "$INPUT" -e $BEXACT -m 2 2>&1 | \
      grep -E "ARBORXSTAT|ARBORXRESULT|TIME|per-rank|eps \(b\)|n_total|error|Error|abort" 
    echo "### srun_rc=${PIPESTATUS[0]}"
  done
done
echo "=== RANKS COMPLETE ==="
