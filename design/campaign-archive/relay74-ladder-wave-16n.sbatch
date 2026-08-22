#!/bin/bash
#SBATCH --account=csc710
#SBATCH -J relay74-ladder-wave-16n
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
# relay74 -- (1) ATTRIBUTION of the component_histogram row, three-point
# build ladder; (2) FIRST 2B ARMS OF THE COMPRESSION WAVE.
#
# QUESTION 1.  relay72 credited the histogram row's 0.429 -> 0.243 s to the
# prefix removal.  The laptop session disputes it: the phantom labels were
# +262,837 on 425M = +0.06%, far too few to halve the row, and what actually
# changed between the correct-old and correct-new points is the SORT+SCAN
# pair path replacing the unordered_map, which landed in 22efde5 -- AFTER
# relay70's binary.  THE DISPUTE IS WELL FOUNDED AND MY relay72 READING WAS
# CONFOUNDED: I compared relay70's 0.497 s (3d25e14) against relay72's
# 0.243 s (22efde5) and called the difference "the prefix removal", but
# those two binaries differ by the sort path as well.  The ladder settles it
# with three points that differ by ONE thing each.
#
#   OLD  FoF3.cpu-prod   md5 b558cc05   paratreet2 7c28411 + unionfind c00032f
#   MID  FoF3.cpu-MID    md5 1fa46e42   paratreet2 277ec6d + unionfind 2ad91a8
#        prefix removal + sign fix; histogram pair path STILL unordered_map
#   NEW  FoF3.cpu-NEW    md5 93fda4b3   paratreet2 e2a877e + unionfind 2ad91a8
#        sort-scan pair path; wave available
#
#     prefix effect = B - A       sort effect = C - B
#
# unionfind 2ad91a8 for BOTH built points, as instructed.  MID and NEW differ
# ONLY in paratreet2.  The wave is fired from the paratreet2 side (e2a877e),
# so MID cannot fire it even though its unionfind carries the knob.
#
# QUESTION 2.  The wave, default OFF.  D is direct mode, E the hedge.
#
# ARMS, CPU best, ppn 7, leaf 32, interleaved rep-major, warm-up discarded:
#   A OLD x2   B MID x2   C NEW wave off x2   D NEW FOF_WAVE=1 x2
#   E NEW FOF_WAVE=2 x1
#
# -l 32 IS PASSED EXPLICITLY TO EVERY ARM.  The OLD binary predates 005b76f
# and its default leaf is 12, not 32; without the flag A would be running a
# different problem from B/C/D/E.  That is the relay61 trap and it would
# invalidate the entire attribution.  Every arm also gates on the leaf the
# app PRINTS.
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
OLD=$M/FoF3.cpu-prod; MID=$M/FoF3.cpu-MID; NEW=$M/FoF3.cpu-NEW

{
  echo "job : $SLURM_JOB_ID  relay74 attribution ladder + compression wave, 16 nodes, 2B, 896 PEs"
  echo "  OLD $OLD  md5 $(md5sum $OLD | cut -d' ' -f1)"
  echo "  MID $MID  md5 $(md5sum $MID | cut -d' ' -f1)"
  echo "  NEW $NEW  md5 $(md5sum $NEW | cut -d' ' -f1)"
  echo "  paratreet2 $(git -C $M/paratreet2 rev-parse --short HEAD)  unionfind $(git -C $M/unionfind rev-parse --short HEAD)"
  echo "  nodes $SLURM_NODELIST"
} | tee "$OUT/provenance.txt"; echo

run () {   # run <label> <bin> <wave 0|1|2>
  local label=$1 bin=$2 wave=$3
  local log=$OUT/${label}.log
  local t0 t1 rc c m it lf
  if [ "$wave" = 0 ]; then unset FOF_WAVE; else export FOF_WAVE=$wave; fi
  t0=$(date +%s)
  srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
    --distribution=block:block --ntasks=128 --gpus-per-node=8 --export=ALL -t 12:00 \
    "$WRAP" "$bin" -f "$INPUT" -d oct -u dist -c stats -l 32 -i 1 \
    +ppn 7 +pemap $PEMAP7 +lci_ndevices 7 +backend_poll_thread 1 > "$log" 2>&1
  rc=$?; t1=$(date +%s)
  c=$(grep -o 'FOF3STAT components: [0-9]*' "$log" | head -1 | awk '{print $3}')
  m=$(grep -o 'max_size [0-9]*' "$log" | head -1 | awk '{print $2}')
  it=$(grep -m1 -o 'Iteration 0 time: [0-9.]*' "$log" | awk '{print $4}')
  lf=$(grep -m1 'Maximum number of particles per leaf:' "$log" | awk '{print $NF}')
  local v=DIFFERS; [ "${c:-N}" = "$GOLD" ] && [ "${m:-N}" = "$GOLDMAX" ] && v=EXACT
  local g=ok; [ "${lf:-N}" != "32" ] && g="!!! LEAF GATE: ran ${lf:-NONE}"
  local hist=$(grep -m1 -o 'component_histogram [0-9.]*' "$log" | awk '{print $2}')
  local wf=$(grep -c 'compression wave' "$log")
  printf "REP %-8s rc=%d %-7s leaf=%-3s Iter0=%9s ms  hist=%-7s wave_lines=%-3s %3ds %s\n" \
         "$label" "$rc" "$v" "${lf:-NONE}" "${it:-NONE}" "${hist:-NONE}" "$wf" "$((t1-t0))" "$g"
  grep -m1 'FOF3STAT time_s: uf2_setup' "$log" | sed 's/FOF3STAT //;s/^/      /'
  grep -m1 'Tree traversal:' "$log" | sed 's/^/      /'
  grep -m1 'compression wave' "$log" | sed 's/^/      /'
  grep -m1 'FOF3STAT components:' "$log" > "$OUT/${label}.components"
  echo "${it:-0}"   >> "$OUT/${label%%-*}.iter"
  echo "${hist:-0}" >> "$OUT/${label%%-*}.hist"
  [ "$rc" != 0 ] && { echo "      ---- NOT CLEAN ----"; tail -8 "$log" | sed 's/^/      | /'; }
  echo
}

