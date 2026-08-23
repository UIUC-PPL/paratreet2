#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay95-canopy-128n
#SBATCH --nodes=128
#SBATCH --ntasks=1024
#SBATCH --time=01:00:00
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
# relay95 -- (a) the canopy_fills counter across caps, (b) the 128-node TRACE
# that must confirm the recvTC serialisation before item (2) is implemented.
#
# (a) relay93 reported "requests flat across every cap".  That was measured on
# the wrong arm: a canopy miss ABOVE the TreePiece level goes
# Traverser.h:186 -> tc_proxy.requestData -> CacheManager::restoreData, which
# never touches serviceRequest.  The new canopy_fills counter closes that hole.
# PREDICTION: canopy_fills RISES as the cap falls, and rises sharply at -s 1 --
# that is the mechanism behind relay94's +101 ms and its 230 ms spread.
#
# (b) THE TRACE.  Item (2) replaces the hand-rolled canopy gather with the
# reduction that already exists, but the serialisation must be CONFIRMED at
# 128 nodes first, not extrapolated from 16.  Expect ONE PE (the Driver's,
# PE 0) executing ~69,670 recvTC -- 34,835 canopies x 2 accumulation rounds --
# while no other PE executes any.  At 16 nodes this was 9,258 on PE 0 and ~5 ms;
# the claim is that it scales with the canopy count and so with P.
#   -s does NOT affect this: the cap applies only to send_size in
#   Driver::loadCache, AFTER every recvTC has already arrived and after
#   sortStorage has sorted all of them.  So one traced arm suffices and the
#   cap it runs at is irrelevant to what is being measured.
#
#   counters:  U / 128 / 1        2 reps each, untraced (FoF3.cpu-prod-v87)
#   trace:     one rep at cap 128 (FoF3.cpu-prodtr-v87), traces KEPT
#
# Tracing costs +2.7-3.2%, so the traced arm's wall is not comparable.
# Binaries FoF3.cpu-prod-v87   e681a3a372a75dbdd2840403067729ae
#          FoF3.cpu-prodtr-v87 b4eaafb7ff399ba4582b74557024ce5f
# branch relay92-loadcache-profile @ 3548ac3 (3 commits on origin/main 55f2efc).
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
unset FOF_WAVE FOF_WAVE_MS FOF_UF_SHORTCIRCUIT FOF_ROOT_CENSUS FOF_PE_SETS
unset PARATREET_DEVICE_TREE FOF_GPU_PHASE1
export FOF_UF_SIZES=0
WRAP=$G/scripts/gcd_wrapper.sh
PEMAP7=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
CH=$M/charm-prod/reconverse-linux-x86_64-amd
export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:/opt/rocm-6.2.4/lib:$BASELIB
PROD=$M/FoF3.cpu-prod-v87; PTR=$M/FoF3.cpu-prodtr-v87
CHT=$M/charm-prodtr/reconverse-linux-x86_64-amd

{
  echo "job : $SLURM_JOB_ID  relay91  -D 2 vs -D 3 at 128 NODES, 2B, 1024 procs, 7168 PEs"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD)  unionfind $(git -C $M/unionfind rev-parse --short HEAD)"
  echo "  prod $(md5sum $PROD|cut -d' ' -f1)  prodtr $(md5sum $PTR|cut -d' ' -f1)"
  echo "  paratreet2 branch $(git -C $M/paratreet2 rev-parse --abbrev-ref HEAD) @ $(git -C $M/paratreet2 rev-parse --short HEAD)"
  echo "  nodes $SLURM_NNODES  tasks $SLURM_NTASKS"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <cap; "U"=unset> [traced 0|1]
  local label=$1 CAP=$2 TR=${3:-0}
  local SARG="" bin=$PROD extra=""
  [ "$CAP" != "U" ] && SARG="-s $CAP"
  if [ "$TR" = 1 ]; then bin=$PTR; mkdir -p "$OUT/traces/$label"
    extra="+traceroot $OUT/traces/$label +logsize 20000000 +sumDetail"
    export LD_LIBRARY_PATH=$M/lci-install/lib64:$CHT/lib:/opt/rocm-6.2.4/lib:$BASELIB
  else export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:/opt/rocm-6.2.4/lib:$BASELIB; fi
  local log=$OUT/${label}.log t0 t1 rc c m it lf dd
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=1024 --gpus-per-node=8 --export=ALL -t 25:00 \
    "$WRAP" "$bin" -f "$INPUT" -d oct -u dist -c stats -l 32 -i 1 -E 16 -D 3 $SARG \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 $extra > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local shipped=$(grep -m1 -o 'shipped [0-9]*' "$log"|awk '{print $2}')
  local bcast=$(grep -c 'Broadcasting every tree canopy' "$log" || true)
  local wa=$(grep -m1 -o 'phase3_walk [0-9.]*' "$log"|awk '{print $2}')
  local lc=$(grep -m1 -o 'loadCache [0-9.]*' "$log"|awk '{print $2}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF ${lf:-NONE}"
  if [ "$CAP" = "U" ]; then
    [ "${bcast:-0}" -eq 1 ] || g="$g !!! control should print the Broadcasting line"
    [ "${shipped:-0}" -gt 30000 ] || g="$g !!! control shipped only ${shipped:-?}"
  else
    [ "${bcast:-1}" -eq 0 ] || g="$g !!! CAP IGNORED -- Broadcasting line still printed"
    [ "${shipped:-0}" = "$CAP" ] || g="$g !!! shipped ${shipped:-?} wanted $CAP"
  fi
  printf "REP %-6s cap=%-6s rc=%d %-7s Iter0=%10s walk=%-7s loadCache=%-8s shipped=%-6s %4ds %s\n" \
         "$label" "$CAP" "$rc" "$v" "${it:-NONE}" "${wa:-NONE}" "${lc:-NONE}" "${shipped:-?}" "$((t1-t0))" "$g"
  grep -E 'FOF3STAT cache:|FOF3STAT memory_MB' "$log" | sed 's/^/      /'
  grep -m1 'FOF3STAT time_s: uf2_setup' "$log" | sed 's/FOF3STAT //;s/^/      /'
  if [ "$TR" = 1 ]; then
    local n=$(ls "$OUT/traces/$label" 2>/dev/null | grep -c '\.log\.gz$')
    echo "      TRACES log.gz $n (want 7168)"; [ "$n" = 7168 ] || echo "      !!! INCOMPLETE"
  fi
  echo "${it:-0} ${wa:-0}" >> "$OUT/${label}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -12 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up / shakedown, DISCARDED"
run W U
for r in 1 2; do
  echo "################ rep $r"
  for C in U 128 1; do run "c$C" "$C"; done
done
echo "################ TRACED arm at cap 128 (traces KEPT)"
run tr128 128 1

echo "################ SUMMARY"
for a in cU c128 c1; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;w+=$2;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
    END{printf "  %-2s n=%d  Iter0 %10.1f ms  [%.1f..%.1f]   walk %6.3f s\n",A,n,i/n,lo,hi,w/n}' "$OUT/$a.dat"
done
echo "  relay93 at 128 nodes: unset 1810.7  8192 1685.0  2048 1553.7  512 1634.3  128 1531.7"
echo "  64-node reference 1728.9 ms"
echo "=== $(date +%T) DONE -- $OUT"
