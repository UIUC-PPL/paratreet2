#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay87-pesets-16n
#SBATCH --nodes=16
#SBATCH --time=00:50:00
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
# relay87 -- DOES THE PE-SET SPLIT CARRY THE WALK IMBALANCE?
#   plus the held CROSS-NODE EDGE CENSUS, folded in as Kale asked.
#
# THE CONFOUND THIS REMOVES.  Kale's local comparison found the GPU arm more
# imbalanced in walk EPs than the CPU arm (5.93x vs 3.98x max/mean) despite
# having no same-process pairs -- but GPU runs leaf 128 and CPU leaf 32, so the
# comparison could not settle anything.  Arm A holds EVERYTHING fixed and turns
# only the split off.
#
#   A  FOF_PE_SETS=1     split OFF: no local-local pairs deferred to the walk
#   B  defaults          AUTO split, sets = ppn = 7
#   2 untraced reps each for the walls, then ONE TRACED rep of each for the
#   per-PE work distribution.
#
# THE DECISION IT FEEDS: if A is LESS imbalanced than B, the local-local pairs
# carry the skew and Kale's within-process walk phase targets it correctly.  If
# A ~ B or worse, the imbalance is in cross-process pairs, that phase cannot
# reach it, and the m2-weighted static tie-break becomes the priority.
#
# FOLDED IN -- THE CROSS-NODE CENSUS (held from 19:30).  New [UFSTAT] rows
# edge_SAME_PROC / edge_INTRA_NODE / edge_INTER_NODE classify every SUBMITTED
# edge by its endpoints' owning processes.  relay86 s5 showed the root-pair
# collapse buys only 1.6%, so a hierarchical gather can only be justified by
# retiring edges internal to a coarser group; this is that number.  Counted in
# union_requests, not union_request, because a flip recurses and would double
# count.  Divisor from FOF_PROCS_PER_PNODE=8, valid only because this job
# launches --ntasks-per-node=8 --distribution=block:block.
#   BONUS: arm A has no same-process pairs by construction, so edge_SAME_PROC
#   must come out ~0 there and ~739k in B (relay86).  That is a free gate on
#   the census itself.
#
# CPU arm throughout: ppn 7, -l 32, -E 16, FOF_UF_SIZES=0, -u dist.
# Binaries FoF3.cpu-prod-v83   7fc5795f7c3d9288a498b9877603fd98
#          FoF3.cpu-prodtr-v83 73e929bc36e2bf7cec8029ed86980a3c
# paratreet2 a65da5a (7573ce6 is docs-only) + unionfind cd2d9c8 + census patch.
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
unset FOF_WAVE FOF_WAVE_MS FOF_UF_SHORTCIRCUIT FOF_ROOT_CENSUS
unset PARATREET_DEVICE_TREE FOF_GPU_PHASE1
export FOF_UF_SIZES=0 FOF_PROCS_PER_PNODE=8
WRAP=$G/scripts/gcd_wrapper.sh
PEMAP7=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
PROD=$M/FoF3.cpu-prod-v83; PTR=$M/FoF3.cpu-prodtr-v83
CHP=$M/charm-prod/reconverse-linux-x86_64-amd
CHT=$M/charm-prodtr/reconverse-linux-x86_64-amd

{
  echo "job : $SLURM_JOB_ID  relay87 PE-set split vs walk imbalance + cross-node census"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD)  unionfind $(git -C $M/unionfind rev-parse --short HEAD) +census"
  echo "  prod $(md5sum $PROD|cut -d' ' -f1)   prodtr $(md5sum $PTR|cut -d' ' -f1)"
  echo "  nodes $SLURM_NODELIST"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <pesets 0=default|1> <traced 0|1>
  local label=$1 ps=$2 tr=$3
  local log=$OUT/${label}.log bin=$PROD extra="" CH=$CHP
  if [ "$tr" = 1 ]; then bin=$PTR; CH=$CHT; mkdir -p "$OUT/traces/$label"
    extra="+traceroot $OUT/traces/$label +logsize 20000000 +sumDetail"; fi
  export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:/opt/rocm-6.2.4/lib:$BASELIB
  if [ "$ps" = 1 ]; then export FOF_PE_SETS=1; else unset FOF_PE_SETS; fi
  local t0=$(date +%s) rc c m it lf
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=128 --gpus-per-node=8 --export=ALL -t 20:00 \
    "$WRAP" "$bin" -f "$INPUT" -d oct -u dist -c stats -l 32 -i 1 -E 16 \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 $extra > "$log" 2>&1
  rc=$?; local t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local sets=$(grep -m1 -io 'PE-set split[^,;]*' "$log" | head -1)
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF ${lf:-NONE}"
  local sp=$(grep -m1 'edge_SAME_PROC' "$log"|awk '{print $NF}')
  if [ "$ps" = 1 ]; then [ "${sp:-1}" -lt 1000 ] || g="$g !!! split OFF but edge_SAME_PROC=$sp"
                    else [ "${sp:-0}" -gt 100000 ] || g="$g !!! split ON but edge_SAME_PROC=$sp"; fi
  printf "REP %-8s pesets=%-7s rc=%d %-7s Iter0=%9s  %4ds %s\n" \
         "$label" "$([ "$ps" = 1 ] && echo OFF || echo AUTO)" "$rc" "$v" "${it:-NONE}" "$((t1-t0))" "$g"
  grep 'FOF3STAT time_s: uf2_setup' "$log" | sed 's/FOF3STAT //;s/^/      /'
  grep -E 'edge_SAME_PROC|edge_INTRA_NODE|edge_INTER_NODE' "$log" | sed 's/^/      /'
  if [ "$tr" = 1 ]; then
    local n=$(ls "$OUT/traces/$label" 2>/dev/null | grep -c '\.log\.gz$')
    echo "      TRACES log.gz $n (want 896)  $(du -sh $OUT/traces/$label 2>/dev/null|cut -f1)"
    [ "$n" = 896 ] || echo "      !!! TRACE SET INCOMPLETE"
  fi
  echo "${it:-0}" >> "$OUT/${label}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -8 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up, DISCARDED"
run W 0 0
for r in 1 2; do
  echo "################ rep $r"
  run A 1 0
  run B 0 0
done
echo "################ traced pair, 1 rep each"
run A-tr 1 1
run B-tr 0 1

echo "################ SUMMARY"
for a in A B; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
    END{printf "  %-2s n=%d  Iter0 %9.1f ms  [%.1f..%.1f]\n",A,n,i/n,lo,hi}' "$OUT/$a.dat"
done
echo "  A = FOF_PE_SETS=1 (split OFF)    B = defaults (AUTO, sets=7)"
echo "=== $(date +%T) DONE -- $OUT"