echo "################ warm-up, DISCARDED (cold 76 GB read)"
run warmup $NEW 0
echo "################ rep 1"
run A-r1 $OLD 0 ; run B-r1 $MID 0 ; run C-r1 $NEW 0 ; run D-r1 $NEW 1 ; run E-r1 $NEW 2
echo "################ rep 2"
run A-r2 $OLD 0 ; run B-r2 $MID 0 ; run C-r2 $NEW 0 ; run D-r2 $NEW 1

echo "################ SUMMARY -- means"
for a in A B C D E; do
  [ -f "$OUT/$a.iter" ] || continue
  paste "$OUT/$a.iter" "$OUT/$a.hist" | awk -v A=$a \
    '{i+=$1; h+=$2; n++} END{printf "  %-2s n=%d  Iter0 %8.1f ms   component_histogram %6.3f s\n", A,n,i/n,h/n}'
done
echo
echo "################ ATTRIBUTION -- the histogram row"
python3 - "$OUT" <<'PY'
import sys,os
O=sys.argv[1]
def mean(a,f):
    p=os.path.join(O,a+'.'+f)
    if not os.path.exists(p): return None
    v=[float(x) for x in open(p) if x.strip()]
    return sum(v)/len(v) if v else None
h={a:mean(a,'hist') for a in 'ABCDE'}
i={a:mean(a,'iter') for a in 'ABCDE'}
if all(h[a] is not None for a in 'ABC'):
    print("  component_histogram   A(OLD) %.3f s   B(MID) %.3f s   C(NEW) %.3f s"%(h['A'],h['B'],h['C']))
    print("    prefix effect  B-A = %+.3f s" % (h['B']-h['A']))
    print("    sort   effect  C-B = %+.3f s" % (h['C']-h['B']))
    tot=h['C']-h['A']
    if abs(tot)>1e-9:
        print("    share of the total %+.3f s:  prefix %+.0f%%   sort %+.0f%%"
              %(tot,100*(h['B']-h['A'])/tot,100*(h['C']-h['B'])/tot))
print()
if all(i[a] is not None for a in 'ABC'):
    print("  Iter0                 A %.1f   B %.1f   C %.1f ms"%(i['A'],i['B'],i['C']))
    print("    prefix effect  B-A = %+.1f ms" % (i['B']-i['A']))
    print("    sort   effect  C-B = %+.1f ms" % (i['C']-i['B']))
print()
if i.get('D') is not None:
    print("  WAVE   C(off) %.1f   D(mode 1) %.1f ms   D-C = %+.1f ms (%+.2f%%)"
          %(i['C'],i['D'],i['D']-i['C'],100*(i['D']-i['C'])/i['C']))
if i.get('E') is not None:
    print("         E(mode 2 hedge) %.1f ms   E-C = %+.1f ms (%+.2f%%)"
          %(i['E'],i['E']-i['C'],100*(i['E']-i['C'])/i['C']))
PY
echo
echo "################ EXACTNESS EQUIVALENCE -- components line, bitwise"
for pair in "C-r1 D-r1" "C-r1 E-r1" "C-r1 B-r1" "C-r1 A-r1"; do
  set -- $pair
  if [ -s "$OUT/$1.components" ] && [ -s "$OUT/$2.components" ]; then
    cmp -s "$OUT/$1.components" "$OUT/$2.components" \
      && echo "    $1 vs $2 : IDENTICAL" \
      || { echo "    $1 vs $2 : !!! DIFFER"; diff <(tr ' ' '\n' <"$OUT/$1.components") <(tr ' ' '\n' <"$OUT/$2.components") | head -8 | sed 's/^/        /'; }
  fi
done
echo
echo "=== $(date +%T) DONE -- $OUT"
