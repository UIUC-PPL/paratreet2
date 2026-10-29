#!/bin/bash
# FoF3 reference run on the 100k snapshot, with the REBUILT stack
# (charm reconverse-linux-x86_64-amd-cmp + the allgather-bootstrap LCI).
# Produces the b value and the FOF3STAT components/max_size that ArborX and
# SWIFT are gated against, in both input formats.
#SBATCH --account=csc710
#SBATCH --job-name=f3smoke
#SBATCH --time=00:25:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err

source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
CHARMB=reconverse-linux-x86_64-amd-cmp
export CHARM_HOME=/ccs/home/rrao/charm_reconverse/$CHARMB
export LD_LIBRARY_PATH=$CHARM_HOME/lib:$LD_LIBRARY_PATH
export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
# LCI's bootstrap needs headroom even at small scale: Frontier's Cray PMI
# defaults to PMI_MAX_KVS_ENTRIES=30 and the (patched) bootstrap publishes
# one key per rank, but the unpatched path would want rank_n^2.
export PMI_MAX_KVS_ENTRIES=65536
ulimit -c 0

BIN=/ccs/home/rrao/paratreet2/examples/fof3/FoF3
NC=/lustre/orion/csc710/scratch/rrao/nctest/100k.nchilada
TP=/lustre/orion/csc710/scratch/rrao/nctest/100k.tipsy
PPN=7
PEMAP=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63

WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"

echo "### binary=$BIN md5=$(md5sum $BIN | cut -c1-12)"
echo "### charm=$CHARMB"
echo "### liblci=$(ldd $BIN 2>/dev/null | awk '/liblci.so/{print $3}')"
ldd "$BIN" 2>&1 | grep "not found" || echo "###   all libs resolved"
# The patched bootstrap must be the one that actually runs: liblci.so is
# resolved by SONAME through LD_LIBRARY_PATH, so a mismatched charm build
# silently executes the OLD library and reproduces its behaviour exactly.
export LCI_LOG_LEVEL=info

for INPUT in "$NC" "$TP"; do
  for NT in 1 8; do
    echo "=================== procs=$NT input=$(basename $INPUT)"
    env PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1 \
    srun --mpi=cray_shasta --network=single_node_vni --unbuffered \
         --cpu-bind=none --ntasks=$NT --gpus-per-node=8 --export=ALL \
         "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l 128 -i 1 \
         +ppn $PPN +pemap $PEMAP +backend lci +lci_ndevices 7 \
         +backend_poll_thread 1 2>&1 | \
      grep -E "linking length|FOF3STAT|Iteration|Decomposition|Bootstrap|CmiAbort|error|Error" | head -40
    echo "### rc=${PIPESTATUS[0]}"
  done
done
echo "=== FOF3 SMOKE COMPLETE ==="
