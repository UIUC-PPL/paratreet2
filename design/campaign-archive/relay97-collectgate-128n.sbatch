#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay97-collectgate-128n
#SBATCH --nodes=128
#SBATCH --ntasks=1024
#SBATCH --time=01:30:00
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
# relay97 -- ITEM 3: THE CANOPY COLLECT GATE, MEASURED AT 128 NODES.
#
# Kale's catch: -s caps what Driver::loadCache SHIPS but not what it COLLECTS.
# Every TreeCanopy element messaged the driver regardless, so all of them
# arrived and were sorted before anything was capped -- relay93's trace found
# 69,670 recvTC invocations ON PE 0 AND ON NO OTHER PE (34,835 canopies x 2
# accumulation rounds), 37.6 ms of one PE inside a 481 ms window.
#
# canopy-collect-gate f9faa0f gates the driver-send on the same -s, keeping
# WHOLE LEVELS: the smallest b^(d+1) with (b^(d+1)-1)/(b-1) >= num_share_nodes.
# At -s 128 with branch factor 8 that is want=897 -> limit=4096, i.e. indices
# < 4096 = depths 0..3 = 1+8+64+512 = 585 elements, x2 rounds = 1,170 sends.
#
# WHAT TO WATCH, in Kale's order:
#   (1) raw_canopies  69,670 -> ~1,170        the recvTC count, read directly
#                                             off FOF3STAT loadcache_pack, so
#                                             NO TRACE IS NEEDED for it
#   (2) sort_s        -> near zero            storage now ARRIVES small instead
#                                             of being trimmed after the fact
#   (3) loadCache phase timer, and Iter0
#
# THE SHIP IS UNCHANGED AND THAT IS THE POINT.  sortStorage sorts by key, the
# element index IS the prefix-coded key, so the shipped prefix is the smallest
# indices -- all 128 of which are < 4096.  Both arms therefore ship a BITWISE
# IDENTICAL 128-entry pack and the walk sees exactly the same starter cache.
# Any Iter0 difference is the collect alone.
#
# HONEST EXPECTATION: the mechanism is ~37.6 ms (recvTC) + sort, against an
# Iter0 near 1500 ms -- about 3%.  relay96 measured a rep-to-rep spread at
# 64 nodes as wide as [1670.9..1771.7] (6%).  SO THE WALL MAY NOT SEPARATE.
# The phase timer and the counters are the primary readout; Iter0 is secondary
# and is reported with its full range, not as a point estimate.
#
# ARMS (interleaved, 3 reps of the pair; exactness-gated)
#   M   FoF3.cpu-prod-v89main  paratreet2 main 1040b63                -s 128
#   G   FoF3.cpu-prod-v89gate  paratreet2 canopy-collect-gate f9faa0f -s 128
#   MU  main, -s UNSET   } negative control: the gate is a documented no-op
#   GU  gate, -s UNSET   } when num_share_nodes <= 0, so MU and GU must agree
#                          and both must show raw_canopies ~69,670
#
# NOT COMPARABLE TO relay93/94/96 IN ABSOLUTE TERMS: main moved 52decf6 ->
# 9798a06 (Ritvik, "avoid unnecessary reads for fof") -> 1040b63 since v88, so
# both arms were rebuilt at 1040b63 and only the A/B inside this job is valid.
# unionfind cd2d9c8 + the relay87 cross-node census patch (unlanded), identical
# in both arms.
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
BINM=$M/FoF3.cpu-prod-v89main
BING=$M/FoF3.cpu-prod-v89gate
WANTM=d6de5990214a9a91f7e66d0709229172
WANTG=7998f8943c3ae2ed4bff00e03bacc26e

{
  echo "job : $SLURM_JOB_ID  relay97  canopy collect gate, 128 NODES, 2B, 1024 procs, 7168 PEs"
  echo "  main arm  $(md5sum $BINM|cut -d' ' -f1)  (want $WANTM)  paratreet2 main               1040b63"
  echo "  gate arm  $(md5sum $BING|cut -d' ' -f1)  (want $WANTG)  paratreet2 canopy-collect-gate f9faa0f"
  echo "  unionfind $(git -C $M/unionfind rev-parse --short HEAD)  dirty=$(git -C $M/unionfind status --porcelain -uno|wc -l) (relay87 census, unlanded, both arms)"
  echo "  nodes $SLURM_NNODES  tasks $SLURM_NTASKS"
  echo "  nodelist $SLURM_JOB_NODELIST"
} | tee "$OUT/provenance.txt"
[ "$(md5sum $BINM|cut -d' ' -f1)" = "$WANTM" ] || { echo "!!! main binary is not the built one"; exit 2; }
[ "$(md5sum $BING|cut -d' ' -f1)" = "$WANTG" ] || { echo "!!! gate binary is not the built one"; exit 2; }
echo

