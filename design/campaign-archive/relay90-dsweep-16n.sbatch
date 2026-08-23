#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay90-dsweep-16n
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
# relay90 -- THE CACHE SHARE DEPTH SWEEP (-D), the main event.
#
# relay89: 76.5% of fetched nodes are never examined by any certificate, and
# replies bundle 15.5 nodes each, so the waste is PREFETCH OVERSHOOT.  -D is
# the knob that sets the bundle.  The trade is explicit and BOTH SIDES are now
# quantified:
#   smaller D  fewer wasted bytes, but MORE requests -- and requests cost
#              18.2% of walk wall at 14.9 us each (relay89 s4), which is MORE
#              PE time than processing the replies (14.0%)
#   larger D   the reverse
# So an interior optimum is plausible, and if D=2 or D=1 wins it is the
# "fetch fewer distinct nodes" lever relay88 said was the only one left.
#
# THREE BINARIES, one sweep:
#   P  FoF3.cpu-prod-v84    88b5169143c8e57acfdcc7636fd955bd   walls, 2 reps/D
#   T  FoF3.cpu-touch-v84   03fc8f246ba2d604cc1c2fe0c04ba908   touch split, 1/D
#   R  FoF3.cpu-prodtr-v84  f4b36f6c5162b363f77dcd1f70969b53   traces, 1/D
# The touch arm runs at EVERY D rather than only the best and worst, because
# best/worst is not known until the walls are in and a second job to fill it
# would cost more than the four runs do.
#
# Tracing costs +2.7-3.2% and the touch build +1.9%, so ONLY the P arms are
# comparable walls.  Message counts come from the R traces, the touch split
# from T, and FOF3STAT cache from all three.
#
# paratreet2 a65da5a + unionfind cd2d9c8 + relay87 census + relay89 touch
# patches (both report-only, NOT pushed).  CPU arm: ppn 7, -l 32, -E 16,
# FOF_UF_SIZES=0, -u dist.
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
CHP=$M/charm-prod/reconverse-linux-x86_64-amd
CHT=$M/charm-prodtr/reconverse-linux-x86_64-amd
P=$M/FoF3.cpu-prod-v84; T=$M/FoF3.cpu-touch-v84; R=$M/FoF3.cpu-prodtr-v84

{
  echo "job : $SLURM_JOB_ID  relay90 cache-share-depth sweep, 16 nodes, 2B"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD)  unionfind $(git -C $M/unionfind rev-parse --short HEAD)"
  for b in $P $T $R; do echo "  $(basename $b) $(md5sum $b|cut -d' ' -f1)"; done
  echo "  nodes $SLURM_NODELIST"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <bin> <D> <traced 0|1> <want_touch 0|1>
  local label=$1 bin=$2 D=$3 tr=$4 wt=$5
  local log=$OUT/${label}.log extra="" CH=$CHP
  if [ "$tr" = 1 ]; then CH=$CHT; mkdir -p "$OUT/traces/$label"
    extra="+traceroot $OUT/traces/$label +logsize 20000000 +sumDetail"; fi
  export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:/opt/rocm-6.2.4/lib:$BASELIB
  local t0=$(date +%s) rc c m it lf dd
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=128 --gpus-per-node=8 --export=ALL -t 25:00 \
    "$WRAP" "$bin" -f "$INPUT" -d oct -u dist -c stats -l 32 -i 1 -E 16 -D "$D" \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 $extra > "$log" 2>&1
  rc=$?; local t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  dd=$(grep -m1 -o 'Cache share depth (-D): [0-9]*' "$log"|awk '{print $NF}')
  local wa=$(grep -m1 -o 'phase3_walk [0-9.]*' "$log"|awk '{print $2}')
  local nt=$(grep -c 'FOF3STAT cache_touch:' "$log" || true)
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF ${lf:-NONE}"
  [ "${dd:-N}" = "$D" ] || g="$g !!! -D GATE printed ${dd:-NONE} wanted $D"
  if [ "$wt" = 1 ]; then [ "${nt:-0}" -gt 0 ] || g="$g !!! touch line missing"
                    else [ "${nt:-0}" -eq 0 ] || g="$g !!! touch line present"; fi
  printf "REP %-10s D=%s rc=%d %-7s Iter0=%9s walk=%-7s %4ds %s\n" \
         "$label" "${dd:-?}" "$rc" "$v" "${it:-NONE}" "${wa:-NONE}" "$((t1-t0))" "$g"
  grep -E 'FOF3STAT cache:|FOF3STAT cache_touch' "$log" | sed 's/^/      /'
  if [ "$tr" = 1 ]; then
    local n=$(ls "$OUT/traces/$label" 2>/dev/null | grep -c '\.log\.gz$')
    echo "      TRACES log.gz $n (want 896)"; [ "$n" = 896 ] || echo "      !!! INCOMPLETE"
  fi
  echo "${it:-0} ${wa:-0}" >> "$OUT/${label}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -8 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up, DISCARDED"
run W $P 3 0 0
for r in 1 2; do
  echo "################ walls, rep $r"
  for D in 1 2 3 4; do run P$D $P $D 0 0; done
done
echo "################ touch census, 1 rep per D"
for D in 1 2 3 4; do run T$D $T $D 0 1; done
echo "################ traced, 1 rep per D (message counts)"
for D in 1 2 3 4; do run R$D $R $D 1 0; done

echo "################ SUMMARY -- walls (P arms only; T and R carry overhead)"
for D in 1 2 3 4; do
  [ -f "$OUT/P$D.dat" ] || continue
  awk -v D=$D '{i+=$1;w+=$2;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
    END{printf "  -D %s  n=%d  Iter0 %9.1f ms  [%.1f..%.1f]   walk %6.3f s\n",D,n,i/n,lo,hi,w/n}' "$OUT/P$D.dat"
done
echo "  reference: -D 3 is today's default; relay82 -u dist sizes-off 4467.2 [4450.6..4491.4]"
echo "=== $(date +%T) DONE -- $OUT"
