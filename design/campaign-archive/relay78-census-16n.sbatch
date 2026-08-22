#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay78-census-16n
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
# relay78 -- WHAT IS ACTUALLY HAPPENING IN THE 313 ms DRAIN, and a first test
# of Kale's backward short-circuit.
#
# KALE'S QUESTION (2026-08-22): are the findBoss entry methods leading to more
# calls to them on other processors?  To parent-pointer changes (unions)?  Or
# being discarded because both find chains reached the same root?
#
# THE TRACE CANNOT ANSWER IT.  The branch taken inside find_boss2 is invisible
# to Projections -- and my first attempt to infer it from message counts was
# wrong: over a whole run sends equal receives, so executions-minus-creations
# measures window skew, not terminations.  So this run counts the branches
# directly.  Census is snapshotted at the fireUF2Edges barrier, so
# walk-concurrent work and the post-walk drain are reported separately.
#
# KALE'S REMEDY, ALSO TESTED.  "If a findBoss request comes to me from another
# PE, and I am about to leave doing another findBoss for the same chain, send
# a backward message asking my sender to point to where I am going."  That
# mechanism ALREADY EXISTS in unionFindLib.C and is commented out at both call
# sites; senderID is already carried in every findBossData.  Here it is
# enabled behind FOF_UF_SHORTCIRCUIT=1.
#   The race Kale named -- do not act on the backward message if the parent
#   already moved -- is handled by a MONOTONE guard rather than an epoch:
#   parent ids strictly decrease toward the root (find_boss2 refuses to point
#   at a larger id), so accepting only a strictly SMALLER id can never move a
#   pointer away from the root. Stale answers are counted and dropped.
#
#   A  census only, short-circuit OFF     x2
#   B  census + FOF_UF_SHORTCIRCUIT=1     x2
#
# Binary FoF3.cpu-prod-cen md5 1775a0218343a85ee2213abb720ceb05, a WORKING-TREE
# patch on 57f2395 / d72ee66, not pushed:
#   patches/relay79-unionfind.diff + relay79-paratreet2.diff (cumulative; see patches/APPLY.md)
# The census adds counter increments in hot paths; arm A against relay77's A
# (4634.1 ms) measures what that costs.  CPU best: ppn 7, -l 32, -E 16.
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
BIN=$M/FoF3.cpu-prod-cen

{
  echo "job : $SLURM_JOB_ID  relay78 union-find branch census, 16 nodes, 2B, 896 PEs"
  echo "  bin $BIN md5 $(md5sum $BIN|cut -d' ' -f1)  (want 1775a0218343a85ee2213abb720ceb05)"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD) +patch  unionfind $(git -C $M/unionfind rev-parse --short HEAD) +patch"
  echo "  UFSTAT $(strings $BIN|grep -c UFSTAT)  FOF_UF_SHORTCIRCUIT $(strings $BIN|grep -c FOF_UF_SHORTCIRCUIT)"
  echo "  nodes $SLURM_NODELIST"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <shortcircuit>
  local label=$1 sc=$2
  local log=$OUT/${label}.log
  local t0 t1 rc c m it lf
  if [ "$sc" = 0 ]; then unset FOF_UF_SHORTCIRCUIT; else export FOF_UF_SHORTCIRCUIT=$sc; fi
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
  local scs=$(grep -m1 'sc_sent' "$log"|awk '{print $NF}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF ${lf:-NONE}"
  [ "${eb:-N}" != "16" ] && g="$g !!! -E printed ${eb:-NONE}"
  [ "${ncen:-0}" -ge 15 ] || g="$g !!! CENSUS MISSING (lines=${ncen:-0})"
  if [ "$sc" != 0 ]; then [ "${scs:-0}" -gt 0 ] || g="$g !!! SHORTCIRCUIT DID NOT FIRE"; \
                     else [ "${scs:-0}" -eq 0 ] || g="$g !!! sc fired with knob off"; fi
  printf "REP %-3s rc=%d %-7s Iter0=%9s  walk=%-7s uf2=%-7s %3ds %s\n" \
         "$label" "$rc" "$v" "${it:-NONE}" "${wa:-NONE}" "${uf:-NONE}" "$((t1-t0))" "$g"
  grep '^\[UFSTAT\]' "$log" | sed 's/^/      /'
  echo "${it:-0} ${wa:-0} ${uf:-0}" >> "$OUT/${label}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -6 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up, DISCARDED"
run W 0
echo "################ rep 1"
run A 0; run B 1
echo "################ rep 2"
run A 0; run B 1

echo "################ SUMMARY"
for a in A B; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;w+=$2;u+=$3;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
    END{printf "  %-2s n=%d  Iter0 %8.1f ms  [%.1f..%.1f]  walk %6.3f s  uf2 %6.3f s\n",A,n,i/n,lo,hi,w/n,u/n}' "$OUT/$a.dat"
done
echo "  relay77 arm A, no census, same config: 4634.1 ms [4626.6..4641.5]"
echo "=== $(date +%T) DONE -- $OUT"
