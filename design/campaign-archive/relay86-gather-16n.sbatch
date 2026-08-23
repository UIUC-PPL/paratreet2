#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay86-gather-16n
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
# relay86 -- CONTRACT-THEN-GATHER (-u gather) against -u dist and -u serial.
#   paratreet2 main a65da5a (design/staged-gather.md) + unionfind cd2d9c8
#   CLEAN trees.  FoF3.cpu-prod-v82 d122cdd346117dd3bd4c335debbad70b
#
# THE QUESTIONS, in Kale's order:
#   1. Does contraction repair serial's +28%?  B vs C isolates exactly the
#      same-process edge inflation the PE-set split manufactures -- predicted
#      ~790k local / ~455k cross at 2B (relay78 s11, relay83 s2).
#   2. Does C approach or beat A?  Gather removes the drain's round-trips but
#      forfeits streaming overlap (relay76 priced that at ~104 ms) and pays two
#      serial stages.
#   Either way C's contracted-vs-cross ratio measures what the root-pair
#   collapse buys, which feeds the hierarchical variant.
#
#   A  -u dist     x2      the shipping mode
#   B  -u serial   x2      gather-to-one, every raw edge
#   C  -u gather   x2      contract per process, then gather
#
# *** THE EXACTNESS GATE IS ALSO AN ALGEBRA GATE.  All three modes must print
#     a BITWISE IDENTICAL FOF3STAT components line.  That is the contraction
#     argument in design/staged-gather.md -- min of a component = min over its
#     local-subcomponent minima -- tested at 2B rather than at 1m.  A mismatch
#     between B and C is a contraction bug; a mismatch of either against A
#     would be far worse.  The script compares the literal lines, not just the
#     two gold numbers. ***
#
# CPU best throughout: ppn 7, -l 32, -E 16, FOF_UF_SIZES=0.  Wave parked.
# NOTE serial at 2B with the PE-set split is expected SLOW (+28%, Main.C:180),
# hence the 50 minute wall.
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
export FOF_UF_SIZES=0
WRAP=$G/scripts/gcd_wrapper.sh
PEMAP7=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
CH=$M/charm-prod/reconverse-linux-x86_64-amd
export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:/opt/rocm-6.2.4/lib:$BASELIB
BIN=$M/FoF3.cpu-prod-v82

{
  echo "job : $SLURM_JOB_ID  relay86  -u dist vs serial vs gather, 16 nodes, 2B, 896 PEs"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD) (want a65da5a)  unionfind $(git -C $M/unionfind rev-parse --short HEAD) (want cd2d9c8)"
  echo "  dirty: pt2=$(git -C $M/paratreet2 status --porcelain -uno|grep -v ' utility$'|wc -l) uf=$(git -C $M/unionfind status --porcelain -uno|wc -l)  (want 0 0)"
  echo "  bin $(md5sum $BIN|cut -d' ' -f1) (want d122cdd346117dd3bd4c335debbad70b)"
  echo "  nodes $SLURM_NODELIST"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <umode>
  local label=$1 um=$2
  local log=$OUT/${label}.log t0 t1 rc c m it lf
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=128 --gpus-per-node=8 --export=ALL -t 25:00 \
    "$WRAP" "$BIN" -f "$INPUT" -d oct -u "$um" -c stats -l 32 -i 1 -E 16 \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  local comp=$(grep -m1 '^FOF3STAT components:' "$log")
  c=$(echo "$comp" | grep -o 'components: [0-9]*' | awk '{print $2}')
  m=$(echo "$comp" | grep -o 'max_size [0-9]*' | awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local um_echo=$(grep -m1 -io 'uf2 mode[: ]*[a-z]*' "$log" | awk '{print $NF}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF ${lf:-NONE}"
  # record the LITERAL components line for the bitwise cross-mode comparison
  echo "$comp" > "$OUT/${label}.compline"
  printf "REP %-8s mode=%-7s rc=%d %-7s Iter0=%9s  %4ds %s\n" "$label" "$um" "$rc" "$v" "${it:-NONE}" "$((t1-t0))" "$g"
  grep -m1 'FOF3STAT time_s:' "$log" | sed 's/FOF3STAT //;s/^/      /'
  grep -m1 'FOF3STAT gather:' "$log" | sed 's/^/      /'
  grep -m1 'FOF3STAT edges' "$log" | sed 's/^/      /'
  echo "      $comp"
  echo "${it:-0}" >> "$OUT/${label}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -8 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up, DISCARDED (cold 76 GB read)"
run W dist
for r in 1 2; do
  echo "################ rep $r"
  run A dist
  run B serial
  run C gather
done

echo "################ SUMMARY"
for a in A B C; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;n++;if(lo==""||$1<lo)lo=$1;if($1>hi)hi=$1}
    END{printf "  %-2s n=%d  Iter0 %9.1f ms  [%.1f..%.1f]\n",A,n,i/n,lo,hi}' "$OUT/$a.dat"
done
echo
echo "################ ALGEBRA GATE: components line bitwise across modes"
for a in A B C; do printf "  %-2s %s\n" "$a" "$(cat $OUT/$a.compline 2>/dev/null)"; done
if [ -f "$OUT/A.compline" ] && [ -f "$OUT/B.compline" ] && [ -f "$OUT/C.compline" ]; then
  if cmp -s "$OUT/A.compline" "$OUT/B.compline" && cmp -s "$OUT/B.compline" "$OUT/C.compline"; then
    echo "  *** ALL THREE BITWISE IDENTICAL -- contraction algebra holds at 2B ***"
  else
    echo "  !!! MISMATCH"
    cmp -s "$OUT/B.compline" "$OUT/C.compline" || echo "  !!! B(serial) vs C(gather) DIFFER -> contraction bug"
    cmp -s "$OUT/A.compline" "$OUT/B.compline" || echo "  !!! A(dist) vs B(serial) DIFFER -> worse than a contraction bug"
  fi
fi
echo
echo "################ THE TWO QUESTIONS"
python3 - "$OUT" <<'PY'
import sys,os
O=sys.argv[1]
def M(a):
    p=os.path.join(O,a+'.dat')
    if not os.path.exists(p): return None
    r=[float(l) for l in open(p) if l.strip()]
    return (sum(r)/len(r), min(r), max(r), len(r))
A,B,C=M('A'),M('B'),M('C')
def sep(x,y): return "SEPARATED" if (y[2]<x[1] or y[1]>x[2]) else "OVERLAPS"
if A and B: print("  serial vs dist    B-A = %+9.1f ms (%+.1f%%)  %s" % (B[0]-A[0],100*(B[0]-A[0])/A[0], sep(A,B)))
if A and C: print("  gather vs dist    C-A = %+9.1f ms (%+.1f%%)  %s" % (C[0]-A[0],100*(C[0]-A[0])/A[0], sep(A,C)))
if B and C: print("  gather vs serial  C-B = %+9.1f ms (%+.1f%%)  %s   <- does contraction repair the +28%%?" % (C[0]-B[0],100*(C[0]-B[0])/B[0], sep(B,C)))
print("\n  reference: relay82 -u dist, sizes off, 3 reps: 4467.2 ms [4450.6..4491.4]")
PY
echo "=== $(date +%T) DONE -- $OUT"
