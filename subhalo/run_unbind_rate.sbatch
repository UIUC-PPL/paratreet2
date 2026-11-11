#!/bin/bash
# Pair-term rate of the unbinding kernel on one MI250X die, with the
# kernel's self-checks first (subhalo/unbind_rate.cpp,
# design/subhalo-checks.md step 1). One node, one die, under 12 minutes.
#
#   make -C subhalo unbind_rate
#   OUT=/lustre/orion/csc710/scratch/$USER/unbind-rate sbatch subhalo/run_unbind_rate.sbatch
#
# OUT (default: the directory sbatch was called from) receives a copy of the
# binary and the log unbind_rate.<jobid>.log.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:12:00
#SBATCH --job-name=unbindrate
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=14
#SBATCH --gpus-per-node=8
#SBATCH --exclusive

PT=${PT:-/ccs/home/rrao/paratreet2}
OUT=${OUT:-$SLURM_SUBMIT_DIR}
SECONDS_PER_CASE=${SECONDS_PER_CASE:-2}
source $PT/examples/fof3/scripts/frontier_env.sh
J=$SLURM_JOB_ID
BIN=$OUT/unbind_rate.$J
cp $PT/subhalo/unbind_rate $BIN || exit 1
LOG=$OUT/unbind_rate.$J.log
echo "### node $(hostname) binary md5 $(md5sum $BIN | cut -c1-12) commit $(git -C $PT rev-parse --short HEAD)"

# One die: of the node's eight, only device 0 is visible to the program.
ROCR_VISIBLE_DEVICES=0 timeout --signal=TERM --kill-after=15 600 \
  srun --ntasks=1 --cpus-per-task=14 --gpus-per-node=8 --unbuffered --export=ALL \
    $BIN --seconds $SECONDS_PER_CASE > $LOG 2>&1
RC=$?
echo "### exit $RC"
grep -E '^CHECK|^UNBIND_RATE|execution space' $LOG
sed -n '/^layout /,$p' $LOG
sacct -j $J -X -n -o state,elapsed
exit $RC
