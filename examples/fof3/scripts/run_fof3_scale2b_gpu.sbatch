#!/bin/bash
# 2B-particle node-count scaling, 8 -> 128 nodes, on the 2026-08-21 stack.
# ONE file parameterised by --nodes, so every point in the sweep runs
# byte-identical code and shape; the config comes from the 16-node
# parameter sweep (job recorded in the log line below).
#
# Env in, via sbatch --export:  PPN PEMAP LEAF NDEV POLL HELPER REPS
# Two arms per node count: the device Replace path at the swept-best
# config, and the CPU chain at its own best leaf (12) for the reference
# curve.  Gate: 424897832 components / max_size 185317566 at every point.
# No Projections tracing anywhere (the binary is not trace-linked).
#SBATCH --account=csc710
#SBATCH --job-name=scale2b
#SBATCH --time=01:00:00
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/sweep0821/%x_%N_n.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/sweep0821/%x_%N_n.%j.err

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh

export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
export PMI_MAX_KVS_ENTRIES=4194304

: ${PPN:=7}
: ${LEAF:=128}
: ${NDEV:=7}
: ${POLL:=1}
: ${HELPER_MODE:=auto}
: ${REPS:=3}

# PEMAP and FOF_HELPER_CPUS are DERIVED HERE, never passed in.  sbatch
# --export splits its value on commas, so --export=ALL,PEMAP=1-7,9-15,...
# delivers PEMAP=1-7 and silently drops the rest: every process on a node
# then pins all 7 PEs to cores 1-7, 56 threads on 7 cores, and the whole
# run comes out ~8x slow with nothing in the log but a short
# "cpuaffinity PE-core map" line.  That cost job 5321313-17.  Anything
# containing a comma is computed on this side of the boundary.
NOSMT=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
SMT=1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127
CCD=0,8,16,24,32,40,48,56
if [ "$PPN" = 7 ]; then PEMAP=$NOSMT; else PEMAP=$SMT; fi
nblocks=$(echo "$PEMAP" | tr ',' '\n' | wc -l)
if [ "$nblocks" -ne 8 ] && [ "$nblocks" -ne 16 ]; then
  echo "FATAL: PEMAP has $nblocks blocks, expected 8 (ppn 7) or 16 (ppn 14): $PEMAP" >&2
  exit 1
fi
case "$HELPER_MODE" in
  ccd)  HELPER=$CCD ;;
  *)    HELPER= ;;      # auto: the fix derives the SMT siblings itself
esac

L=/lustre/orion/csc710/scratch/rrao/sweep0821
cd /ccs/home/rrao/paratreet2/examples/fof3 || exit 1
INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
N=$SLURM_JOB_NUM_NODES
NT=$((N * 8))

WRAP=$L/gcd_wrap.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"

arm() {
  local label=$1 leaf=$2; shift 2
  echo "=== ARM $label rep $REP nodes $N (ppn=$PPN leaf=$leaf ndev=$NDEV poll=$POLL pemap=$PEMAP env='$E') ==="
  date +"start %s.%N %F %T"
  env $E \
    srun --mpi=cray_shasta --network=job_vni --unbuffered \
      --cpu-bind=none --distribution=block:block \
      --ntasks=$NT --gpus-per-node=8 --export=ALL \
      "$WRAP" ./FoF3 -f "$INPUT" -d oct -u dist -c stats -l $leaf -i 2 \
      +ppn $PPN +pemap $PEMAP +lci_ndevices $NDEV +backend_poll_thread $POLL
  echo "exit: $?"
  date +"end %s.%N %F %T"
}

DEV="PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1"
[ -n "$HELPER" ] && DEV="$DEV FOF_HELPER_CPUS=$HELPER"

echo "### scale2b nodes=$N ppn=$PPN leaf=$LEAF ndev=$NDEV poll=$POLL helper='$HELPER' reps=$REPS"
REP=warmup; E="$DEV" arm device $LEAF
# Three arms per point, because the leaf choice is a phase-1/phase-3
# TRADE and the trade moves with node count (fof3-2b-scaling.md section 4:
# at 64 nodes the coarse tree lost).  device_l12 is the apples-to-apples
# arm -- same tree, same labels, same phase-3 work as cpu, only phase 1's
# implementation differs -- and it is what makes the downstream
# regression measurable rather than inferred.
for REP in $(seq 1 $REPS); do
  E="$DEV"                    arm device      $LEAF
  E="$DEV"                    arm device_l12  12
  E="PARATREET_DEVICE_TREE=0" arm cpu         12
done
echo "=== SCALE COMPLETE ==="
