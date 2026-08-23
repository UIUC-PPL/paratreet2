#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay91-dsweep-128n
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
# relay91 -- -D 2 vs -D 3 AT 128 NODES.  The only item touching the scaling
# frontier rather than a constant factor at 16.
#
# relay90 found -D 2 and -D 3 tied on wall at 16 nodes, with -D 2 moving half
# the bytes (4.4 vs 8.7 GB) and holding 130 MB/process less.  At 16 nodes TIME
# binds and that saving is worth nothing.  At 128 the known blocker is
# loadCache anti-scaling (49x from 8 to 128, pack size ruled out) and the
# constraint may be memory or network instead -- where -D 2 is free.
#
#   D2  -D 2   x2        D3  -D 3   x2    interleaved, exactness-gated
#
# 8x the processes of every run in this campaign since relay16, so treat the
# first arm as a shakedown as well as a warm-up: if the EXACT gate or the
# poller arithmetic fails it will fail there.  ndev 7 / poll 1 at ppn 7 is the
# same combination validated at 16 nodes (every device gets a poller).
# PMI_MAX_KVS_ENTRIES is already sized for 128 x 8 x 7 (the precedent script
# examples/fof3/scripts/run_fof3_scale_128.sbatch).
#
# Binary FoF3.cpu-prod-v84 88b5169143c8e57acfdcc7636fd955bd.
# paratreet2 a65da5a + unionfind cd2d9c8 + the two report-only patches.
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
BIN=$M/FoF3.cpu-prod-v84

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
