#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay109-nvss-perphase-128n
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
# relay109 -- WHERE DOES THE HELPER DAMAGE ACTUALLY LAND?  N vs S, PER PHASE.
#
# N = affinity fix OFF (FOF_NO_AFFINITY_FIX=1), ppn 7.  ROCm helper threads
#     inherit the firing PE's single-CPU pin and preempt it.  The damage.
# S = affinity fix ON in its default SMT-sibling form, ppn 7.  Nothing else
#     differs between the two arms -- same binary, same cap, same ppn, same
#     pemap, same allocation.
#
# WHY THIS RERUN EXISTS: A BUG IN MY OWN SCRIPT.  relay108 passed "r$r" as the
# label for all four arms of a rep, so all four wrote to the SAME $OUT/r$r.log
# and only the last arm of each rep survived.  The .dat files were keyed by
# arm, which is why relay108's summary is intact -- but the per-phase
# FOF3STAT time_s lines exist for exactly one N rep and two B6 reps, and for
# NO S rep at all.  The comparison Kale ranked first was therefore impossible
# from the data on disk.  THE LABEL NOW CARRIES THE ARM.
#
# THE QUESTION.  relay108 measured N 2471.3 ms against S 914.1 ms -- the fix
# is worth roughly 2.7x at 128 nodes (quoted that way, not to three digits:
# n=3-4, one job, and relay98 concluded this campaign cannot resolve walls
# better than about 4%).  The walk worsens 0.426 -> 0.725 s, but Iteration 0
# worsens by ~1557 ms, so THE WALK IS ONLY ABOUT A FIFTH OF THE DAMAGE.  This
# run finds the other four fifths in data the binary already prints.
#
# WHAT TO READ.  Every FOF3STAT time_s line is captured per arm per rep:
#   phase1, phase1_device, phase1_devinit, tip_encode, upwardPass, loadCache,
#   tip_sentinel, uf2_setup, phase3_walk, edge_gather, uf2, relabel,
#   component_histogram
# A UNIFORM tax across every phase means the helper is stealing PE time
# throughout, and the walk is only prominent because it is the longest phase.
# A CONCENTRATED one names the phase that actually suffers.
# The unreviewed N-vs-B6 pair that survived relay108 hinted at the former --
# uf2 0.030 -> 0.328, edge_gather 0.005 -> 0.104 against the walk's
# 0.373 -> 0.531, i.e. SHORT phases hit hardest in relative terms -- but that
# pair confounds the fix with ppn 6 vs 7 and is NOT evidence.  This run is.
#
# 3 reps each, interleaved, order alternating.  -s 128 (relay106's best on
# GPU), -l 128, GPU codepath, NO --core-spec (relay105).
set -u
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
       FI_MR_CACHE_MONITOR=userfaultfd PMI_MAX_KVS_ENTRIES=16777216
unset LCI_OFI_THREADING_HINT FOF_HIST_DENSE
unset FOF_WAVE FOF_WAVE_MS FOF_UF_SHORTCIRCUIT FOF_ROOT_CENSUS FOF_PE_SETS
export FOF_UF_SIZES=0
export PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1
WRAP=$G/scripts/gcd_wrapper.sh
# RESUBMIT FIX (job 5335749 died in 1 s on every arm): I wrote this script
# fresh and dropped CH and the LD_LIBRARY_PATH export that every other relay
# script carries, so the GPU binary could not find libamdhip64.so.6 and all
# 13 sruns exited 127.  BASELIB was computed and never used -- that was the
# tell.  The preflight gate below now proves the binary RESOLVES before a
# single node-second is spent.
CH=$M/charm-prod/reconverse-linux-x86_64-amd
export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:/opt/rocm-6.2.4/lib:$BASELIB
PEMAP7=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
PEMAP6=1-6,9-14,17-22,25-30,33-38,41-46,49-54,57-62
HELPER6=7,15,23,31,39,47,55,63          # the 7th core of each octet, PE-free at ppn 6
BIN=$M/FoF3.gpu-prod-v91

{
  echo "job : $SLURM_JOB_ID  relay108  helper placement, 128 NODES, 2B, GPU, -s 128"
  echo "  bin $(md5sum $BIN|cut -d' ' -f1)  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD)"
  echo "  NO --core-spec: the OS core stays reserved, per Kale 2026-08-24"
  echo "  cpuset per process: 7 physical cores + their SMT siblings (cpus-per-task=14)"
  echo "  nodes $SLURM_NNODES  tasks $SLURM_NTASKS  started $(date '+%H:%M:%S')"
} | tee "$OUT/provenance.txt"

# PREFLIGHT: a missing shared library is a 1-second failure repeated 13 times.
# Catch it here, on the batch node, before any srun.
MISSING=$(ldd "$BIN" 2>/dev/null | grep -c 'not found' || true)
echo "  preflight: ldd unresolved = $MISSING (want 0)"
if [ "$MISSING" != 0 ]; then
  echo "  !!! the binary does not resolve its shared libraries; aborting before any srun"
  ldd "$BIN" 2>&1 | grep 'not found' | sed 's/^/     /'
  exit 3
