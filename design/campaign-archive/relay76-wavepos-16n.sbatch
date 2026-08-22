#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay76-wavepos-16n
#SBATCH --nodes=16
#SBATCH --time=00:45:00
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
# relay76 -- IS THE WAVE A BAD IDEA, OR FIRED AT THE WRONG MILESTONE?
# Tested with NO code change, using the -E knob.
#
# THE HYPOTHESIS (relay74 + Kale's 2026-08-22 observation).  The ~300 ms of
# near-idle union-find tail is INSIDE the walk bracket: the walk streams
# edges in batches of -E (default 16), so the union cascade runs concurrently
# with the walk and is charged to phase3_walk.  t_uf2 -- which brackets
# fireUF2Edges + QD + find_components -- is only ~40 ms.  The wave fires at
# the fireUF2Edges barrier, i.e. INSIDE that 40 ms bracket, AFTER the
# expensive drain has already happened.  That would explain all three relay74
# observations at once: it rewrites only ~256 parents at 2B, it costs +50 ms,
# and the 300 ms tail is untouched.
#
# THE TEST.  -E 0 turns streaming OFF (classic post-walk injection), which
# moves the whole union cascade INTO the uf2 bracket -- exactly where the
# wave fires.  If the wave is sound and merely mistimed, then under -E 0 it
# should rewrite FAR more than 256 parents and RECOVER time.  If it still
# costs time with a deep forest in front of it, the mechanism itself is
# wrong, not its position.
#
#   E16-off  -E 16, wave off    x2   the current default, relay74 arm C
#   E16-w1   -E 16, FOF_WAVE=1  x2   relay74 arm D (+50.1 ms there)
#   E0-off   -E  0, wave off    x2   streaming off, the deep-forest control
#   E0-w1    -E  0, FOF_WAVE=1  x2   THE DECISIVE ARM
#   E0-w2    -E  0, FOF_WAVE=2  x1   hedge, under the same conditions
#
# NOTE the expected direction of -E itself: the campaign record has -E 16
# worth about -208 ms at 16 nodes against streaming off, so E0 arms should be
# SLOWER overall.  That is not what is being tested -- the question is
# whether the WAVE's delta changes sign when it is given real work to do.
#
# Binary FoF3.cpu-NEW (untraced, md5 93fda4b3), paratreet2 e2a877e +
# unionfind 2ad91a8.  CPU best: ppn 7, PEMAP7, ndev 7, poll 1, -l 32 explicit.
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
unset PARATREET_DEVICE_TREE FOF_GPU_PHASE1
WRAP=$G/scripts/gcd_wrapper.sh
PEMAP7=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
CH=$M/charm-prod/reconverse-linux-x86_64-amd
export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:/opt/rocm-6.2.4/lib:$BASELIB
BIN=$M/FoF3.cpu-NEW

{
  echo "job : $SLURM_JOB_ID  relay76 wave position test via -E, 16 nodes, 2B, 896 PEs"
  echo "  bin $BIN md5 $(md5sum $BIN|cut -d' ' -f1)"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD)  unionfind $(git -C $M/unionfind rev-parse --short HEAD)"
  echo "  nodes $SLURM_NODELIST"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <estream> <wave>
  local label=$1 es=$2 wave=$3
  local log=$OUT/${label}.log
  local t0 t1 rc c m it lf
  if [ "$wave" = 0 ]; then unset FOF_WAVE; else export FOF_WAVE=$wave; fi
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=128 --gpus-per-node=8 --export=ALL -t 12:00 \
    "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l 32 -i 1 -E "$es" \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log"|head -1|awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log"|head -1|awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log"|awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log"|awk '{print $NF}')
  local eb=$(grep -m1 -o 'stream batch (-E): [0-9]*' "$log"|awk '{print $NF}')
  local wk=$(grep -m1 -o 'rewrote [0-9]* parents' "$log"|awk '{print $2}')
  local uf=$(grep -m1 -o ' uf2 [0-9.]*' "$log"|awk '{print $2}')
  local wa=$(grep -m1 -o 'phase3_walk [0-9.]*' "$log"|awk '{print $2}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF ${lf:-NONE}"
  [ "${eb:-N}" != "$es" ] && g="$g !!! -E GATE printed ${eb:-NONE} wanted $es"
  printf "REP %-9s rc=%d %-7s E=%-5s Iter0=%9s  walk=%-7s uf2=%-7s rewrote=%-8s %3ds %s\n" \
         "$label" "$rc" "$v" "${eb:-NONE}" "${it:-NONE}" "${wa:-NONE}" "${uf:-NONE}" "${wk:-none}" "$((t1-t0))" "$g"
  grep -m1 'compression wave' "$log" | sed 's/^/      /'
  grep -m1 'Tree traversal:' "$log" | sed 's/^/      /'
  echo "${it:-0} ${wa:-0} ${uf:-0}" >> "$OUT/${label}.dat"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -6 "$log"|sed 's/^/      | /'; }
  echo
}

echo "################ warm-up, DISCARDED"
run W 16 0
echo "################ rep 1"
run E16-off 16 0; run E16-w1 16 1; run E0-off 0 0; run E0-w1 0 1; run E0-w2 0 2
echo "################ rep 2"
run E16-off 16 0; run E16-w1 16 1; run E0-off 0 0; run E0-w1 0 1

echo "################ SUMMARY  (mean Iter0 / phase3_walk / uf2)"
for a in E16-off E16-w1 E0-off E0-w1 E0-w2; do
  [ -f "$OUT/$a.dat" ] || continue
  awk -v A=$a '{i+=$1;w+=$2;u+=$3;n++} END{printf "  %-8s n=%d  Iter0 %8.1f ms   walk %6.3f s   uf2 %6.3f s\n",A,n,i/n,w/n,u/n}' "$OUT/$a.dat"
done
echo
echo "################ THE TEST: does the wave's delta change sign with a deep forest?"
python3 - "$OUT" <<'PY'
import sys,os
O=sys.argv[1]
def M(a):
    p=os.path.join(O,a+'.dat')
    if not os.path.exists(p): return None
    r=[list(map(float,l.split())) for l in open(p) if l.strip()]
    n=len(r); return [sum(x[k] for x in r)/n for k in range(3)]
d={a:M(a) for a in ('E16-off','E16-w1','E0-off','E0-w1','E0-w2')}
for base,w1,tag in (('E16-off','E16-w1','-E 16 (streaming ON, default)'),
                    ('E0-off','E0-w1','-E  0 (streaming OFF, deep forest)')):
    if d[base] and d[w1]:
        di=d[w1][0]-d[base][0]; du=d[w1][2]-d[base][2]
        print("  %-34s wave delta Iter0 %+8.1f ms (%+.2f%%)   uf2 %+.3f s"
              %(tag,di,100*di/d[base][0],du))
if d['E0-off'] and d['E0-w2']:
    di=d['E0-w2'][0]-d['E0-off'][0]
    print("  %-34s hedge delta Iter0 %+8.1f ms (%+.2f%%)"%('-E  0 hedge',di,100*di/d['E0-off'][0]))
if d['E16-off'] and d['E0-off']:
    di=d['E0-off'][0]-d['E16-off'][0]
    print("\n  for reference, -E itself: E0 - E16 = %+.1f ms (streaming is worth %+.1f ms)"%(di,-di))
PY
echo
echo "=== $(date +%T) DONE -- $OUT"
