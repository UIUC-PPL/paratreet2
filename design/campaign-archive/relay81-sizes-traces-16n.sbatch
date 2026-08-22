#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay81-sizes-traces-16n
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
# relay81 -- TRACED PAIRS FOR FOF_UF_SIZES, BOTH ARMS ON BOTH DEVICES.
#
# THE QUESTION relay79 left open: removing add_size bought -1.92% and -15.8%
# on the walk bracket, but add_size is a SIDE FLOW off the find dependence
# chain.  So did the 265 ms drain itself shrink, or only the queuing around it?
# The wall cannot tell them apart; entry-method attribution can
# (scripts/relay77-waveattrib.py, and relay78-groups.py for the labeling split).
#
#   cpu-szON / cpu-szOFF   the pair relay79 measured, now traced
#   gpu-szON / gpu-szOFF   the same pair on the GPU arm
#
# Four traced arms plus a discarded warm-up.  Expect roughly 2.5 GB of traces.
#
# BINARIES, both against merged/charm-prodtr (Release + tracing):
#   FoF3.cpu-prodtr-v79  22777303bf63b966fb99ecf4e9d8a429
#   FoF3.gpu-prodtr-v79  4062b58854e1ee240d41fd3111f2202b
# from CLEAN trees at paratreet2 7263ff1 / unionfind db73766.
# Linked tracing costs CPU +2.7-3.2% (relay65) -- these walls are NOT
# production numbers and must not be quoted as them.
#
# FOF_WAVE and FOF_WAVE_MS unset everywhere: the wave is PARKED (relay77).
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
unset FOF_WAVE FOF_WAVE_MS FOF_UF_SHORTCIRCUIT
WRAP=$G/scripts/gcd_wrapper.sh
PEMAP7=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
CHT=$M/charm-prodtr/reconverse-linux-x86_64-amd
export LD_LIBRARY_PATH=$M/lci-install/lib64:$CHT/lib:/opt/rocm-6.2.4/lib:$BASELIB
CPU=$M/FoF3.cpu-prodtr-v79; GPU=$M/FoF3.gpu-prodtr-v79

{
  echo "job : $SLURM_JOB_ID  relay81 FOF_UF_SIZES traced pairs, CPU and GPU, 16 nodes, 2B"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD) (want 7263ff1)  unionfind $(git -C $M/unionfind rev-parse --short HEAD) (want db73766)"
  for B in $CPU $GPU; do
    echo "  $(basename $B) $(md5sum $B|cut -d' ' -f1)  TraceProj $(nm -C $B|grep -ci TraceProjections)  fofgpu $(nm -C $B|grep -c 'fofgpu::')"
  done
  echo "  nodes $SLURM_NODELIST"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <bin> <gpu> <sizes> <leaf> <ndev> <poll> <keep>
  local label=$1 bin=$2 gpu=$3 sz=$4 leaf=$5 ndev=$6 poll=$7 keep=$8
  local log=$OUT/${label}.log tr=$OUT/traces/${label} t0 t1 rc c m it lf
  mkdir -p "$tr" || { echo "  $label: cannot create $tr"; return; }
  if [ "$gpu" = 1 ]; then export PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1
  else unset PARATREET_DEVICE_TREE FOF_GPU_PHASE1; fi
  export FOF_UF_SIZES=$sz
  echo "=== $(date +%T) $label  gpu=$gpu sizes=$sz leaf=$leaf"
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=128 --gpus-per-node=8 --export=ALL -t 20:00 \
    "$WRAP" "$bin" -f "$INPUT" -d oct -u dist -c stats -l "$leaf" -i 1 -E 16 \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices "$ndev" +backend_poll_thread "$poll" \
    +traceroot "$tr" +logsize 20000000 +sumDetail > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local asr=$(grep -m1 'addsize_root' "$log"|awk '{print $NF}')
  local ask=$(grep -m1 'addsize_SKIPPED' "$log"|awk '{print $NF}')
  local nw=$(grep -c 'compression wave' "$log" || true)
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "$leaf" ] && g="!!! LEAF ${lf:-NONE}"
  [ "${nw:-0}" -eq 0 ] || g="$g !!! WAVE FIRED -- it is parked"
  if [ "$sz" = 0 ]; then
    [ "${ask:-0}" -gt 0 ] || g="$g !!! sizes knob did not take"
    [ "${asr:-1}" -eq 0 ] || g="$g !!! add_size still ran"
  else
    [ "${asr:-0}" -gt 0 ] || g="$g !!! add_size did not run"
    [ "${ask:-1}" -eq 0 ] || g="$g !!! skipped>0 with knob on"
  fi
  local nlog=$(ls "$tr" 2>/dev/null | grep -c '\.log\.gz$')
  local nsd=$(ls "$tr" 2>/dev/null | grep -c '\.sumd$')
  printf "REP %-10s rc=%d %-7s leaf=%-4s Iter0=%9s ms  %3ds  %s\n" "$label" "$rc" "$v" "${lf:-NONE}" "${it:-NONE}" "$((t1-t0))" "$g"
  grep -m1 'FOF3STAT time_s: uf2_setup' "$log" | sed 's/FOF3STAT //;s/^/      /'
  grep -E 'addsize_root|addsize_SKIPPED|fb2_UNION' "$log" | sed 's/^/      /'
  echo "      TRACES log.gz $nlog sumd $nsd (want 896/896)  $(du -sh "$tr" 2>/dev/null|cut -f1)"
  [ "$nlog" = 896 ] && [ "$nsd" = 896 ] || echo "      !!! TRACE SET INCOMPLETE"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -6 "$log"|sed 's/^/      | /'; }
  if [ "$keep" = 0 ]; then rm -rf "$tr"; echo "      warm-up traces DELETED"; fi
  echo
}

echo "################ warm-up, traces DELETED"
run W         $CPU 0 1 32  7 1 0
echo "################ CPU pair"
run cpu-szON  $CPU 0 1 32  7 1 1
run cpu-szOFF $CPU 0 0 32  7 1 1
echo "################ GPU pair"
run gpu-szON  $GPU 1 1 128 4 2 1
run gpu-szOFF $GPU 1 0 128 4 2 1
echo "=== $(date +%T) DONE -- $OUT"
du -sh "$OUT"/traces/* 2>/dev/null
