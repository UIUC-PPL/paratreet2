#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay75-traces-16n
#SBATCH --nodes=16
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
# relay75 -- TRACES OF THE CURRENT STACK.  Projections + sumDetail.
#
# TWO PURPOSES: Kale wants to see the current state directly, and the wave's
# target needs settling -- relay74 found FOF_WAVE=1 costs +50.1 ms rather
# than recovering the 150-300 ms of post-walk near-idle it was aimed at.
#
# KALE'S CORRECTION, 2026-08-22, and it changes what to measure.  I wrote in
# relay74 that "the whole uf2 row is 40.5 ms", implying that is what the
# union-find costs.  It is not.  FoFPhase3.h:
#     t_walk   = t1 - t0     the walk
#     t_gather = t2 - t1     phase3Stats only
#     t_uf2    = t3 - t2     initUF2 + fireUF2Edges + CkWaitQD + find_components
# The walk streams edges in batches of 16 AS IT GOES, so the union-find work
# those edges trigger runs CONCURRENTLY with the walk and is charged to
# phase3_walk.  t_uf2 is the POST-WALK RESIDUE only.  Union-find must be
# measured by ENTRY METHOD across the whole run, which is what these traces
# are for.
#
# BINARIES: paratreet2 e2a877e + unionfind 2ad91a8 against charm-prodtr
# (--with-production --enable-tracing: Release AND traceable).
#   FoF3.cpu-prodtr2  md5 8028c7d0f2d8e774d70cf4ce3bcf9511
#   FoF3.gpu-prodtr2  md5 2a278db21fd570b25ab644bb784eee28
# Linked tracing costs a few percent (relay65: GPU inside the production
# spread, CPU +2.7-3.2%), so these walls are NOT production numbers.
#
# ARMS:
#   W  warm-up, traces DELETED -- the first arm pays the cold 76 GB read and
#      tracing it would bake that into a trace read as representative
#   1  cpu-off    CPU best, wave off      the current CPU state
#   2  cpu-wave1  CPU best, FOF_WAVE=1    what the wave actually does
#   3  gpu-off    GPU best, -l 128        the current GPU state
#
# +logsize 20000000 so there is no mid-run buffer flush; +sumDetail for the
# 1 ms .sumd files; traceroot created before the srun.  Each arm gated on
# EXACT, on the printed leaf, and on a complete 896-PE trace set.
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
WRAP=$G/scripts/gcd_wrapper.sh
PEMAP7=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
CHT=$M/charm-prodtr/reconverse-linux-x86_64-amd
export LD_LIBRARY_PATH=$M/lci-install/lib64:$CHT/lib:/opt/rocm-6.2.4/lib:$BASELIB
CPU=$M/FoF3.cpu-prodtr2; GPU=$M/FoF3.gpu-prodtr2

{
  echo "job : $SLURM_JOB_ID  relay75 traces of the current stack, 16 nodes, 2B, 896 PEs"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD)  unionfind $(git -C $M/unionfind rev-parse --short HEAD)"
  for B in $CPU $GPU; do
    echo "  $(basename $B) md5 $(md5sum $B|cut -d' ' -f1)  TraceProj $(nm -C $B|grep -ci TraceProjections)  TraceSum $(nm -C $B|grep -ci TraceSummary)  fofgpu $(nm -C $B|grep -c 'fofgpu::')"
  done
  echo "  charm-prodtr Release+TRACING; nodes $SLURM_NODELIST"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <bin> <extra> <gpu> <wave> <leafwant> <keep>
  local label=$1 bin=$2 extra=$3 gpu=$4 wave=$5 leafwant=$6 keep=$7
  local log=$OUT/${label}.log
  local tr=$OUT/traces/${label}
  local t0 t1 rc c m it lf
  mkdir -p "$tr" || { echo "  $label: cannot create $tr"; return; }
  if [ "$gpu" = 1 ]; then export PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1
  else unset PARATREET_DEVICE_TREE FOF_GPU_PHASE1; fi
  if [ "$wave" = 0 ]; then unset FOF_WAVE; else export FOF_WAVE=$wave; fi
  echo "=== $(date +%T) $label  gpu=$gpu wave=$wave args='$extra'"
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=128 --gpus-per-node=8 --export=ALL -t 15:00 \
    "$WRAP" "$bin" -f "$INPUT" -d oct -u dist -c stats -i 1 $extra \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices ${8:-7} +backend_poll_thread ${9:-1} \
    +traceroot "$tr" +logsize 20000000 +sumDetail > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "$leafwant" ] && g="!!! LEAF GATE ${lf:-NONE}"
  local nlog=$(ls "$tr" 2>/dev/null | grep -c '\.log\.gz$')
  local nsum=$(ls "$tr" 2>/dev/null | grep -c '\.sum$')
  local nsd=$(ls "$tr" 2>/dev/null | grep -c '\.sumd$')
  printf "REP %-10s rc=%d %-7s leaf=%-4s Iter0=%9s ms  %3ds  %s\n" "$label" "$rc" "$v" "${lf:-NONE}" "${it:-NONE}" "$((t1-t0))" "$g"
  grep -m1 'FOF3STAT time_s: uf2_setup' "$log" | sed 's/FOF3STAT //;s/^/      /'
  grep -m1 'Tree traversal:' "$log" | sed 's/^/      /'
  grep -m1 'compression wave' "$log" | sed 's/^/      /'
  echo "      TRACES log.gz $nlog sum $nsum sumd $nsd (want 896/896/896)  $(du -sh "$tr" 2>/dev/null|cut -f1)"
  [ "$nlog" = 896 ] && [ "$nsd" = 896 ] || echo "      !!! TRACE SET INCOMPLETE"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -6 "$log"|sed 's/^/      | /'; }
  if [ "$keep" = 0 ]; then rm -rf "$tr"; echo "      warm-up traces DELETED"; fi
  echo
}

echo "################ warm-up, traces DELETED"
run W        $CPU ""       0 0 32  0 7 1
echo "################ 1 -- CPU best, wave OFF (the current CPU state)"
run cpu-off  $CPU ""       0 0 32  1 7 1
echo "################ 2 -- CPU best, FOF_WAVE=1"
run cpu-wave1 $CPU ""      0 1 32  1 7 1
echo "################ 3 -- GPU best, -l 128 (the current GPU state)"
run gpu-off  $GPU "-l 128" 1 0 128 1 4 2
echo "=== $(date +%T) DONE -- $OUT"
du -sh "$OUT"/traces/* 2>/dev/null
