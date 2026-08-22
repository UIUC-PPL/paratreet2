#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay82-cpureps-16n
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
# relay82 -- HOW BIG IS THE CPU FOF_UF_SIZES EFFECT, REALLY?  3 reps each.
#
# relay79 (pre-landing patch)  on 4629.9  off 4541.0   -88.9 ms  -1.92%
# relay80 (landed, 1 rep each) on 4494.3  off 4464.8   -29.5 ms  -0.66%
#
# Both agree on SIGN and on the mechanism -- the gain is in the walk bracket
# (relay79 -79 ms, relay80 -60 ms) -- but they disagree on how much of it
# reaches the wall (relay79 -89 ms, relay80 -29 ms).  relay80 had ONE rep per
# arm, which carries no spread information at all: my summary line printed
# "SEPARATED" for two single points, which is vacuous and is fixed below.
# relay80's whole baseline also ran 135 ms faster than relay79's (4494 vs
# 4630), i.e. this is partly a between-job shift, not only an arm effect.
#
# So: 3 reps each, interleaved rep-major, one allocation, same binary.
# EXACTNESS IS ALREADY SETTLED by relay80 -- every arm EXACT, including the
# landed local_union gating.  This job is only about magnitude.
#
# Binary FoF3.cpu-prod-v79 6ccf74a0fb65408063cd667efb178a06, clean trees at
# paratreet2 7263ff1 / unionfind db73766.  Wave parked and gated off.
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
CPU=$M/FoF3.cpu-prod-v79; GPU=$M/FoF3.gpu-prod-v79

{
  echo "job : $SLURM_JOB_ID  relay80 FOF_UF_SIZES clean-tree CPU rep + GPU arm, 16 nodes, 2B"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD) (want 7263ff1)  dirty=$(git -C $M/paratreet2 status --porcelain -uno | grep -v ' utility$' | wc -l)"
  echo "  unionfind  $(git -C $M/unionfind  rev-parse --short HEAD) (want db73766)  dirty=$(git -C $M/unionfind status --porcelain -uno | wc -l)"
  echo "  cpu $(md5sum $CPU|cut -d' ' -f1)  gpu $(md5sum $GPU|cut -d' ' -f1)"
  echo "  nodes $SLURM_NODELIST"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <bin> <gpu> <sizes> <leaf> <ndev> <poll>
  local label=$1 bin=$2 gpu=$3 sz=$4 leaf=$5 ndev=$6 poll=$7
  local log=$OUT/${label}.log t0 t1 rc c m it lf
  if [ "$gpu" = 1 ]; then export PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1
  else unset PARATREET_DEVICE_TREE FOF_GPU_PHASE1; fi
  export FOF_UF_SIZES=$sz
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
  local asr=$(grep -m1 'addsize_root' "$log"|awk '{print $NF}')
  local ask=$(grep -m1 'addsize_SKIPPED' "$log"|awk '{print $NF}')
  local nw=$(grep -c 'compression wave' "$log" || true)
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "$leaf" ] && g="!!! LEAF ${lf:-NONE} wanted $leaf"
  [ "${nw:-0}" -eq 0 ] || g="$g !!! WAVE FIRED ($nw) -- it is parked"
  if [ "$sz" = 0 ]; then
    [ "${ask:-0}" -gt 0 ] || g="$g !!! sizes knob did not take (skipped=${ask:-0})"
    [ "${asr:-1}" -eq 0 ] || g="$g !!! add_size still ran (root=$asr)"
  else
    [ "${asr:-0}" -gt 0 ] || g="$g !!! add_size did not run with knob on"
    [ "${ask:-1}" -eq 0 ] || g="$g !!! skipped>0 with knob on"
  fi
  printf "REP %-10s rc=%d %-7s leaf=%-4s Iter0=%9s  walk=%-7s uf2=%-7s %3ds %s\n" \
         "$label" "$rc" "$v" "${lf:-NONE}" "${it:-NONE}" "${wa:-NONE}" "${uf:-NONE}" "$((t1-t0))" "$g"
  grep -E 'addsize_root|addsize_SKIPPED|fb2_UNION' "$log" | sed 's/^/      /'
  echo "${it:-0} ${wa:-0} ${uf:-0}" >> "$OUT/${label}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -6 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up, DISCARDED (cold 76 GB read)"
run W         $CPU 0 1 32 7 1
for r in 1 2 3; do
  echo "################ rep $r"
  run cpu-szON  $CPU 0 1 32 7 1
  run cpu-szOFF $CPU 0 0 32 7 1
done

echo "################ SUMMARY"
for a in cpu-szON cpu-szOFF; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;w+=$2;u+=$3;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
    END{printf "  %-10s n=%d  Iter0 %8.1f ms  [%.1f..%.1f]  walk %6.3f s  uf2 %6.3f s\n",A,n,i/n,lo,hi,w/n,u/n}' "$OUT/$a.dat"
done
echo
python3 - "$OUT" <<'PY'
import sys,os
O=sys.argv[1]
def M(a):
    p=os.path.join(O,a+'.dat')
    if not os.path.exists(p): return None
    r=[list(map(float,l.split())) for l in open(p) if l.strip()]
    return (sum(x[0] for x in r)/len(r), min(x[0] for x in r), max(x[0] for x in r), len(r))
for on,off,tag in (('cpu-szON','cpu-szOFF','CPU'),):
    a,b=M(on),M(off)
    if not a or not b: continue
    # n=1 carries NO spread information; two single points never "overlap".
    if a[3]<2 or b[3]<2: sep="n<2, NO SPREAD INFORMATION"
    elif b[2]<a[1] or b[1]>a[2]: sep="SEPARATED"
    else: sep="OVERLAPS"
    print("  %s  sizes off - on = %+8.1f ms (%+.2f%%)   %s   [on %.1f..%.1f n=%d | off %.1f..%.1f n=%d]"
          %(tag,b[0]-a[0],100*(b[0]-a[0])/a[0],sep,a[1],a[2],a[3],b[1],b[2],b[3]))
print("\n  relay79 (pre-landing patch): on 4629.9 [4625.2..4634.7]  off 4541.0 [4538.2..4543.7]  -1.92%")
print("  relay80 (landed, n=1 each):  on 4494.3  off 4464.8  -0.66%")
PY
echo "=== $(date +%T) DONE -- $OUT"
