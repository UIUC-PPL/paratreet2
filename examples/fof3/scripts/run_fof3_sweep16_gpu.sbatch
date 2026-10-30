#!/bin/bash
# 2B / 16 nodes: parameter sweep for the DEVICE (Replace) phase-1 path on
# the 2026-08-21 rebuilt stack (merged main + helper-thread affinity fix,
# charm reconverse-specific-build 90ef159b9, reconverse 6f34e68).
#
# One factor at a time from the README-recommended baseline
#   ppn 7 (no SMT) / lci_ndevices 7 / backend_poll_thread 1 / -l 128
# and 3 reps of every arm, interleaved as three full passes so machine
# drift cannot alias onto any single arm (the relay46/47 convention).
#
# The open questions this is here to answer:
#  * LEAF SIZE.  -l 128 was chosen from an 80M PHASE-1-ONLY sweep
#    (design/phase1-gpu.md 16.2/16.3), which explicitly did not measure
#    tree build or phase 3.  This sweeps whole-iteration time at 2B.
#  * HELPER LANDING ZONE.  At ppn 7 the fix derives the SMT siblings,
#    which share physical cores with the PEs.  The CCD-first cores
#    (0,8,...,56) are idle at ppn 7 too and are separate physical cores.
#    Untested; helper_ccd is that arm.
#  * ppn.  relays 46/47 measured ppn 7 > ppn 14 on the OLD runtime.
#    Re-measured here, both with and without the ppn-14 rescue config.
#  * NETWORK.  ndev 7 vs 4 vs 1, poll 1 vs 2 -- the scripts and the
#    README disagree and neither was measured on this stack.
# Every arm is gated on the gold pair 424897832 / 185317566.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --job-name=sweep16
#SBATCH --nodes=16
#SBATCH --time=00:45:00
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/sweep0821/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/sweep0821/%x.%j.err

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh

export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export FI_MR_CACHE_MONITOR=userfaultfd
export PMI_MAX_KVS_ENTRIES=4194304

L=/lustre/orion/csc710/scratch/rrao/sweep0821
cd /ccs/home/rrao/paratreet2/examples/fof3 || exit 1
INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
NT=$((SLURM_JOB_NUM_NODES * 8))

NOSMT=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
SMT=1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127
CCD=0,8,16,24,32,40,48,56

WRAP=$L/gcd_wrap.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"

# arm <label> <ppn> <pemap> <leaf> <ndev> <poll> <extra srun args> ; env in $E
arm() {
  local label=$1 ppn=$2 pemap=$3 leaf=$4 ndev=$5 poll=$6; shift 6
  echo "=== ARM $label rep $REP (ppn=$ppn leaf=$leaf ndev=$ndev poll=$poll pemap=$pemap env='$E' srun='$*') ==="
  date +"start %s.%N %F %T"
  # HARD TIMEOUT ON EVERY ARM.  +backend_poll_thread 2 with lci_ndevices 7
  # at ppn 7 HANGS: one thread per device means stride 2 leaves devices 1,
  # 3 and 5 with no poller at all, forever (the pathology named in
  # reconverse comm_backend_lci2.cpp:419-423).  Job 5321594 arm 4 sat
  # there for 19 minutes against 12-18 s for a healthy arm.  A good arm
  # here is ~35 s, so 150 s is 4x headroom, and one wedged arm can no
  # longer eat the whole sweep -- it just reports 124 and moves on.
  env $E \
    timeout --signal=TERM --kill-after=30 150 \
    srun --mpi=cray_shasta --network=job_vni --unbuffered \
      --cpu-bind=none --distribution=block:block \
      --ntasks=$NT --gpus-per-node=8 --export=ALL "$@" \
      "$WRAP" ./FoF3 -f "$INPUT" -d oct -u dist -c stats -l $leaf -i 2 \
      +noisomalloc +ppn $ppn +pemap $pemap +lci_ndevices $ndev +backend_poll_thread $poll
  echo "exit: $?"
  date +"end %s.%N %F %T"
}

DEV="PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1"

pass() {
  E="$DEV"                                    arm base       7 $NOSMT 128 7 1
  E="$DEV"                                    arm leaf12     7 $NOSMT  12 7 1
  E="$DEV"                                    arm leaf32     7 $NOSMT  32 7 1
  E="$DEV"                                    arm leaf64     7 $NOSMT  64 7 1
  E="$DEV"                                    arm leaf96     7 $NOSMT  96 7 1
  E="$DEV"                                    arm leaf192    7 $NOSMT 192 7 1
  E="$DEV"                                    arm leaf256    7 $NOSMT 256 7 1
  E="$DEV"                                    arm ndev4      7 $NOSMT 128 4 1
  E="$DEV"                                    arm ndev1      7 $NOSMT 128 1 1
  E="$DEV"                                    arm poll2      7 $NOSMT 128 7 2
  E="$DEV FOF_HELPER_CPUS=$CCD"               arm helper_ccd 7 $NOSMT 128 7 1
  E="$DEV FOF_NO_AFFINITY_FIX=1"              arm nofix      7 $NOSMT 128 7 1
  E="$DEV"                                    arm ppn14      14 $SMT  128 7 1
  E="$DEV FOF_HELPER_CPUS=$CCD"               arm ppn14_ccd  14 $SMT  128 7 1 --core-spec=0
  E="PARATREET_DEVICE_TREE=0"                 arm cpu_l12    7 $NOSMT  12 7 1
}

# One discarded warm-up: the first run of a job pays page-cache and
# fabric-setup costs that no later run pays, and it must not land on an arm.
REP=warmup
E="$DEV" arm warmup 7 $NOSMT 128 7 1

for REP in 1 2 3; do pass; done
echo "=== SWEEP COMPLETE ==="