run () {   # run <label> <arm M|G> <cap; "U" = unset>
  local label=$1 ARM=$2 CAP=$3
  local BIN=$BINM; [ "$ARM" = "G" ] && BIN=$BING
  local SARG=""; [ "$CAP" != "U" ] && SARG="-s $CAP"
  local log=$OUT/${label}.log t0 t1 rc c m it lf
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=1024 --gpus-per-node=8 --export=ALL -t 25:00 \
    "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l 32 -i 1 -E 16 -D 3 $SARG \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local pk=$(grep -m1 'FOF3STAT loadcache_pack:' "$log")
  local raw=$(echo "$pk"|grep -o 'raw_canopies [0-9]*'|awk '{print $2}')
  local ded=$(echo "$pk"|grep -o 'deduped [0-9]*'|awk '{print $2}')
  local shp=$(echo "$pk"|grep -o 'shipped [0-9]*'|awk '{print $2}')
  local srt=$(echo "$pk"|grep -o 'sort_s [0-9.]*'|awk '{print $2}')
  local bcast=$(grep -c 'Broadcasting every tree canopy' "$log" || true)
  local wa=$(grep -m1 -o 'phase3_walk [0-9.]*' "$log"|awk '{print $2}')
  local lc=$(grep -m1 -o 'loadCache [0-9.]*' "$log"|awk '{print $2}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF ${lf:-NONE}"
  # ARM-IDENTITY GATE.  This is the real proof of which code ran: the collect
  # gate is inlined at -O and invisible to nm, so the binary gate is advisory
  # and raw_canopies is the one that decides.
  if [ "$CAP" = "U" ]; then
    [ "${bcast:-0}" -eq 1 ] || g="$g !!! unset arm did not print the Broadcasting line"
    [ "${raw:-0}" -gt 50000 ] || g="$g !!! unset arm collected only ${raw:-?} (gate must be a NO-OP when -s is unset)"
  else
    [ "${bcast:-1}" -eq 0 ] || g="$g !!! CAP IGNORED"
    [ "${shp:-0}" = "$CAP" ] || g="$g !!! shipped ${shp:-?} wanted $CAP"
    if [ "$ARM" = "G" ]; then
      [ "${raw:-0}" -lt 5000 ] || g="$g !!! GATE ARM STILL COLLECTED ${raw:-?} -- gate not active"
    else
      [ "${raw:-0}" -gt 50000 ] || g="$g !!! CONTROL ARM collected only ${raw:-?} -- gate leaked into main"
    fi
  fi
  printf "REP %-5s arm=%s cap=%-4s rc=%d %-7s Iter0=%9s walk=%-6s loadCache=%-7s raw=%-6s ded=%-6s shipped=%-4s sort_s=%-7s %4ds %s\n" \
         "$label" "$ARM" "$CAP" "$rc" "$v" "${it:-NONE}" "${wa:-NONE}" "${lc:-NONE}" \
         "${raw:-?}" "${ded:-?}" "${shp:-?}" "${srt:-?}" "$((t1-t0))" "$g"
  grep -m1 'FOF3STAT loadcache_install:' "$log" | sed 's/FOF3STAT //;s/^/      /'
  grep -E 'FOF3STAT cache:' "$log" | sed 's/^/      /'
  grep -m1 'FOF3STAT time_s: uf2_setup' "$log" | sed 's/FOF3STAT //;s/^/      /'
  echo "${it:-0} ${wa:-0} ${lc:-0} ${srt:-0} ${raw:-0}" >> "$OUT/${label%%-*}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -12 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up / cold 76 GB read, DISCARDED"
run W M 128

for r in 1 2 3; do
  echo "################ rep $r  (order alternates so drift cannot favour one arm)"
  if [ $((r % 2)) -eq 1 ]; then
    run M-r$r M 128
    run G-r$r G 128
  else
    run G-r$r G 128
    run M-r$r M 128
  fi
done

echo "################ negative control: -s UNSET, where the gate must do nothing"
run MU-r1 M U
run GU-r1 G U

echo "################ SUMMARY"
for a in M G MU GU; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;w+=$2;l+=$3;s+=$4;r+=$5;n++;
                if(lo==""||$1<lo)lo=$1; if($1>hi)hi=$1;
                if(llo==""||$3<llo)llo=$3; if($3>lhi)lhi=$3}
    END{printf "  %-2s n=%d  Iter0 %9.1f ms [%.1f..%.1f]   walk %6.3f s   loadCache %7.1f ms [%.1f..%.1f]   sort %6.2f ms   raw %.0f\n",
               A,n,i/n,lo,hi,w/n,l/n*1000,llo*1000,lhi*1000,s/n*1000,r/n}' "$OUT/$a.dat"
done
echo
echo "  THE HEADLINE NUMBER: raw_canopies, M vs G.  69,670 -> ~1,170 is the claim."
echo "  Iter0 is secondary and is quoted with its range; ~3% may not separate at this scale."
echo "=== $(date +%T) DONE -- $OUT"
