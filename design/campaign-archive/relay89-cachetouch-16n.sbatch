#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay89-cachetouch-16n
#SBATCH --nodes=16
#SBATCH --time=00:50:00
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
# relay89 -- CACHE-TOUCH CENSUS: is fetched tree data actually examined?
#
# relay88 showed both duplicate-fetch mechanisms are zero by construction, so
# the only remaining cache lever is fetching FEWER DISTINCT nodes.  This
# measures whether there are fewer to fetch: of the ~318,415 distinct nodes
# each process pulls in, how many does a certificate never look at?
#
# THREE STATES, not one, per Kale: a single "touched" fraction conflates a
# pruning DEFECT with correct pruning.
#   never   fetched and no certificate ever examined it   -> prefetch defect
#   pruned  examined and correctly rejected               -> the cache working
#   used    descended into, or particles read
# Split canopy/deep at the cache share depth (-D 3): the two have different
# prefetch implications.
#
# COMPILE-TIME GATE (-DFOF_CACHE_TOUCH=1, separate binary), not an env check --
# the write is on the hottest path in the traversal.  MAKE_OPTS is passed to
# src/, fof/ AND examples/fof3/ because Node carries the field: building only
# some layers with it would be an ODR violation on the Node layout.
#   T  FoF3.cpu-touch-v84  03fc8f246ba2d604cc1c2fe0c04ba908  instrumented
#   P  FoF3.cpu-prod-v84   88b5169143c8e57acfdcc7636fd955bd  same patch, no flag
# P is here to price the instrumentation AND to prove the report is inert
# without the flag (its cache_touch line must be ABSENT: the tally is zero, so
# FoF3.C suppresses it).
#
# CPU arm: ppn 7, -l 32, -E 16, FOF_UF_SIZES=0, -u dist, defaults elsewhere.
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
export LCI_ATTR_BACKEND=ofi FI_CXI_RX_MATCH_MODE=hybrid \
       FI_MR_CACHE_MONITOR=userfaultfd PMI_MAX_KVS_ENTRIES=4194304
unset LCI_OFI_THREADING_HINT FOF_NO_AFFINITY_FIX FOF_HIST_DENSE
unset FOF_WAVE FOF_WAVE_MS FOF_UF_SHORTCIRCUIT FOF_ROOT_CENSUS
unset PARATREET_DEVICE_TREE FOF_GPU_PHASE1
export FOF_UF_SIZES=0 FOF_PROCS_PER_PNODE=8
WRAP=$G/scripts/gcd_wrapper.sh
PEMAP7=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
TOUCH=$M/FoF3.cpu-touch-v84; PLAIN=$M/FoF3.cpu-prod-v84
CHP=$M/charm-prod/reconverse-linux-x86_64-amd
CHT=$M/charm-prodtr/reconverse-linux-x86_64-amd

{
  echo "job : $SLURM_JOB_ID  relay87 PE-set split vs walk imbalance + cross-node census"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD)  unionfind $(git -C $M/unionfind rev-parse --short HEAD) +census"
  echo "  touch $(md5sum $TOUCH|cut -d' ' -f1)   plain $(md5sum $PLAIN|cut -d' ' -f1)"
  echo "  nodes $SLURM_NODELIST"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <binary> <expect_touch 0|1>
  local label=$1 bin=$2 want=$3
  local log=$OUT/${label}.log
  export LD_LIBRARY_PATH=$M/lci-install/lib64:$CHP/lib:/opt/rocm-6.2.4/lib:$BASELIB
  unset FOF_PE_SETS
  local t0=$(date +%s) rc c m it lf
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=128 --gpus-per-node=8 --export=ALL -t 20:00 \
    "$WRAP" "$bin" -f "$INPUT" -d oct -u dist -c stats -l 32 -i 1 -E 16 \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; local t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local nt=$(grep -c 'FOF3STAT cache_touch:' "$log" || true)
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF ${lf:-NONE}"
  if [ "$want" = 1 ]; then [ "${nt:-0}" -gt 0 ] || g="$g !!! touch line MISSING from instrumented binary"
                      else [ "${nt:-0}" -eq 0 ] || g="$g !!! touch line PRESENT without the flag"; fi
  printf "REP %-8s rc=%d %-7s Iter0=%9s  %4ds %s\n" "$label" "$rc" "$v" "${it:-NONE}" "$((t1-t0))" "$g"
  grep -E 'FOF3STAT cache:|FOF3STAT cache_touch' "$log" | sed 's/^/      /'
  echo "${it:-0}" >> "$OUT/${label}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -8 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up, DISCARDED"
run W $PLAIN 0
for r in 1 2; do
  echo "################ rep $r"
  run T $TOUCH 1
  run P $PLAIN 0
done

echo "################ SUMMARY"
for a in T P; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
    END{printf "  %-2s n=%d  Iter0 %9.1f ms  [%.1f..%.1f]\n",A,n,i/n,lo,hi}' "$OUT/$a.dat"
done
echo "  T = -DFOF_CACHE_TOUCH=1 (instrumented)    P = same patch, no flag"
echo "=== $(date +%T) DONE -- $OUT"
