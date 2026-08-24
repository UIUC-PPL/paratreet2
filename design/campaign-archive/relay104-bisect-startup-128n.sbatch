#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay104-bisect-startup-128n
#SBATCH --nodes=128
#SBATCH --ntasks=1024
#SBATCH --time=02:30:00
#SBATCH -p batch
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --network=job_vni
#SBATCH -o /lustre/orion/csc710/scratch/lvkale/s3ab/%x-%j.out
#SBATCH -e /lustre/orion/csc710/scratch/lvkale/s3ab/%x-%j.err
#
# relay104 -- BISECT THE 19-MINUTE STARTUP, FROM A KNOWN-GOOD BASE.
#
# relay103 (5334873) came back ALL FOUR SLOW: cpu/gpu x helpers-on/off all
# took 978-1401 s of wall for ~1 s of Iteration 0.  So it is neither the GPU
# codepath nor FOF_HELPER_CPUS.
#
# MY 2x2 HAD A FLAW AND IT MUST BE SAID.  I described it as testing "the
# core-thief fix", but that fix is TWO things: an ALLOCATION (--core-spec=0
# --cpus-per-task=16) and an ENV VAR (FOF_HELPER_CPUS).  The allocation half
# is set in the sbatch header and was therefore present in ALL FOUR arms.  I
# only varied the env var.  So relay103 rules out the GPU codepath and the
# helper pinning, and rules out NOTHING ELSE.
#
# FOUR UNCONTROLLED VARIABLES separate the last known-fast run (relay99,
# ~30 s/rep, ended 18:17) from the first slow one (relay101, 18:23):
#   (1) binary      v89main (1040b63)  ->  v91 (b0f04fc, width audit)
#   (2) leaf        -l 32              ->  -l 128
#   (3) allocation  cpus-per-task=14   ->  core-spec=0 cpus-per-task=16
#   (4) PMI         PMI_MAX_KVS_ENTRIES 4194304 -> 16777216
# and a FIFTH that is not mine at all:
#   (5) THE MACHINE.  Everything before 18:17 was fast and everything after
#       18:23 has been slow, across four jobs and both codepaths.  Ritvik's
#       bigscale runs (256n on 491 GB, 512n on 1.3 TB) began at about 18:05
#       on the same Lustre filesystem this reads its 76 GB from.  A six-minute
#       correlation is suggestive, NOT proof, and it is the cheapest thing to
#       test because it needs no new variables at all.
#
# THIS HEADER IS relay97's: --cpus-per-task=14, NO --core-spec.  That restores
# variable (3) to its known-good value for every arm.
#
# ADAPTIVE, TO AVOID BURNING THE ALLOCATION ON A DEAD HYPOTHESIS.  Arm A is
# the EXACT relay97/99 configuration and should take ~30 s.
#   If A is SLOW  -> the cause is (5), external to everything I changed, and
#                    the script STOPS.  Bisecting my own variables would be
#                    measuring machine weather.
#   If A is FAST  -> the cause is one of (1)(2)(4) and the ladder runs, one
#                    variable at a time from the known-good base.
#
#   A  v89main  -l 32   PMI 4194304    the known-good control
#   B  v89main  -l 32   PMI 16777216   + PMI only
#   C  v89main  -l 128  PMI 4194304    + leaf only
#   D  v91cpu   -l 32   PMI 4194304    + binary only
#   E  v91cpu   -l 128  PMI 16777216   all three together
# -s 128 throughout, CPU throughout: the GPU path is already excluded and
# holding it out keeps the ladder cheap.
#
# 2:30 of wall clock, sized from the OBSERVED 1401 s worst case.  relay101
# died because I sized from CPU timings; this one is sized from what was
# actually measured, and it stops early rather than running to the wall.
set -u
M=$HOME/software/merged
G=$HOME/software/gpu-stalls
INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
GOLD=424897832; GOLDMAX=185317566
SLOW_S=200          # anything past this is "slow"; fast reps were 27-50 s
OUT=/lustre/orion/csc710/scratch/$USER/s3ab/$SLURM_JOB_ID
mkdir -p "$OUT"; cd "$OUT"

source /opt/cray/pe/lmod/lmod/init/bash
module load craype-x86-trento libfabric/2.3.1 craype-network-ofi \
  perftools-base/24.11.0 xpmem/1.0.1-1.5_1_gfb6998056825 cray-pmi/6.1.15 \
  Core/25.03 hwloc/2.11.1 gcc-native/13.2 craype/2.7.33 cray-dsmml/0.3.0 \
  cray-mpich/8.1.31 cray-libsci/24.11.0 PrgEnv-gnu/8.6.0 rocm/6.2.4 \
  cmake/3.30.5 2>&1 | tail -1
