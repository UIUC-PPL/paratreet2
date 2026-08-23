#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay96-ssweep-scale
#SBATCH --nodes=64
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
# relay96 -- THE DISCRIMINATING -s SWEEP AT 16 AND 64 NODES.
#   Dataset selected by $DS (2B or 80M); submit once for each.
#   ONE 64-NODE ALLOCATION COVERS BOTH SCALES: srun may use fewer nodes than
#   the allocation holds, so 16 and 64 run inside it rather than as two queue
#   waits -- and the machine and binary stay fixed across the scale points,
#   which two separate jobs could not guarantee.
#
# THE QUESTION.  relay93/94 found s* ~ 128 at 128 nodes (P=1024).  Two models:
#   FAN-IN   C_ship = alpha.P.s,  C_lazy = beta.P/s  -> both carry P, P CANCELS,
#            s* = sqrt(beta/alpha), SAME 128 at every scale
#   CHAIN    C_lazy ~ L.log_b(N/s) -> s* moves as 1/P, i.e. ~1024 at 16 nodes
#            (P=128) and ~256 at 64 nodes (P=512)
# THE SAME ABSOLUTE CAP SET IS USED AT BOTH SCALES so the comparison is direct:
#   {2048, 512, 128, 32} plus the unset control.
#   16 nodes: FAN-IN predicts 128 best; CHAIN predicts 512 or 2048 best.
#   64 nodes: FAN-IN predicts 128 best; CHAIN predicts ~256, i.e. 512 or 128.
# The 16-node point is the discriminating one -- the two predictions are 8x
# apart there and only 2x apart at 64.
#
# WHY IT IS LOAD-BEARING NOW (Kale): if s* really is P-independent, a fixed
# DEPTH cut on the collect makes the collected canopy count b^d, independent of
# P, so item (1) and item (3) compose into an O(1) collect. If s* moves with P
# that does not hold and the depth gate needs to be P-dependent.
#
# 3 reps per arm, interleaved rep-major.  canopy_fills reported with every wall.
#
# EXACTNESS.  For 2B the campaign gold is known (424897832 / 185317566).  For
# 80M IT IS NOT, so the gate is SELF-CONSISTENCY: the warm-up arm's literal
# FOF3STAT components line becomes the reference and every later arm must match
# it BITWISE.  That is the stronger gate anyway -- it catches a cap changing
# the answer even if the absolute values were never known.
#
# Binary FoF3.cpu-prod-v88 0ac0c6e141f541f4da186cc7e75a73fa, pinned to
# paratreet2 52decf6 (clean), unionfind cd2d9c8 + relay87 census patch.
# -l 32, -E 16, -D 3, FOF_UF_SIZES=0, -u dist, ppn 7.
set -u
M=$HOME/software/merged
G=$HOME/software/gpu-stalls
DS="${DS:-2B}"
if [ "$DS" = "2B" ]; then
  INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
  GOLD=424897832; GOLDMAX=185317566
else
  INPUT=/lustre/orion/csc710/proj-shared/lambb.00500
  GOLD=""; GOLDMAX=""      # unknown: self-consistency gate instead
fi
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
BIN=$M/FoF3.cpu-prod-v88
REFLINE=""

