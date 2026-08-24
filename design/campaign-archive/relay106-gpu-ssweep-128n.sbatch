#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay106-gpu-ssweep-128n
#SBATCH --nodes=128
#SBATCH --ntasks=1024
#SBATCH --time=02:00:00
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
# relay106 -- relay101 REDONE WITHOUT --core-spec=0, WHICH IS WHAT KILLED IT.
#
# relay101 (5333878) TIMED OUT after 4 of 16 arms because every rep cost
# 937-1274 s of wall for 1.4 s of Iteration 0.  relay103/104/105 chased that
# through five wrong hypotheses to the real one:
#   relay105 (5335233), --core-spec=0 --cpus-per-task=16 header, otherwise
#   relay104-A's known-good configuration:
#     P  srun default            1122 s  SLOW   Iter0 1516.9 ms
#     Q  srun --cpus-per-task=14 1184 s  SLOW   Iter0 1539.9 ms
#   against relay104-A WITHOUT that header:  49 s  Iter0 1514.9 ms
# BOTH arms slow -> it is --core-spec=0 ITSELF, not the cpuset width.  And
# Iteration 0 is UNCHANGED (1516.9 / 1539.9 / 1514.9), so the header buys
# nothing and costs ~1100 s.  IT IS REMOVED HERE.
#
# THE ORIGINAL RESULT STANDS AND IS WHAT THIS EXTENDS.  relay101's four
# surviving arms were internally valid -- same machine, same header for all of
# them:  U 1372.4 ms / 2048 891.2 / 512 879.6, i.e. capping worth -35.9%, with
# pool_MB 157,694 -> 77,042.  n=1 each.  This run gets the missing reps and the
# missing 128 and 32 arms.
#
# ALSO CARRIED OVER FROM THE CHASE: PMI_MAX_KVS_ENTRIES stays at the raised
# 16777216 (relay104-B measured it harmless at 1024 processes, 26 s) because
# relay100 proved 4194304 is too small at 2048, and one setting for both
# scales is worth more than a scale-dependent one.
#
# 2 reps per arm rather than 3, and 2 hours rather than 1:30, both sized from
# what was MEASURED and not from what I expected.  The per-rep wall is printed
# so the sizing can be checked rather than assumed.
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
       FI_MR_CACHE_MONITOR=userfaultfd PMI_MAX_KVS_ENTRIES=16777216
unset LCI_OFI_THREADING_HINT FOF_NO_AFFINITY_FIX FOF_HIST_DENSE
unset FOF_WAVE FOF_WAVE_MS FOF_UF_SHORTCIRCUIT FOF_ROOT_CENSUS FOF_PE_SETS
export FOF_UF_SIZES=0
# THE GPU CODEPATH -- both are required, not optional.
export PARATREET_DEVICE_TREE=1
export FOF_GPU_PHASE1=1
# THE CORE-THIEF FIX (patch 0020): ROCm helper threads inherit the caller's
# affinity and land on a compute core.  --core-spec=0 --cpus-per-task=16 makes
# the CCD-first service cores reachable; FOF_HELPER_CPUS puts one helper per
# process on one, sharing that process's L3.  Measured -22.8% at ppn 7.
unset FOF_HELPER_CPUS   # needs --core-spec=0, which is what broke relay101
WRAP=$G/scripts/gcd_wrapper.sh
PEMAP7=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
CH=$M/charm-prod/reconverse-linux-x86_64-amd
export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:/opt/rocm-6.2.4/lib:$BASELIB
BIN=$M/FoF3.gpu-prod-v91

{
  echo "job : $SLURM_JOB_ID  relay101  GPU -s sweep, 128 NODES, 2B, 1024 procs, 7168 PEs"
  echo "  bin $(md5sum $BIN|cut -d' ' -f1)  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD) (want b0f04fc)"
  echo "  dirty p2=$(git -C $M/paratreet2 status --porcelain -uno|grep -v ' utility$'|wc -l)  uf=$(git -C $M/unionfind status --porcelain -uno|wc -l) (relay87 census)"
  echo "  GPU codepath: PARATREET_DEVICE_TREE=$PARATREET_DEVICE_TREE FOF_GPU_PHASE1=$FOF_GPU_PHASE1"
  echo "  core-thief fix: OFF (relay105: --core-spec=0 costs ~1100 s of startup at 128 nodes and changes Iteration 0 by nothing)"
  echo "  nodes $SLURM_NNODES  tasks $SLURM_NTASKS"
} | tee "$OUT/provenance.txt"
echo

