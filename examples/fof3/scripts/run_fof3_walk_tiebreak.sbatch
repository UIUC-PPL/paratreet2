#!/bin/bash
# Issue #5: phase-3 dual-walk tie-break A/B at 2B particles, one job per node
# count, same shape as run_fof3_scale2b_gpu.sbatch (ppn 7, leaf 128 device /
# leaf 12 cpu, ndev 7, poll 1). FOF_WALK_TIEBREAK = parity (today) / hash
# (control) / weighted (w = 1 + leaf interactions of iteration 0, used from
# iteration 1). Report iteration 1 and its `FOF3STAT walk_timing` line.
#
#   sbatch --nodes=N --export=ALL,OUTDIR=<dir>,FOF3_BIN=<frozen binary> \
#     run_fof3_walk_tiebreak.sbatch
#
# Fails fast on the first arm that misses the gold count in either iteration.
#SBATCH --account=csc710
#SBATCH --job-name=walk5
#SBATCH --time=00:15:00
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive

source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh

export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
export PMI_MAX_KVS_ENTRIES=4194304

: ${REPS:=2}
: ${MODE:=tiebreak}
: ${ARM_TIMEOUT:=120}
L=${OUTDIR:?OUTDIR must name the output directory}
: ${FOF3_BIN:?FOF3_BIN must name a frozen binary}
PEMAP=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
cd /ccs/home/rrao/paratreet2/examples/fof3 || exit 1
INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
N=$SLURM_JOB_NUM_NODES
NT=$((N * 8))
GOLD="FOF3STAT components: 424897832 max_size 185317566"

WRAP=$L/gcd_wrap.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"

arm() {  # label leaf "extra FoF3 args" envs...
  local label=$1 leaf=$2 extra=$3; shift 3
  local log=$L/arm_${N}n_${label}_${REP}.${SLURM_JOB_ID}.log
  echo "=== ARM $label rep $REP nodes $N leaf $leaf args '$extra' env '$*' ==="
  env "$@" timeout --signal=TERM --kill-after=30 $ARM_TIMEOUT \
    srun --mpi=cray_shasta --network=job_vni --unbuffered \
      --cpu-bind=none --distribution=block:block \
      --ntasks=$NT --gpus-per-node=8 --export=ALL \
      "$WRAP" "$FOF3_BIN" -f "$INPUT" -d oct -u dist -c stats -l $leaf -i 2 $extra \
      +ppn 7 +pemap $PEMAP +lci_ndevices 7 +backend_poll_thread 1 \
    > "$log" 2>&1
  local rc=$?
  echo "exit: $rc"
  grep -E '^Iteration 1 time|walk_timing|phase3_walk' "$log" | tail -3 | cut -c1-330
  if [ $rc -ne 0 ] || [ "$(grep -c "$GOLD" "$log")" -ne 2 ]; then
    echo "=== ABORT: $label rep $REP missed the gold count (exit $rc); stopping ==="
    exit 1
  fi
}

DEV="PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1"
echo "### walk5 mode=$MODE nodes=$N reps=$REPS timeout=${ARM_TIMEOUT}s bin=$FOF3_BIN md5=$(md5sum $FOF3_BIN | cut -c1-12)"
REP=warmup; arm device_parity 128 "" $DEV FOF_WALK_TIEBREAK=parity
if [ "$MODE" = stream ]; then
  for REP in $(seq 1 $REPS); do
    arm device_E16_parity  128 ""     $DEV FOF_WALK_TIEBREAK=parity
    arm device_E0_parity   128 "-E 0" $DEV FOF_WALK_TIEBREAK=parity
    arm device_E0_weighted 128 "-E 0" $DEV FOF_WALK_TIEBREAK=weighted
  done
else
  for REP in $(seq 1 $REPS); do
    for tb in parity hash weighted; do
      arm device_$tb 128 "" $DEV FOF_WALK_TIEBREAK=$tb
    done
  done
  REP=1
  arm cpu_parity   12 "" PARATREET_DEVICE_TREE=0 FOF_WALK_TIEBREAK=parity
  arm cpu_weighted 12 "" PARATREET_DEVICE_TREE=0 FOF_WALK_TIEBREAK=weighted
fi
echo "=== WALK5 COMPLETE ==="
