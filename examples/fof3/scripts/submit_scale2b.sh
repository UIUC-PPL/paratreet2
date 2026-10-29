#!/bin/bash
# Submit the 2B node-count scaling sweep, 8 -> 128 nodes, one job per
# point, all from the SAME run_fof3_scale2b_gpu.sbatch so the shape cannot drift between
# points.  Config comes in as arguments so the record of what was swept
# is this command line.
#
#   ./submit_scale.sh LEAF NDEV POLL PPN [HELPER]
#
# The 128-node point is submitted first: it is the longest queue wait, so
# starting it first shortens the wall clock of the whole sweep.
set -e
LEAF=${1:?leaf} NDEV=${2:?ndev} POLL=${3:?poll} PPN=${4:?ppn} HELPER_MODE=${5:-auto}
# NOTHING with a comma in it crosses --export -- sbatch splits the value
# on commas and drops the tail.  run_fof3_scale2b_gpu.sbatch derives PEMAP and
# FOF_HELPER_CPUS from PPN and HELPER_MODE instead.
cd "$(dirname "$0")"
for N in 128 64 32 16 8; do
  sbatch --nodes=$N --job-name=scale2b_$N \
    --export=ALL,PPN=$PPN,LEAF=$LEAF,NDEV=$NDEV,POLL=$POLL,HELPER_MODE=$HELPER_MODE,REPS=3 \
    run_fof3_scale2b_gpu.sbatch
done
squeue -u $USER -o "%.10i %.9q %.12j %.2t %.10M %.6D %R"
