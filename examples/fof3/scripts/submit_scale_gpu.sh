#!/bin/bash
# Submit the 2B device-path scaling sweep (design/fof3-2b-scaling.md).
#
# One script, five node counts, overridden on the command line so the
# body cannot drift between points. Walltimes are sized by the WORK, not
# by the QOS ceiling (design/phase1-gpu.md trap 18: the section-17 job
# asked for the 2-hour debug maximum, sat behind priority, and took 1:42).
# Each job is 3 arms x 2 iterations, plus 3 more arms at 16 nodes.
#
# QOS: debug allows exactly ONE SUBMITTED job per user -- not one running,
# one submitted -- so the four debug points cannot be queued together and
# --dependency does not help (a pending dependent job still counts).
# They are submitted one at a time, each after the previous finishes.
# The 128-node point goes to the default queue, matching
# run_fof3_scale_128.sbatch which likewise carries no --qos line, and is
# therefore submitted FIRST so it queues while the debug points run.
#
# Requires a binary built with GPU=1 PROJECTIONS=1 (tracing must be
# AVAILABLE at every node count so that +traceoff is what turns it off,
# exactly as in the CPU sweep; the 16-node job is the only one that turns
# it on).
set -e
cd "$(dirname "$0")"

S=run_fof3_scale_gpu.sbatch

# Default queue: submit and leave it queueing.
sbatch --nodes=128 --ntasks=1024 --time=00:30:00 --job-name=fof3_sgpu_128 $S

# Debug queue, strictly one at a time.
for spec in "8 64 00:35:00" "16 128 00:45:00" "32 256 00:30:00" \
            "64 512 00:30:00"; do
  set -- $spec
  while squeue -h -u "$USER" --qos=debug -o "%i" | grep -q .; do sleep 30; done
  sbatch --qos=debug --nodes=$1 --ntasks=$2 --time=$3 \
         --job-name=fof3_sgpu_$1 $S
  sleep 5
done
