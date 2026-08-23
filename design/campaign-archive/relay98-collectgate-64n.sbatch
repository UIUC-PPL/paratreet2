#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay98-collectgate-64n
#SBATCH --nodes=64
#SBATCH --ntasks=512
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
# relay98 -- THE COLLECT GATE AT 64 NODES, where capping was a WASH.
#
# relay97 (128 nodes, job 5332618) measured canopy-collect-gate f9faa0f:
#   raw_canopies 69,670 -> 1,170   sort_s 9.73 -> 0.10 ms
#   loadCache 15.7 -> 6.0 ms       Iter0 1619.4 -> 1484.6 (-8.3%), 3/3 paired
# The wall moved ~135 ms while loadCache's own timer moved only ~10 ms, so most
# of the gain is OUTSIDE the phase timer -- recvTC arrives during the canopy
# upward pass, before loadCache is entered, and its lateness on the DRIVER PE
# delays the ship and everything after it.
#
# WHY 64 NODES IS THE INTERESTING SCALE.  relay96 found capping is a wash here:
#   U 1764.8   2048 1723.6 [1670.9..1771.7] (OVERLAPS U)   128 1824.2 WORST
# i.e. at 64 nodes the walk penalty of a cap roughly cancels the loadCache it
# removes.  The collect gate adds a SECOND, INDEPENDENT saving on the same
# knob -- one that costs the walk nothing, because the SHIP IS UNCHANGED.  If
# it is worth ~130 ms here too, the crossover in the relay96 model moves and
# 64 nodes turns from a tie into a capping win.  THAT IS THE QUESTION.
#
# The canopy at 64 nodes is 35,388 raw / 17,694 deduped (relay96, measured).
# Whole-levels limits, branch factor 8:
#   -s 128  -> want 897   -> limit 4096  -> depths 0..3 =   585 x2 =  1,170 raw
#   -s 2048 -> want 14337 -> limit 32768 -> depths 0..4 = 4,681 x2 =  9,362 raw
# So the gate should cut the collect 30x at -s 128 and 3.8x at -s 2048.
#
# ARMS (paired and interleaved, exactness-gated)
#   M128 / G128    main vs gate at -s 128    3 reps each  (relay96's WORST cap)
#   M2048 / G2048  main vs gate at -s 2048   2 reps each  (relay96's BEST cap)
#   MU / GU        -s UNSET                  1 rep each   (gate must be a no-op)
#
# NOISE FLOOR, measured not assumed: relay97's MU vs GU are identical code
# paths and differed by 68.8 ms at 128 nodes.  relay96 saw a 64-node rep spread
# as wide as [1670.9..1771.7].  Single-rep deltas below ~70 ms mean NOTHING
# here; only the paired multi-rep means are quoted as results.
#
# Binaries and refs are relay97's, unchanged:
#   FoF3.cpu-prod-v89main  paratreet2 main               1040b63
#   FoF3.cpu-prod-v89gate  paratreet2 canopy-collect-gate f9faa0f
# Only src/TreeCanopy.h differs.  unionfind cd2d9c8 + the relay87 cross-node
# census patch (unlanded), identical in both arms.
# Absolute walls are NOT comparable to relay96: main moved 52decf6 -> 9798a06
# ("avoid unnecessary reads for fof") -> 1040b63, and both arms were rebuilt.
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
  echo "job : $SLURM_JOB_ID  relay98  canopy collect gate, 64 NODES, 2B, 512 procs, 3584 PEs"
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
    --distribution=block:block --ntasks=512 --gpus-per-node=8 --export=ALL -t 25:00 \
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
    [ "${raw:-0}" = 35388 ] || g="$g !!! unset arm collected ${raw:-?} (gate must be a NO-OP when -s is unset)"
  else
    [ "${bcast:-1}" -eq 0 ] || g="$g !!! CAP IGNORED"
    [ "${shp:-0}" = "$CAP" ] || g="$g !!! shipped ${shp:-?} wanted $CAP"
    # The expected collect depends on the cap (whole levels), so the gate is
    # keyed to it rather than to one hard-coded number.
    local want_raw=0
    [ "$CAP" = "128"  ] && want_raw=1170
    [ "$CAP" = "2048" ] && want_raw=9362
    if [ "$ARM" = "G" ]; then
      [ "${raw:-0}" = "$want_raw" ] || g="$g !!! GATE ARM collected ${raw:-?}, whole-levels predicts $want_raw"
    else
      [ "${raw:-0}" = 35388 ] || g="$g !!! CONTROL ARM collected ${raw:-?}, want the full 35388"
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
  echo "################ rep $r at -s 128  (order alternates so drift cannot favour one arm)"
  if [ $((r % 2)) -eq 1 ]; then
    run M128-r$r M 128; run G128-r$r G 128
  else
    run G128-r$r G 128; run M128-r$r M 128
  fi
done

for r in 1 2; do
  echo "################ rep $r at -s 2048  (relay96's best cap at this scale)"
  if [ $((r % 2)) -eq 1 ]; then
    run M2048-r$r M 2048; run G2048-r$r G 2048
  else
    run G2048-r$r G 2048; run M2048-r$r M 2048
  fi
done

echo "################ negative control: -s UNSET, where the gate must do nothing"
run MU-r1 M U
run GU-r1 G U

echo "################ SUMMARY"
for a in M128 G128 M2048 G2048 MU GU; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;w+=$2;l+=$3;s+=$4;r+=$5;n++;
                if(lo==""||$1<lo)lo=$1; if($1>hi)hi=$1;
                if(llo==""||$3<llo)llo=$3; if($3>lhi)lhi=$3}
    END{printf "  %-5s n=%d  Iter0 %9.1f ms [%.1f..%.1f]   walk %6.3f s   loadCache %7.1f ms [%.1f..%.1f]   sort %6.2f ms   raw %.0f\n",
               A,n,i/n,lo,hi,w/n,l/n*1000,llo*1000,lhi*1000,s/n*1000,r/n}' "$OUT/$a.dat"
done
echo
echo
echo "  relay96 reference at 64 nodes, DIFFERENT BUILD, for shape only not for subtraction:"
echo "    U 1764.8   2048 1723.6 [1670.9..1771.7]   512 1761.0   128 1824.2   32 1745.1"
echo "  THE QUESTION: does G128 beat MU?  If it does, the collect gate turns 64"
echo "  nodes from a capping WASH into a capping WIN, and the relay96 crossover moves."
echo "  Noise floor here is ~70 ms (relay97 MU-vs-GU, identical code paths)."
echo "=== $(date +%T) DONE -- $OUT"
