#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay105-corespec-startup-128n
#SBATCH --nodes=128
#SBATCH --ntasks=1024
#SBATCH --time=01:30:00
#SBATCH -p batch
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=16
#SBATCH --core-spec=0
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --network=job_vni
#SBATCH -o /lustre/orion/csc710/scratch/lvkale/s3ab/%x-%j.out
#SBATCH -e /lustre/orion/csc710/scratch/lvkale/s3ab/%x-%j.err
#
# relay105 -- CONFIRM THE LAST SUSPECT: THE ALLOCATION HEADER ITSELF.
#
# relay104 (5335196) ran a ladder from the known-good configuration and EVERY
# ARM CAME BACK FAST, including arm E which combined all three of the
# variables I had introduced:
#   A  v89main l32  pmi 4M    49 s   FAST   (reproduces relay97/99)
#   B  v89main l32  pmi 16M   26 s   FAST   -> PMI is not it
#   C  v89main l128 pmi 4M    29 s   FAST   -> the leaf is not it
#   D  v91cpu  l32  pmi 4M    31 s   FAST   -> the b0f04fc binary is not it
#   E  v91cpu  l128 pmi 16M   32 s   FAST   -> nor any combination of them
# The machine is not it either: arm A reproduced 49 s at 22:53, and a direct
# Lustre probe gave 3.3 GB/s single-stream with the project queue empty.
#
# ONE VARIABLE IS LEFT, AND IT IS THE ONE NEITHER PREVIOUS JOB COULD VARY,
# because it lives in the sbatch header and not in the srun line:
#       --core-spec=0 --cpus-per-task=16
# relay103 had it in ALL FOUR cells, which is exactly why all four were slow
# and why that 2x2 could not see it.  relay104 did not have it in ANY arm,
# which is why all five were fast.  It is the only thing separating
# relay104-E (32 s) from relay103-A (978 s).
#
# THIS HEADER CARRIES IT.  Both arms otherwise use relay104-A's known-good
# configuration -- v89main, -l 32, PMI 4194304, -s 128, CPU, no helper pinning.
#
#   P  srun default              (16 CPUs/task, the whole node incl. SMT
#                                 siblings and the freed service cores)
#   Q  srun --cpus-per-task=14   (narrows the cpuset back to relay97's width
#                                 inside the SAME core-spec=0 allocation)
# P SLOW and Q FAST  -> it is the CPUSET WIDTH (16 vs 14), not --core-spec
# P SLOW and Q SLOW  -> it is --core-spec=0 itself
# both FAST          -> the effect needs the env var too, and I have to go back
#
# WHY THIS MATTERS BEYOND TONIGHT.  The campaign's note records the core-thief
# fix as -22.8% at ppn 7 -- MEASURED AT 16 NODES.  Nothing has checked what it
# costs at 128, because until relay103 no report printed wall-minus-iteration;
# every script showed a per-rep seconds column that I read as launch overhead.
# If the fix costs ~950 s of startup at 128 nodes it is not a fix at scale, and
# the note needs a scale caveat rather than a flat recommendation.
#
# Iteration 0 is reported too: if the -22.8% is still there in P, the fix helps
# the ITERATION and hurts the STARTUP, which is a trade worth stating exactly.
set -u
M=$HOME/software/merged
G=$HOME/software/gpu-stalls
INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
GOLD=424897832; GOLDMAX=185317566
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
export PMI_MAX_KVS_ENTRIES=4194304
unset LCI_OFI_THREADING_HINT FOF_NO_AFFINITY_FIX FOF_HIST_DENSE FOF_HELPER_CPUS
unset FOF_WAVE FOF_WAVE_MS FOF_UF_SHORTCIRCUIT FOF_ROOT_CENSUS FOF_PE_SETS
unset PARATREET_DEVICE_TREE FOF_GPU_PHASE1
export FOF_UF_SIZES=0
WRAP=$G/scripts/gcd_wrapper.sh
PEMAP7=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
CH=$M/charm-prod/reconverse-linux-x86_64-amd
export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:/opt/rocm-6.2.4/lib:$BASELIB
BIN=$M/FoF3.cpu-prod-v89main

{
  echo "job : $SLURM_JOB_ID  relay105  is the core-spec=0/cpt=16 HEADER the startup cost?"
  echo "  bin $(md5sum $BIN|cut -d' ' -f1)  (v89main, paratreet2 1040b63 -- relay104-A's binary)"
  echo "  header: --core-spec=0 --cpus-per-task=16  (THE SUSPECT)"
  echo "  everything else is relay104-A: -l 32, PMI 4194304, -s 128, CPU, no helper pinning"
  echo "  relay104-A got 49 s.  relay103-A, same config plus this header, got 978 s."
  echo "  nodes $SLURM_NNODES  tasks $SLURM_NTASKS  started $(date '+%H:%M:%S')"
} | tee "$OUT/provenance.txt"
echo

run () {   # run <label> <extra srun args> <note>
  local label=$1 EXTRA=$2 NOTE=$3
  local log=$OUT/${label}.log t0 t1 rc it lf
  echo "=== $(date '+%H:%M:%S') START $label  $NOTE"
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=1024 --gpus-per-node=8 --export=ALL -t 40:00 $EXTRA \
    "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l 32 -i 1 -E 16 -D 3 -s 128 \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  local c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  local m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local wall=$((t1-t0))
  local verdict=FAST; [ "$wall" -gt 200 ] && verdict=SLOW
  printf "REP %-3s %-32s rc=%d %-7s  WALL %5ds  %-4s  Iter0 %9s ms  leaf %s\n" \
         "$label" "$NOTE" "$rc" "$v" "$wall" "$verdict" "${it:-NONE}" "${lf:-?}"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; grep -v '^srun' "$log" | tail -8 | sed 's/^/      | /'; }
  echo
}

run P ""                     "srun default (16 cpus/task)"
run Q "--cpus-per-task=14"   "srun --cpus-per-task=14"

echo "################ VERDICT"
echo "  relay104-A (no core-spec header, otherwise identical):  49 s"
echo "  relay103-A (this header, v91/l128, helpers off):       978 s"
echo "  P SLOW, Q FAST -> the cpuset WIDTH is the cost, not --core-spec"
echo "  P SLOW, Q SLOW -> --core-spec=0 itself is the cost"
echo "  both FAST      -> the env var is needed too; go back to relay103's cells"
echo "=== $(date +%T) DONE -- $OUT"