BASELIB=$(echo "${LD_LIBRARY_PATH:-}" | tr ':' '\n' | grep -v "^$HOME/software/" | grep -v '^$' | paste -sd:)
export LCI_ATTR_BACKEND=ofi FI_CXI_RX_MATCH_MODE=hybrid FI_MR_CACHE_MONITOR=userfaultfd
unset LCI_OFI_THREADING_HINT FOF_NO_AFFINITY_FIX FOF_HIST_DENSE FOF_HELPER_CPUS
unset FOF_WAVE FOF_WAVE_MS FOF_UF_SHORTCIRCUIT FOF_ROOT_CENSUS FOF_PE_SETS
unset PARATREET_DEVICE_TREE FOF_GPU_PHASE1
export FOF_UF_SIZES=0
WRAP=$G/scripts/gcd_wrapper.sh
PEMAP7=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
CH=$M/charm-prod/reconverse-linux-x86_64-amd
export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:/opt/rocm-6.2.4/lib:$BASELIB
V89=$M/FoF3.cpu-prod-v89main
V91=$M/FoF3.cpu-prod-v91

{
  echo "job : $SLURM_JOB_ID  relay104  bisect the startup cost, 128 NODES, 2B, CPU only"
  echo "  v89main $(md5sum $V89|cut -d' ' -f1)  (paratreet2 1040b63)"
  echo "  v91cpu  $(md5sum $V91|cut -d' ' -f1)  (paratreet2 b0f04fc)"
  echo "  header: cpus-per-task=14, NO core-spec -- relay97's known-good allocation"
  echo "  nodes $SLURM_NNODES  tasks $SLURM_NTASKS  started $(date '+%H:%M:%S')"
} | tee "$OUT/provenance.txt"
echo

run () {   # run <label> <bin> <leaf> <pmi> <note>
  local label=$1 BIN=$2 LEAF=$3 PMI=$4 NOTE=$5
  export PMI_MAX_KVS_ENTRIES=$PMI
  local log=$OUT/${label}.log t0 t1 rc it lf
  echo "=== $(date '+%H:%M:%S') START $label  $NOTE"
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=1024 --gpus-per-node=8 --export=ALL -t 40:00 \
    "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l "$LEAF" -i 1 -E 16 -D 3 -s 128 \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  local c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  local m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  WALL=$((t1-t0))
  local verdict=FAST; [ "$WALL" -gt "$SLOW_S" ] && verdict=SLOW
  printf "REP %-3s %-34s rc=%d %-7s  WALL %5ds  %-4s  Iter0 %9s ms  leaf %s\n" \
         "$label" "$NOTE" "$rc" "$v" "$WALL" "$verdict" "${it:-NONE}" "${lf:-?}"
  [ "${lf:-N}" = "$LEAF" ] || echo "      !!! LEAF ${lf:-NONE} wanted $LEAF"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; grep -v '^srun' "$log" | tail -8 | sed 's/^/      | /'; }
  echo
}

echo "################ ARM A -- the known-good control.  relay97/99 got ~30 s."
run A "$V89" 32 4194304 "v89main l32 pmi4M (known good)"

if [ "$WALL" -gt "$SLOW_S" ]; then
  cat <<MSG
################ STOPPING.  ARM A IS SLOW ($WALL s against ~30 s in relay97/99).

The EXACT configuration that ran in 30 s at 16:44 and 18:13 today is slow now,
with the same binary, the same leaf, the same allocation shape and the same
PMI setting.  NONE of the four variables I introduced can explain that.

The cause is therefore EXTERNAL to every change made since 18:17 -- machine or
filesystem state, with Ritvik's 256-node and 512-node bigscale reads (491 GB
and 1.3 TB, same Lustre) as the leading candidate on timing alone.

Bisecting my own variables now would be measuring machine weather, so the
remaining arms are SKIPPED rather than burned.  What this run has established
is worth more than the ladder would have been: relay101's -35.9% GPU capping
result is still trustworthy, because every arm inside it saw the same machine.
MSG
  echo "=== $(date +%T) DONE (early) -- $OUT"
  exit 0
fi

echo "################ ARM A WAS FAST.  The cause is one of my variables."
run B "$V89" 32  16777216 "v89main l32 pmi16M (+PMI)"
run C "$V89" 128 4194304  "v89main l128 pmi4M (+leaf)"
run D "$V91" 32  4194304  "v91cpu  l32 pmi4M (+binary)"
run E "$V91" 128 16777216 "v91cpu  l128 pmi16M (all three)"

echo "################ SUMMARY"
echo "  Compare each arm against A.  The one that goes SLOW names the cause;"
echo "  if only E is slow, it is an interaction and not any single variable."
echo "=== $(date +%T) DONE -- $OUT"
