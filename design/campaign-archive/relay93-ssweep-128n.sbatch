#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay93-ssweep-128n
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
# relay93 -- THE -s SWEEP AT 128 NODES.  The campaign's highest-value run.
#
# relay92 root-caused the scaling stall: 64->128 nodes buys nothing (0.98x) and
# loadCache alone accounts for it, at 78.6% ship.  The ship is P x pack_n and
# pack_n ~ P, hence P^1.89.  -s caps pack_n, removing one factor of P.
#
# WHAT -s ACTUALLY DOES (paratreet2 55f2efc documents it): storage is sorted
# ascending by Key, canopy keys are prefix-coded, so a cap ships the SHALLOWEST
# N entries -- it truncates the canopy at a depth rather than batching.  The
# omitted canopies are then fetched on demand through the ordinary miss path.
# Kale verified locally that every cap is exact down to -s 1; correctness does
# not depend on the starter pack.
#
#   canopy depth cumulative (branch factor 8): 1, 9, 73, 585, 4681, 37449
#   measured pack at 128 nodes: 34,835 entries = 4.46 MB (~through depth 5)
#
#   arms, 2 reps each, interleaved, exactness-gated
#     U      unset (control)        34,835 entries   4.46 MB
#     8192   ~depth 5 truncated      8,192           1.05 MB
#     2048                           2,048           0.26 MB
#     512    ~depth 3 (585)            512           0.07 MB
#     128    ~depth 2 (73) + change    128           0.02 MB
#
# THE PREDICTION ON RECORD, so it can fail: the residual falls roughly linearly
# in the cap while walk fetches (the new `requests` field) rise, giving an
# INTERIOR OPTIMUM, and 128 nodes becomes faster than 64 (1728.9 ms) for the
# first time.  If requests do NOT rise, the canopy was never being used and the
# story is simpler than we think; if the residual does NOT fall linearly, the
# P x pack_n model is wrong.
#
# Binary FoF3.cpu-prod-v86 f0576ea2b12e727738707cb56ec1319f, built from
# branch relay92-loadcache-profile @ e2f6d91 (2 commits on origin/main 55f2efc),
# clean tree.  Both commits are report-only.
# unionfind cd2d9c8 + the relay87 census patch (report-only, uncommitted).
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
BIN=$M/FoF3.cpu-prod-v86

{
  echo "job : $SLURM_JOB_ID  relay91  -D 2 vs -D 3 at 128 NODES, 2B, 1024 procs, 7168 PEs"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD)  unionfind $(git -C $M/unionfind rev-parse --short HEAD)"
  echo "  bin $(md5sum $BIN|cut -d' ' -f1) (want f0576ea2b12e727738707cb56ec1319f)"
  echo "  paratreet2 branch $(git -C $M/paratreet2 rev-parse --abbrev-ref HEAD) @ $(git -C $M/paratreet2 rev-parse --short HEAD)"
  echo "  nodes $SLURM_NNODES  tasks $SLURM_NTASKS"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <cap; "U" = unset>
  local label=$1 CAP=$2
  local SARG=""
  [ "$CAP" != "U" ] && SARG="-s $CAP"
  local log=$OUT/${label}.log t0 t1 rc c m it lf dd
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
  echo "${it:-0} ${wa:-0}" >> "$OUT/${label}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -12 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up / shakedown, DISCARDED"
run W U
for r in 1 2; do
  echo "################ rep $r"
  for C in U 8192 2048 512 128; do run "c$C" "$C"; done
done

echo "################ SUMMARY"
for a in cU c8192 c2048 c512 c128; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;w+=$2;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
    END{printf "  %-2s n=%d  Iter0 %10.1f ms  [%.1f..%.1f]   walk %6.3f s\n",A,n,i/n,lo,hi,w/n}' "$OUT/$a.dat"
done
echo "  targets: 64-node Iter0 was 1728.9 ms; uncapped 128-node is ~1736-1947 ms (relay92)"
echo "=== $(date +%T) DONE -- $OUT"