run () {   # run <label> <cap; "U" = unset>
  local label=$1 CAP=$2
  local SARG=""; [ "$CAP" != "U" ] && SARG="-s $CAP"
  local log=$OUT/${label}.log t0 t1 rc c m it lf
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=1024 --gpus-per-node=8 --export=ALL -t 25:00 \
    "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l 128 -i 1 -E 16 -D 3 $SARG \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local pk=$(grep -m1 'FOF3STAT loadcache_pack:' "$log")
  local raw=$(echo "$pk"|grep -o 'raw_canopies [0-9]*'|awk '{print $2}')
  local shp=$(echo "$pk"|grep -o 'shipped [0-9]*'|awk '{print $2}')
  local srt=$(echo "$pk"|grep -o 'sort_s [0-9.]*'|awk '{print $2}')
  local lc=$(grep -m1 -o 'loadCache [0-9.]*' "$log"|awk '{print $2}')
  local wa=$(grep -m1 -o 'phase3_walk [0-9.]*' "$log"|awk '{print $2}')
  local p1=$(grep -m1 -o 'phase1 [0-9.]*' "$log"|awk '{print $2}')
  local pool=$(grep -m1 -o 'pool_MB [0-9.]*' "$log"|awk '{print $2}')
  local fills=$(grep -m1 -o 'canopy_fills [0-9]*' "$log"|awk '{print $2}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  # LEAF GATE on the PRINTED value -- the campaign's most expensive past bug.
  local g=ok; [ "${lf:-N}" != "128" ] && g="!!! LEAF ${lf:-NONE} WANTED 128"
  [ "$rc" = 0 ] || g="!!! rc=$rc"
  # GPU-ARM GATE: if the device path silently did not engage, the wall would
  # look like a CPU wall and the whole sweep would be mislabelled.
  local gp=$(grep -c 'GPU\|device' "$log" || true)
  # COLLECT-GATE CHECK: the gate is merged, so a cap must shrink the collect.
  if [ "$CAP" = "U" ]; then
    [ "${raw:-0}" -gt 50000 ] || g="$g !!! uncapped collected only ${raw:-?}"
  else
    [ "${shp:-0}" = "$CAP" ] || g="$g !!! shipped ${shp:-?} wanted $CAP"
    [ "${raw:-999999}" -lt 50000 ] || g="$g !!! MERGED COLLECT GATE DID NOT FIRE (raw ${raw:-?})"
  fi
  printf "REP %-9s cap=%-5s rc=%d %-7s Iter0=%9s p1=%-7s walk=%-7s loadCache=%-7s raw=%-6s shipped=%-6s sort_s=%-7s fills=%-8s %4ds %s\n" \
         "$label" "$CAP" "$rc" "$v" "${it:-NONE}" "${p1:-NONE}" "${wa:-NONE}" "${lc:-NONE}" \
         "${raw:-?}" "${shp:-?}" "${srt:-?}" "${fills:-?}" "$((t1-t0))" "$g"
  printf "      leaf %-5s pool_MB %-12s gpu-mentions %s\n" "${lf:-?}" "${pool:-?}" "$gp"
  grep -m1 'FOF3STAT time_s:' "$log" | sed 's/FOF3STAT //;s/^/      /'
  echo "${it:-0} ${wa:-0} ${lc:-0} ${pool:-0}" >> "$OUT/${label%%-*}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; grep -v '^srun: error' "$log" | tail -12 | sed 's/^/      | /'; }
  echo
}

echo "################ warm-up / cold 76 GB read, DISCARDED"
run W U

for r in 1 2; do
  echo "################ rep $r"
  for C in U 2048 512 128 32; do run "c$C-r$r" "$C"; done
done

echo "################ SUMMARY"
for a in cU c2048 c512 c128 c32; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;w+=$2;l+=$3;p+=$4;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
    END{printf "  %-6s n=%d  Iter0 %9.1f ms [%.1f..%.1f]  spread %4.1f%%  walk %6.3f s  loadCache %7.1f ms  pool_MB %9.0f\n",
               A,n,i/n,lo,hi,(hi-lo)/(i/n)*100,w/n,l/n*1000,p/n}' "$OUT/$a.dat"
done
echo
echo "  TWO THINGS TO READ OFF THIS:"
echo "   1. the GPU crossover -- does capping win at 128 nodes, and by how much"
echo "   2. the SPREAD column.  Under the core-thief fix the campaign's note"
echo "      says <1%.  If it is <1% here, the relay97/98/99 disagreement was"
echo "      allocation noise and is explained.  If it is still 2-4%, it is not,"
echo "      and no -s recommendation should be quoted to better than that."
echo "  CPU reference at 128 nodes, DIFFERENT codepath and baseline, shape only:"
echo "    relay93/94: best cap 128 at 1499.5 ms; uncapped ~1736-1947 ms"
echo "=== $(date +%T) DONE -- $OUT"
