#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay92-lcscale-64n
#SBATCH --nodes=64
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
# relay92 -- loadCache ANTI-SCALING, the 8 / 16 / 64 node points.
# (128 comes from job 5329432, which runs it alongside the -D arms.)
#
# ONE 64-NODE ALLOCATION COVERS ALL THREE POINTS: srun can use fewer nodes than
# the allocation holds, so 8, 16 and 64 run as three sruns inside it rather
# than three queue waits.  That also holds the machine and the binary fixed
# across the scale points, which three separate jobs would not.
#
# WHAT IS BEING TESTED.  loadCache grows 49x from 8 to 128 nodes -- 16x more
# processes but 49x more time, so the per-process term is itself superlinear in
# P.  Starter-pack size was ruled out.  The untested candidate is the
# per-process install: Driver::loadCache sorts the canopy storage once, then
# broadcasts, and recvStarterPack loops installBoundary over EVERY canopy entry
# on EVERY process.  If the pack grows with P and every process installs all of
# it, that is O(P) per process and O(P^2) globally -- exactly the observed
# shape.
#
# THE PREDICTION, so the run can falsify it: install_mean_s should track P
# roughly linearly and pack_n should track P; ship+barrier (the residual)
# should NOT dominate.  If instead the residual dominates, the mechanism is the
# broadcast or the barrier and the fix is different.
#
# Reported per scale: loadcache_pack (raw/deduped/shipped canopies, sort time,
# pack MB), loadcache_install (per-process install sum/mean/max/min and
# max/mean), plus cached_nodes and placeholders from the new cache line.
#
# NO CROSS-PROCESS TIMESTAMPS -- relay84 measured clock offsets up to 66 ms.
# Only per-process durations are reduced.
#
# 1 rep per scale, -D 3 (the default), CPU arm, exactness-gated.
# Binary FoF3.cpu-prod-v85 7d216a0670c5610bb980dbeed7cb6bf8.
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
  echo "job : $SLURM_JOB_ID  relay92 loadCache scale sweep 8/16/64 nodes, 2B, -D 3"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD) +relay92  unionfind $(git -C $M/unionfind rev-parse --short HEAD) +relay87"
  echo "  bin $(md5sum $BIN|cut -d' ' -f1)   allocation $SLURM_NNODES nodes"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <nodes>
  # TWO statements, deliberately.  `local N=$1 T=$((N*8))` expands the
  # arithmetic BEFORE N is assigned, so under `set -u` it is a fatal unbound
  # variable that exits the shell -- and `|| true` at the call site cannot
  # catch it.  That killed job 5329433 in 6 seconds with no log at all.
  local N=$1
  local T=$((N*8))
  local log=$OUT/n$N.log t0 t1 rc c m it lf
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --nodes=$N --ntasks=$T --gpus-per-node=8 \
    --export=ALL -t 30:00 \
    "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l 32 -i 1 -E 16 -D 3 \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local lc=$(grep -m1 -o 'loadCache [0-9.]*' "$log"|awk '{print $2}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF ${lf:-NONE}"
  printf "REP nodes=%-4s procs=%-5s rc=%d %-7s Iter0=%10s loadCache=%-8s %4ds %s\n" \
         "$N" "$T" "$rc" "$v" "${it:-NONE}" "${lc:-NONE}" "$((t1-t0))" "$g"
  grep -E 'FOF3STAT loadcache_pack|FOF3STAT loadcache_install|FOF3STAT cache:' "$log" | sed 's/^/      /'
  echo "$N $T ${it:-0} ${lc:-0}" >> "$OUT/scale.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -12 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up at 16 nodes, DISCARDED (cold read)"
run 16 || echo "  (warm-up returned non-zero; continuing)"
rm -f "$OUT/scale.dat"
echo "################ scale points"
for N in 8 16 64; do run $N; done

echo "################ SUMMARY  loadCache against process count"
awk '{printf "  nodes %-4s procs %-5s  Iter0 %10.1f ms   loadCache %8.3f s\n",$1,$2,$3,$4}' "$OUT/scale.dat"
echo "  (128-node point is job 5329432; 8->128 is 16x processes and the record says 49x loadCache)"
echo "=== $(date +%T) DONE -- $OUT"
