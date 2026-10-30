#!/bin/bash
# NChilada reader check on the GPU arm: same comparison as
# run_fof3_nchilada_check.sbatch, but against a GPU=1 FoF3 so the device
# phase-1 path sees the NChilada-loaded particles too. The three inputs hold
# the same particles in the same global order, so the FOF3STAT components
# lines must be identical.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:10:00
#SBATCH --job-name=fof3_nchilada_gpu
#SBATCH --nodes=1
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=fof3_nchilada_gpu.%j.out
#SBATCH --error=fof3_nchilada_gpu.%j.err

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh

export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
export PMI_MAX_KVS_ENTRIES=4194304

# +backend_poll_thread MUST be 1 here (the POLL default in
# run_fof3_scale2b_gpu.sbatch). With 2, every run wedges at
# "* Initializing cache managers" and never reaches the Reader at all --
# it looks exactly like an input-loading hang and is not one.
PPN=7
PEMAP=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
DATA=/lustre/orion/csc710/scratch/rrao/nctest

cd /ccs/home/rrao/paratreet2/examples/fof3 || exit 1

WRAP=/lustre/orion/csc710/scratch/rrao/nctest/gcd_wrap.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"

# Raw, unfiltered output straight to the job log. Piping srun through
# tee|grep block-buffers the pipeline, so a run that stops early shows
# nothing at all and looks like a hang wherever it actually stopped.
run_one() {
  local tag=$1 input=$2
  echo "=== $tag: $input ==="
  date +"start %F %T"
  # --network=single_node_vni: required for a run confined to one physical node.
  # PARATREET_DEVICE_TREE / FOF_GPU_PHASE1 are what actually route phase 1
  # to the device; a GPU=1 binary without them runs the CPU path.
  env PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1 \
  srun --mpi=cray_shasta --network=single_node_vni --unbuffered \
    --cpu-bind=none --distribution=block:block --export=ALL \
    "$WRAP" ./FoF3 -f "$input" -d oct -u dist -b 0.2 -c stats -l 128 \
    +noisomalloc +ppn $PPN +pemap $PEMAP +lci_ndevices 7 +backend_poll_thread 1 \
    2>&1 | tee "$DATA/run_$tag.log"
  echo "exit: ${PIPESTATUS[0]}"
  date +"end %F %T"
  echo
}

run_one tipsy      "$DATA/100k.tipsy"
run_one nchilada   "$DATA/100k.nchilada"
run_one nchilada64 "$DATA/100k-f64.nchilada"

echo "=== comparison ==="
for t in tipsy nchilada nchilada64; do
  printf '%-12s %s\n' "$t" "$(grep -m1 'FOF3STAT components:' "$DATA/run_$t.log")"
done
a=$(grep -m1 'FOF3STAT components:' "$DATA/run_tipsy.log")
b=$(grep -m1 'FOF3STAT components:' "$DATA/run_nchilada.log")
c=$(grep -m1 'FOF3STAT components:' "$DATA/run_nchilada64.log")
if [ -n "$a" ] && [ "$a" = "$b" ] && [ "$a" = "$c" ]; then
  echo "RESULT: PASS -- all three inputs produce identical components"
else
  echo "RESULT: FAIL -- component lines differ"
fi