fi
echo

run () {   # run <label> <arm N|S|D6|B6>
  local label=$1 ARM=$2
  local PPN=7 PEMAP=$PEMAP7
  unset FOF_NO_AFFINITY_FIX FOF_HELPER_CPUS
  case "$ARM" in
    N)  export FOF_NO_AFFINITY_FIX=1 ;;
    S)  : ;;
    D6) PPN=6; PEMAP=$PEMAP6; export FOF_HELPER_CPUS=$HELPER6 ;;
    B6) PPN=6; PEMAP=$PEMAP6 ;;
  esac
  local log=$OUT/${label}.log t0 t1 rc it
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=1024 --gpus-per-node=8 --export=ALL -t 30:00 \
    "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l 128 -i 1 -E 16 -D 3 -s 128 \
    +ppn "$PPN" +pemap "$PEMAP" +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  local c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  local m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  local wa=$(grep -m1 -o 'phase3_walk [0-9.]*' "$log"|awk '{print $2}')
  local p1=$(grep -m1 -o 'phase1 [0-9.]*' "$log"|awk '{print $2}')
  local lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local pes=$(grep -m1 -o '[0-9]* PEs' "$log"|head -1|awk '{print $1}')
  # THE PLACEMENT GATE: the binary says what it did.  Read it, do not assume.
  local fixline=$(grep -m1 -o 'affinity fix active ([^)]*)' "$log")
  local declined=$(grep -c 'affinity fix DECLINED' "$log" || true)
  local place="${fixline:-none}"
  [ "$declined" -gt 0 ] && place="DECLINED"
  [ "$ARM" = "N" ] && [ "$place" = "none" ] && place="off (as intended)"
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" = "128" ] || g="!!! LEAF ${lf:-NONE}"
  [ "$rc" = 0 ] || g="!!! rc=$rc"
  case "$ARM" in
    S|B6) echo "$place" | grep -q 'SMT siblings' || g="$g !!! wanted SMT siblings, got: $place" ;;
    D6)   echo "$place" | grep -q 'FOF_HELPER_CPUS' || g="$g !!! wanted the named core, got: $place" ;;
    N)    [ "$declined" -eq 0 ] || g="$g !!! nofix arm printed DECLINED" ;;
  esac
  printf "REP %-6s arm=%-3s ppn=%d rc=%d %-7s  walk %-7s  Iter0 %9s ms  p1 %-7s  PEs %-6s wall %4ds  %s\n" \
         "$label" "$ARM" "$PPN" "$rc" "$v" "${wa:-NONE}" "${it:-NONE}" "${p1:-NONE}" "${pes:-?}" "$((t1-t0))" "$g"
  printf "      placement: %s\n" "$place"
  # THE POINT OF THIS RUN: every phase line, kept per arm per rep.
  grep 'FOF3STAT time_s:' "$log" | sed 's/FOF3STAT //;s/^/      /'
  grep 'FOF3STAT time_s:' "$log" | sed "s|^|$ARM |" >> "$OUT/phases-$ARM.txt"
  echo "${it:-0} ${wa:-0}" >> "$OUT/${ARM}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; grep -v '^srun' "$log" | tail -8 | sed 's/^/      | /'; }
  echo
}

echo "################ warm-up / cold 76 GB read, DISCARDED"
run W-warm S

for r in 1 2 3; do
  echo "################ rep $r  (order alternates so drift cannot favour one arm)"
  case $((r % 2)) in
    1) run "N-r$r" N; run "S-r$r" S ;;
    0) run "S-r$r" S; run "N-r$r" N ;;
  esac
done

echo "################ SUMMARY   walk is the number Kale asked for"
for a in N S; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;w+=$2;n++;if(wlo==""||$2<wlo)wlo=$2;if($2>whi)whi=$2;
                if(ilo==""||$1<ilo)ilo=$1;if($1>ihi)ihi=$1}
    END{printf "  %-3s n=%d  walk %6.3f s [%.3f..%.3f]   Iter0 %8.1f ms [%.1f..%.1f]\n",
               A,n,w/n,wlo,whi,i/n,ilo,ihi}' "$OUT/$a.dat"
done
echo
echo "  N vs S   = what the affinity fix is worth at 128 nodes.  Never measured"
echo "             here before; the campaign's -22.8% is a 16-NODE number."
echo "  D6 vs B6 = what the DEDICATED CORE is worth, with the ppn 6 PE loss"
echo "             held constant.  This is the comparison that tests Kale's idea."
echo "  D6 vs S  = whether giving up 1 PE in 7 is worth buying a whole core."
echo "  relay106 reference, same config at ppn 7 with SMT siblings and -s 128:"
echo "    Iter0 896.7 ms [894.8..898.5]  walk 0.399 s"
echo "=== $(date +%T) DONE -- $OUT"
