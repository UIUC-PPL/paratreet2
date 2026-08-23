#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay85-rootcensus-16n
#SBATCH --nodes=16
#SBATCH --time=00:40:00
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
# relay85 -- WHERE DO THE ROOTS OF CROSS-PROCESS COMPONENTS LIVE?
#
# Kale, 2026-08-22: "I want to know the distribution of roots of components
# that straddle processes, by PEs.  Since union-find makes parent pointers go
# to smaller numbers, maybe they will all concentrate based on the numbering
# used (but I see activity in the tail happening on PEs like 350, 840, 854)."
#
# THE ENCODING MAKES HALF OF THIS AN EXACT PREDICTION, NOT A GUESS.
# FoFPhase1.h:124  uf2EncodeTip(process, dense_index)
#                    = (process << kUF2IdxBits) | dense_index
# The process index is in the HIGH bits, and find_boss2 refuses to point at a
# larger id (min-heap), so the root of a component is the MINIMUM id in it and
# therefore always sits on the LOWEST-NUMBERED PROCESS that owns any of its
# vertices.  What is NOT predictable is the shape: if components straddle only
# neighbouring processes in the SFC ordering, every chare except the last can
# root straddling components and the distribution is broad; if they straddle
# far, the mass piles onto the low chares.  That is what this measures.
#
# Note the distinction that probably explains Kale's observation: the tail
# ACTIVITY he sees on PEs 350 / 840 / 854 (chares 50 / 120 / 122) is climb and
# label traffic, which happens wherever the vertices are.  Root OWNERSHIP is a
# different quantity and is what is measured here.
#
# METHOD (report-only, FOF_ROOT_CENSUS=1, after labeling so every vertex has
# its label): each element dedupes the component labels it holds and maps each
# to its owning chare -- the label IS the root's vertexID.  A label owned by
# another chare means this chare holds vertices of a component ROOTED
# ELSEWHERE, i.e. that component straddles.  Summing those incidences over all
# chares gives, per chare c, how much cross-process structure is rooted at c.
#
#   A  CPU, census on    x2      B  GPU, census on    x2
#
# The GPU arm is included because FOF_GPU_PHASE1 forces peSets=1, so its
# straddling set is genuinely cross-process rather than manufactured by the
# PE-set split -- the two shapes should differ and the difference is the point.
#
# FOF_UF_SIZES=0 everywhere (the recommended config).  Wave parked and gated.
# Binaries FoF3.cpu-prod-v81 3dd519ab5a257a2de2a2392c1d4292fd
#          FoF3.gpu-prod-v81 48e8ddcff19b8eff1b8237edd9781277
# Working-tree patch on 724782d / cd2d9c8, NOT pushed:
#   patches/relay85-unionfind.diff + relay85-paratreet2.diff
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
CH=$M/charm-prod/reconverse-linux-x86_64-amd
export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:/opt/rocm-6.2.4/lib:$BASELIB
CPU=$M/FoF3.cpu-prod-v81; GPU=$M/FoF3.gpu-prod-v81