{
  echo "job : $SLURM_JOB_ID  relay96 -s sweep at 16 and 64 nodes, dataset $DS"
  echo "  input $INPUT"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD) (want 52decf6)  dirty=$(git -C $M/paratreet2 status --porcelain -uno|grep -v ' utility$'|wc -l)"
  echo "  bin $(md5sum $BIN|cut -d' ' -f1) (want 0ac0c6e141f541f4da186cc7e75a73fa)"
  echo "  allocation $SLURM_NNODES nodes"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <nodes> <cap; U=unset> <tag>
  local N=$1 CAP=$2 TAG=$3 T=$((N*8))
  local SARG=""
  [ "$CAP" != "U" ] && SARG="-s $CAP"
  local log=$OUT/${TAG}.log t0 t1 rc it lf
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --nodes=$N --ntasks=$T --gpus-per-node=8 \
    --export=ALL -t 25:00 \
    "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l 32 -i 1 -E 16 -D 3 $SARG \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  local comp=$(grep -m1 '^FOF3STAT components:' "$log")
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local wa=$(grep -m1 -o 'phase3_walk [0-9.]*' "$log"|awk '{print $2}')
  local lc=$(grep -m1 -o 'loadCache [0-9.]*' "$log"|awk '{print $2}')
  local shipped=$(grep -m1 -o 'shipped [0-9]*' "$log"|awk '{print $2}')
  local packn=$(grep -m1 -o 'deduped [0-9]*' "$log"|awk '{print $2}')
  local cf=$(grep -m1 -o 'canopy_fills [0-9]*' "$log"|awk '{print $2}')
  local bcast=$(grep -c 'Broadcasting every tree canopy' "$log" || true)
  # exactness: gold when known, otherwise bitwise self-consistency
  local v=DIFFERS
  if [ -n "$GOLD" ]; then
    local c=$(echo "$comp"|grep -o 'components: [0-9]*'|awk '{print $2}')
    local m=$(echo "$comp"|grep -o 'max_size [0-9]*'|awk '{print $2}')
    [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  else
    if [ -z "$REFLINE" ]; then REFLINE="$comp"; v=REF
    elif [ "$comp" = "$REFLINE" ]; then v=MATCH; fi
  fi
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF ${lf:-NONE}"
  if [ "$CAP" = "U" ]; then [ "${bcast:-0}" -eq 1 ] || g="$g !!! control lost the Broadcasting line"
  else [ "${bcast:-1}" -eq 0 ] || g="$g !!! CAP IGNORED"; [ "${shipped:-0}" = "$CAP" ] || g="$g !!! shipped ${shipped:-?} wanted $CAP"; fi
  printf "REP %-14s n=%-3s cap=%-5s rc=%d %-7s Iter0=%9s walk=%-7s loadCache=%-8s pack=%-6s shipped=%-6s fills=%-9s %3ds %s\n" \
         "$TAG" "$N" "$CAP" "$rc" "$v" "${it:-NONE}" "${wa:-NONE}" "${lc:-NONE}" "${packn:-?}" "${shipped:-?}" "${cf:-?}" "$((t1-t0))" "$g"
  echo "${it:-0} ${wa:-0} ${lc:-0} ${cf:-0}" >> "$OUT/${TAG%%_r*}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -8 "$log"|sed 's/^/      | /'; }
}

for N in 16 64; do
  echo "################ $DS, $N nodes -- warm-up DISCARDED"
  run $N U "warm_n$N"; rm -f "$OUT/warm_n$N.dat"
  for r in 1 2 3; do
    echo "################ $DS, $N nodes, rep $r"
    for C in U 2048 512 128 32; do run $N "$C" "n${N}_c${C}_r${r}"; done
  done
  echo "################ SUMMARY  $DS at $N nodes"
  for C in U 2048 512 128 32; do
    f="$OUT/n${N}_c${C}.dat"; [ -f "$f" ] || continue
    awk -v C=$C -v N=$N '{i+=$1;w+=$2;l+=$3;c+=$4;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
      END{printf "  %s nodes cap %-5s n=%d  Iter0 %9.1f [%.1f..%.1f]  walk %6.3f  loadCache %6.4f  fills %10.0f\n",N,C,n,i/n,lo,hi,w/n,l/n,c/n}' "$f"
  done
  echo
done
echo "  128-node reference (relay93/94): pack 34,835; best cap 128 at 1499.5 ms"
echo "  FAN-IN model predicts cap 128 best at BOTH scales; CHAIN predicts 2048ish at 16 and 512ish at 64"
echo "=== $(date +%T) DONE -- $OUT"
