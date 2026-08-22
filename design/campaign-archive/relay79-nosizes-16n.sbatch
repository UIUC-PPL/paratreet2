#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay79-nosizes-16n
#SBATCH --nodes=16
#SBATCH --time=00:30:00
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
# relay79 -- IS add_size DEAD WORK?  FOF_UF_SIZES=0 skips it.
#
# relay78 section 10 established from the source that the union-find `size`
# field is never read in FoF3:
#   app seeds v.size (FoFPhase1.h:1145); library maintains it via add_size;
#   set_component propagates (componentNumber, componentSize); but
#   collectComponentLabels (unionFindLib.h:146) copies componentNumber ONLY,
#   and lazy mode is on by construction (unionFindInitOnePerNode broadcasts
#   enableLazyMode).  componentSize appears once in the app, as "= -1", never
#   read.  prune_components / return_vertices / report_surviving_components are
#   never called, and the size-consuming merge_count_results path is in #if 0.
#   Sizes and max_size come from the LABEL histogram at the end:
#   depositLabelCounts -> histogramShard -> collectTouchedCounts.
#
# COST OF THE DEAD FIELD: 479,445 add_size calls per run, 41,377 of them
# forwarding hops, and 145,129 of 476,945 protocol messages inside the 265 ms
# drain -- 30.4% of drain traffic.
#
# *** THE EXACTNESS GATE IS THE TEST OF THE ANALYSIS.  The gate checks
#     components AND max_size (424897832 / 185317566).  If `size` were read
#     anywhere on the path to either number, arm B would come out DIFFERS.
#     An EXACT arm B is direct evidence that the field is dead. ***
#
#   A  FOF_UF_SIZES=1  (default, current behaviour)   x2
#   B  FOF_UF_SIZES=0  (add_size calls skipped)       x2
#
# The knob gate is positive on both sides: A must show addsize_root > 0 and
# addsize_SKIPPED == 0, B the reverse.
#
# CAVEAT ON THE EXPECTED RESULT: add_size is NOT on the find dependence chain.
# It is fire-and-forget, issued just before the parent write, and nothing waits
# on it.  In a latency-bound drain the length is set by the longest chain, so
# removing a side flow need not shorten it.  What it can do is stop delaying
# queued find messages on the ~39 PEs that are busy (Charm entry methods are
# non-preemptive).  A null result here is a real result.
#
# Binary FoF3.cpu-prod-nsz md5 e87d47d9814a79bebc328225fe42fa17, working-tree
# patch on 57f2395 / d72ee66, NOT pushed:
#   patches/relay79-unionfind.diff + relay79-paratreet2.diff (see patches/APPLY.md)
# CPU best: ppn 7, -l 32, -E 16.  Short-circuit OFF in both arms.
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
unset PARATREET_DEVICE_TREE FOF_GPU_PHASE1 FOF_WAVE FOF_WAVE_MS
WRAP=$G/scripts/gcd_wrapper.sh
PEMAP7=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
CH=$M/charm-prod/reconverse-linux-x86_64-amd
export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:/opt/rocm-6.2.4/lib:$BASELIB
BIN=$M/FoF3.cpu-prod-nsz

{
  echo "job : $SLURM_JOB_ID  relay78 union-find branch census, 16 nodes, 2B, 896 PEs"
  echo "  bin $BIN md5 $(md5sum $BIN|cut -d' ' -f1)  (want e87d47d9814a79bebc328225fe42fa17)"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD) +patch  unionfind $(git -C $M/unionfind rev-parse --short HEAD) +patch"
  echo "  UFSTAT $(strings $BIN|grep -c UFSTAT)  FOF_UF_SHORTCIRCUIT $(strings $BIN|grep -c FOF_UF_SHORTCIRCUIT)"
  echo "  nodes $SLURM_NODELIST"
} | tee "$OUT/provenance.txt"; echo

unset FOF_UF_SHORTCIRCUIT
run () {   # run <label> <sizes 0|1>
  local label=$1 sz=$2
  local log=$OUT/${label}.log
  local t0 t1 rc c m it lf
  export FOF_UF_SIZES=$sz
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=128 --gpus-per-node=8 --export=ALL -t 12:00 \
    "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l 32 -i 1 -E 16 \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local eb=$(grep -m1 -o 'stream batch (-E): [0-9]*' "$log"|awk '{print $NF}')
  local uf=$(grep -m1 -o ' uf2 [0-9.]*' "$log"|awk '{print $2}')
  local wa=$(grep -m1 -o 'phase3_walk [0-9.]*' "$log"|awk '{print $2}')
  local ncen=$(grep -c '^\[UFSTAT\]' "$log" || true)
  local asr=$(grep -m1 'addsize_root' "$log"|awk '{print $NF}')
  local ask=$(grep -m1 'addsize_SKIPPED' "$log"|awk '{print $NF}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF ${lf:-NONE}"
  [ "${eb:-N}" != "16" ] && g="$g !!! -E printed ${eb:-NONE}"
  [ "${ncen:-0}" -ge 15 ] || g="$g !!! CENSUS MISSING (lines=${ncen:-0})"
  if [ "$sz" = 0 ]; then
    [ "${ask:-0}" -gt 0 ] || g="$g !!! SIZES KNOB DID NOT TAKE (skipped=${ask:-0})"
    [ "${asr:-1}" -eq 0 ] || g="$g !!! add_size still ran (root=$asr)"
  else
    [ "${asr:-0}" -gt 0 ] || g="$g !!! add_size did not run with knob on"
    [ "${ask:-1}" -eq 0 ] || g="$g !!! skipped>0 with knob on"
  fi
  printf "REP %-3s rc=%d %-7s Iter0=%9s  walk=%-7s uf2=%-7s %3ds %s\n" \
         "$label" "$rc" "$v" "${it:-NONE}" "${wa:-NONE}" "${uf:-NONE}" "$((t1-t0))" "$g"
  grep '^\[UFSTAT\]' "$log" | sed 's/^/      /'
  echo "${it:-0} ${wa:-0} ${uf:-0}" >> "$OUT/${label}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -6 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up, DISCARDED"
run W 1
echo "################ rep 1"
run A 1; run B 0
echo "################ rep 2"
run A 1; run B 0

echo "################ SUMMARY"
for a in A B; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;w+=$2;u+=$3;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
    END{printf "  %-2s n=%d  Iter0 %8.1f ms  [%.1f..%.1f]  walk %6.3f s  uf2 %6.3f s\n",A,n,i/n,lo,hi,w/n,u/n}' "$OUT/$a.dat"
done
echo "  relay77 arm A, no census, same config: 4634.1 ms [4626.6..4641.5]"
echo "=== $(date +%T) DONE -- $OUT"