{
  echo "job : $SLURM_JOB_ID  relay83 sharding measurement + GPU reps, 16 nodes, 2B"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD) (want 724782d)  unionfind $(git -C $M/unionfind rev-parse --short HEAD) (want cd2d9c8)"
  echo "  dirty: pt2=$(git -C $M/paratreet2 status --porcelain -uno|grep -v ' utility$'|wc -l) uf=$(git -C $M/unionfind status --porcelain -uno|wc -l)"
  echo "  cpu $(md5sum $CPU|cut -d' ' -f1)  gpu $(md5sum $GPU|cut -d' ' -f1)"
  echo "  nodes $SLURM_NODELIST"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <bin> <gpu> <sizes> <localcomp> <leaf> <ndev> <poll>
  local label=$1 bin=$2 gpu=$3 sz=$4 lc=$5 leaf=$6 ndev=$7 poll=$8
  local log=$OUT/${label}.log t0 t1 rc c m it lf
  if [ "$gpu" = 1 ]; then export PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1
  else unset PARATREET_DEVICE_TREE FOF_GPU_PHASE1; fi
  export FOF_UF_SIZES=$sz FOF_ROOT_CENSUS=1
  if [ "$lc" = 1 ]; then unset FOF_UF_LOCALCOMP; else export FOF_UF_LOCALCOMP=0; fi
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=128 --gpus-per-node=8 --export=ALL -t 15:00 \
    "$WRAP" "$bin" -f "$INPUT" -d oct -u dist -c stats -l "$leaf" -i 1 -E 16 \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices "$ndev" +backend_poll_thread "$poll" > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local wa=$(grep -m1 -o 'phase3_walk [0-9.]*' "$log"|awk '{print $2}')
  local uf=$(grep -m1 -o ' uf2 [0-9.]*' "$log"|awk '{print $2}')
  local ask=$(grep -m1 'addsize_SKIPPED' "$log"|awk '{print $NF}')
  local asr=$(grep -m1 'addsize_root' "$log"|awk '{print $NF}')
  local hist=$(grep -m1 'climb_local_hops_log2' "$log")
  local rcn=$(grep -c '^\[ROOTCENSUS\]' "$log" || true)
  local nw=$(grep -c 'compression wave' "$log" || true)
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "$leaf" ] && g="!!! LEAF ${lf:-NONE}"
  [ "${nw:-0}" -eq 0 ] || g="$g !!! WAVE FIRED -- parked"
  [ "${ask:-0}" -gt 0 ] || g="$g !!! sizes knob did not take"
  [ "${asr:-1}" -eq 0 ] || g="$g !!! add_size still ran"
  [ -n "$hist" ] || g="$g !!! HOP HISTOGRAM MISSING"
  [ "${rcn:-0}" -gt 3 ] || g="$g !!! ROOT CENSUS MISSING (lines=${rcn:-0})"
  printf "REP %-9s rc=%d %-7s leaf=%-4s Iter0=%9s  walk=%-7s uf2=%-7s %3ds %s\n" \
         "$label" "$rc" "$v" "${lf:-NONE}" "${it:-NONE}" "${wa:-NONE}" "${uf:-NONE}" "$((t1-t0))" "$g"
  grep '^\[UFSTAT\]' "$log" | grep -E 'hops_log2|fb1_root|fb2_flip|local_union' | sed 's/^/      /'
  grep '^\[ROOTCENSUS\]' "$log" | sed 's/^/      /'
  # local_union against TOTAL SUBMISSIONS.  Distributed chains = fb1_root, and
  # a flip re-issues union_request so it starts an EXTRA chain; distinct
  # distributed edges = fb1_root - fb2_flip.  Submissions = those + local_union.
  awk '/^\[UFSTAT\] fb1_root/{r=$NF} /^\[UFSTAT\] fb2_flip/{f=$NF} /^\[UFSTAT\] local_union/{l=$NF}
       END{ if(r&&l){ d=r-f; t=d+l;
         printf "      SUBMISSIONS  local_union %d (%.1f%%)  distributed %d (%.1f%%)  total %d\n", l,100*l/t,d,100*d/t,t } }' "$log"
  echo "${it:-0} ${wa:-0} ${uf:-0}" >> "$OUT/${label}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -6 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up, DISCARDED"
run W       $CPU 0 0 1 32  7 1
for r in 1 2; do
  echo "################ rep $r"
  run cpu       $CPU 0 0 1 32  7 1
  run gpu       $GPU 1 0 1 128 4 2
done

echo "################ SUMMARY"
for a in cpu gpu; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;w+=$2;u+=$3;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
    END{printf "  %-9s n=%d  Iter0 %8.1f ms  [%.1f..%.1f]  walk %6.3f s  uf2 %6.3f s\n",A,n,i/n,lo,hi,w/n,u/n}' "$OUT/$a.dat"
done
echo
echo "  relay80 GPU reference: szON 2630.9  szOFF 2562.2 [2547.9..2576.5]  -2.61%"
echo "  relay82 CPU reference: szON 4577.7  szOFF 4467.2 [4450.6..4491.4]  -2.41%"
echo "=== $(date +%T) DONE -- $OUT"
