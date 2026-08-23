#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay92-lc128n
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
# relay92 -- 128-NODE COMPANION to job 5329100: the two things that job
# cannot report, on the same scale point.
#
# 5329100 was built from v84, which predates paratreet2 73923d7.  It gives the
# -D 2 vs -D 3 walls and the loadCache row, but NOT the placeholders count --
# that field did not exist when it was built.  Rather than cancel a 128-node
# allocation that has been accumulating priority, this runs alongside it with
# v85 and adds:
#   - placeholders (used_nodes - cached_nodes), per -D arm.  The frontier
#     population relay91 s3 had to INFER from the interior/leaf split.
#   - the loadCache decomposition (FOF3STAT loadcache_pack / loadcache_install),
#     which is the 128-node point of the anti-scaling sweep.
#
# So between the two jobs the gating result is complete, and the expensive
# scale point of item 2 comes free with it.
#
#   D2  -D 2   x2      D3  -D 3   x2     interleaved, exactness-gated
#
# THE loadCache DECOMPOSITION, and what it is testing.  Driver::loadCache is
# sortStorage (pack build, driver-only) then a nodegroup broadcast whose
# recvStarterPack loops installBoundary over EVERY canopy entry on EVERY
# process.  If that install is the anti-scaling term it is O(P) per process and
# O(P^2) globally, which is the shape Kale predicted from 16x processes giving
# 49x time.  install_max/mean also shows whether it is skewed or uniform.
#   NO CROSS-PROCESS TIMESTAMPS: relay84 measured clock offsets up to 66 ms,
#   which would swamp the ship term.  Only per-process DURATIONS are reduced;
#   ship+barrier is the residual against the driver-side total.
#
# Binary FoF3.cpu-prod-v85 7d216a0670c5610bb980dbeed7cb6bf8,
# paratreet2 73923d7 + relay92 patch, unionfind cd2d9c8 + relay87 patch.
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
BIN=$M/FoF3.cpu-prod-v85

{
  echo "job : $SLURM_JOB_ID  relay91  -D 2 vs -D 3 at 128 NODES, 2B, 1024 procs, 7168 PEs"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD)  unionfind $(git -C $M/unionfind rev-parse --short HEAD)"
  echo "  bin $(md5sum $BIN|cut -d' ' -f1)"
  echo "  nodes $SLURM_NNODES  tasks $SLURM_NTASKS"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <D>
  local label=$1 D=$2
  local log=$OUT/${label}.log t0 t1 rc c m it lf dd
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=1024 --gpus-per-node=8 --export=ALL -t 25:00 \
    "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l 32 -i 1 -E 16 -D "$D" \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  dd=$(grep -m1 -o 'Cache share depth (-D): [0-9]*' "$log"|awk '{print $NF}')
  local wa=$(grep -m1 -o 'phase3_walk [0-9.]*' "$log"|awk '{print $2}')
  local lc=$(grep -m1 -o 'loadCache [0-9.]*' "$log"|awk '{print $2}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF ${lf:-NONE}"
  [ "${dd:-N}" = "$D" ] || g="$g !!! -D printed ${dd:-NONE} wanted $D"
  printf "REP %-5s D=%s rc=%d %-7s Iter0=%10s walk=%-7s loadCache=%-7s %4ds %s\n" \
         "$label" "${dd:-?}" "$rc" "$v" "${it:-NONE}" "${wa:-NONE}" "${lc:-NONE}" "$((t1-t0))" "$g"
  grep -E 'FOF3STAT cache:|FOF3STAT memory_MB' "$log" | sed 's/^/      /'
  grep -m1 'FOF3STAT time_s: uf2_setup' "$log" | sed 's/FOF3STAT //;s/^/      /'
  echo "${it:-0} ${wa:-0}" >> "$OUT/${label}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -12 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up / shakedown, DISCARDED"
run W 3
for r in 1 2; do
  echo "################ rep $r"
  run D2 2
  run D3 3
done

echo "################ SUMMARY"
for a in D2 D3; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;w+=$2;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
    END{printf "  %-2s n=%d  Iter0 %10.1f ms  [%.1f..%.1f]   walk %6.3f s\n",A,n,i/n,lo,hi,w/n}' "$OUT/$a.dat"
done
echo "  16-node reference (relay90): D2 4525.7 [4494.8..4556.7]  D3 4553.1 [4528.9..4577.3], TIED"
echo "=== $(date +%T) DONE -- $OUT"
