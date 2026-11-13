#!/bin/bash
# Step 2, parts 2a and 2b (design/subhalo-checks.md, "Step 2: unbinding
# inside FoF3"): FoF3 -U on 8m-lambb (b = 0.2, periodic, -K -m 20), every
# kept group gathered onto one process and unbound on that process's GPU.
# Two nodes: FoF3 on 1, 8 and 16 processes (CPU arm for FoF itself), the
# standalone tools on one node's cores and one die.
#
#   make -C subhalo device all compare      # libsubhalodevice.a, unbind, unbindcheck
#   make -C fof GPU=1 ...; make -C examples/fof3 GPU=1 ...   # (fof/gpu/rebuild_deps.sh)
#   CHECK_ONLY=1 bash subhalo/run_fof3_unbind.sbatch          # inputs only
#   sbatch subhalo/run_fof3_unbind.sbatch
#
# Settings (code units; eps is the snapshot's softening, 0.00999999975 Mpc
# parsed in single precision as unbind does, over the 100 Mpc box):
#   exact   cap=0 (all pairs), bf=1
#   cap     cap=10000, refine=1000, bf=1                 (f64)
#   capr    the same with kernel=f32, rsqrt=1
#   prod    the defaults: cap 10000, refine 1000, bf 0.995 (timing only)
# Checks:
#   1. FoF3's label_digest (c4f41054eacf794c) and catalogue digest
#      (57c1e840e74a8aae) unchanged in every run.
#   2. exact, cap, capr: both unbinding digests equal on 1, 8 and 16 processes.
#   3. One process against the references, through fof3ubd.py bin and
#      unbindcheck at tau: exact against HBT-HERONS's exact reference; cap
#      and capr (capr also with --single-against cap) against unbindspec
#      --cap 10000 --refine 1000 (job 5916562's file). The pass rule takes
#      the difference of units and of precision (FoF3 holds single-precision
#      code units, the snapshot double-precision Mpc).
#   4. One process against the standalone unbind on the snapshot, same
#      settings (fof3ubd.py compare): bound sets, energies, the most bound
#      particle, the refined energies.
#   5. The same with FoF3 given the G the standalone tools use on this
#      snapshot (G=0.9999517944622952 in code units: HBT-HERONS's 43.0071
#      in tipsy2hbt's units, which assume 43.00917), and one 8-process run
#      at the defaults against the digests of job 5916950.
# PARTS selects the parts (default "1 2 3 4"; 5 alone runs on one node:
#   PARTS=5 sbatch --nodes=1 subhalo/run_fof3_unbind.sbatch).
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:30:00
#SBATCH --job-name=fof3unbind
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive

PT=${PT:-/ccs/home/rrao/paratreet2}
S=${S:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11}
T8=$S/8m/8m-lambb.tipsy
SNAP=$S/8m/snap/snap_0000.hdf5
EXACT_REF=$S/8m/snap/exact-double.particles.bin
CAP_REF=$S/refine.5916562/8m-lambb/cap10000/spec.particles.bin
WRAP=${WRAP:-/lustre/orion/csc710/scratch/rrao/lazycanopy-2026-11-02/v7/gcd_wrap.sh}
HBIN=${HBIN:-/opt/cray/pe/hdf5/1.14.3.9/gnu/12.3/bin}
TAU=0.00141
EPS=9.999999776482582e-05
LABEL_DIGEST=c4f41054eacf794c
CATALOG_DIGEST=57c1e840e74a8aae
PARTS=${PARTS:-"1 2 3 4"}
part() { [[ " $PARTS " == *" $1 "* ]]; }
GEFF=0.9999517944622952
declare -A SET=(
  [exact]="eps=$EPS,cap=0,bf=1"
  [cap]="eps=$EPS,cap=10000,refine=1000,bf=1"
  [capr]="eps=$EPS,cap=10000,refine=1000,bf=1,kernel=f32,rsqrt=1"
  [prod]="eps=$EPS"
)

missing=0
for f in $T8 $SNAP $EXACT_REF $CAP_REF $WRAP $PT/examples/fof3/FoF3 $PT/subhalo/unbind \
         $PT/subhalo/unbindcheck $PT/subhalo/fof3ubd.py $PT/subhalo/capdistance.py; do
  [ -s $f ] || { echo "### MISSING: $f"; missing=1; }
done
strings $PT/examples/fof3/FoF3 | grep -q 'FOF3STAT unbind:' ||
  { echo "### $PT/examples/fof3/FoF3 has no -U: rebuild with GPU=1"; missing=1; }
[ -x $HBIN/h5dump ] || { echo "### MISSING: $HBIN/h5dump"; missing=1; }
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
export H5DUMP=$HBIN/h5dump
J=${SLURM_JOB_ID:-nojob}
OUT=$S/fof3unbind.$J
B=$OUT/bin
mkdir -p $B && cp $PT/examples/fof3/FoF3 $PT/subhalo/unbind $PT/subhalo/unbindcheck \
  $PT/subhalo/fof3ubd.py $PT/subhalo/capdistance.py $B/ || exit 1
echo "### nodes $(scontrol show hostnames $SLURM_JOB_NODELIST | tr '\n' ' ') commit $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet || echo ' + uncommitted changes')"
echo "### md5: FoF3 $(md5sum $B/FoF3 | cut -c1-12) unbind $(md5sum $B/unbind | cut -c1-12) unbindcheck $(md5sum $B/unbindcheck | cut -c1-12)"

cpu() {   # a host program on one node's cores
  OMP_NUM_THREADS=56 OMP_PROC_BIND=spread OMP_PLACES=cores timeout --signal=TERM --kill-after=15 900 \
    srun --nodes=1 --ntasks=1 --cpus-per-task=112 --unbuffered --export=ALL "$@"
}
gpu() {   # unbind on one die
  ROCR_VISIBLE_DEVICES=0 timeout --signal=TERM --kill-after=15 300 \
    srun --nodes=1 --ntasks=1 --cpus-per-task=14 --gpus-per-node=8 --unbuffered --export=ALL "$@"
}
# FoF3 -K -m 20 -U <settings>: run name, settings, processes.
fof3() {
  local name=$1 set=$2 np=$3 d=$OUT/$1 map net nodes=1
  mkdir -p $d
  case $np in
    1) map=1-7 ;;
    *) map=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63 ;;
  esac
  net="--network=single_node_vni"
  [ $np -gt 8 ] && { nodes=$(( (np + 7) / 8 )); net=""; }
  ( cd $PT/examples/fof3 && PARATREET_DEVICE_TREE=0 timeout --signal=TERM --kill-after=15 600 \
    srun --mpi=cray_shasta $net --unbuffered --cpu-bind=none --distribution=block:block \
      --nodes=$nodes --ntasks=$np --gpus-per-node=8 --export=ALL \
      $WRAP $B/FoF3 -f $T8 -d oct -u dist -b 0.2 -c stats -l 12 -i 1 -K -m 20 -P 1 \
      -U "$set,out=$d/u" +ppn 7 +pemap $map +lci_ndevices 7 +backend_poll_thread 1 \
      > $d/fof3.log 2>&1 )
  local rc=$?
  local ld=$(grep -o 'label_digest: n [0-9]* digest [0-9a-f]*' $d/fof3.log | awk '{print $NF}')
  local cg=$(grep -o 'FOF3STAT catalog: .* digest [0-9a-f]*' $d/fof3.log | awk '{print $NF}')
  echo "  $name ($np processes): exit $rc; label_digest $ld ($([ "$ld" = $LABEL_DIGEST ] && echo unchanged || echo CHANGED)), catalogue $cg ($([ "$cg" = $CATALOG_DIGEST ] && echo unchanged || echo CHANGED))"
  grep -E 'FOF3STAT unbind' $d/fof3.log | sed 's/^/    /'
  [ $rc = 0 ] || grep -m5 -E 'Fatal|Abort|abort|rror|subhalodev' $d/fof3.log | sed 's/^/    /'
}
digests() { grep -o 'digest_subhaloes [0-9a-f]* digest_particles [0-9a-f]*' $OUT/$1/fof3.log; }

if part 1; then
echo "=== 1. FoF3 -U on 1, 8 and 16 processes"
for set in exact cap capr; do
  for np in 1 8 16; do fof3 ${set}_np$np "${SET[$set]}" $np; done
done
for np in 8 16; do fof3 prod_np$np "${SET[prod]}" $np; done
fi

if part 2; then
echo "=== 2. the unbinding digests do not depend on the number of processes"
for set in exact cap capr; do
  a=$(digests ${set}_np1) b=$(digests ${set}_np8) c=$(digests ${set}_np16)
  same=$([ -n "$a" ] && [ "$a" = "$b" ] && [ "$a" = "$c" ] && echo yes || echo NO)
  echo "  $set: 1 process: $a; 8: $b; 16: $c; equal: $same"
done
fi

check() {   # tag, run name, reference, options...
  local tag=$1 name=$2 ref=$3; shift 3
  cpu $B/unbindcheck $SNAP $ref $OUT/${name}_np1.particles.bin --tau $TAU "$@" > $OUT/unbindcheck_$tag.log 2>&1
  local rc=$?
  echo "  unbindcheck $tag ($name against $(basename $ref)${*:+, $*}): exit $rc (0 = pass)"
  grep -v '^srun' $OUT/unbindcheck_$tag.log | sed 's/^/    /'
}
standalone() {   # set: the standalone unbind on the snapshot with that set's settings
  [ -s $OUT/unbind_$1.hdf5 ] && return
  case $1 in
    exact) gpu $B/unbind $SNAP --bound-fraction 1 --dump $OUT/unbind_exact.hdf5 > $OUT/unbind_exact.log 2>&1 ;;
    cap) gpu $B/unbind $SNAP --bound-fraction 1 --cap 10000 --refine 1000 --dump $OUT/unbind_cap.hdf5 \
           > $OUT/unbind_cap.log 2>&1 ;;
    capr) gpu $B/unbind $SNAP --bound-fraction 1 --cap 10000 --refine 1000 --kernel f32 --rsqrt \
            --dump $OUT/unbind_capr.hdf5 > $OUT/unbind_capr.log 2>&1 ;;
  esac
}

if part 3; then
echo "=== 3. one process against the references (unbindcheck at tau)"
for set in exact cap capr; do
  python3 $B/fof3ubd.py bin $SNAP $OUT/${set}_np1/u $OUT/${set}_np1.particles.bin 2>&1 | sed 's/^/  /'
done
check exact exact $EXACT_REF
check cap cap $CAP_REF --cap 10000
check capr capr $CAP_REF --cap 10000
check capr_allowance capr $CAP_REF --cap 10000 --single-against $OUT/cap_np1.particles.bin
fi

if part 4; then
echo "=== 4. one process against the standalone unbind on the snapshot"
for set in exact cap capr; do
  standalone $set
  echo "  $set: unbind: $(grep -E '^bound particles' $OUT/unbind_$set.log)"
  python3 $B/fof3ubd.py compare $SNAP $OUT/${set}_np1/u $OUT/unbind_$set.hdf5 2>&1 | sed 's/^/    /'
done
fi

if part 5; then
echo "=== 5. FoF3 with the standalone tools' G ($GEFF in code units)"
for set in exact cap; do
  fof3 ${set}_g_np1 "${SET[$set]},G=$GEFF" 1
  python3 $B/fof3ubd.py bin $SNAP $OUT/${set}_g_np1/u $OUT/${set}_g_np1.particles.bin 2>&1 | sed 's/^/  /'
  standalone $set
  echo "  $set with G $GEFF against the standalone unbind ($(grep -E '^bound particles' $OUT/unbind_$set.log)):"
  python3 $B/fof3ubd.py compare $SNAP $OUT/${set}_g_np1/u $OUT/unbind_$set.hdf5 2>&1 | sed 's/^/    /'
done
check exact_g exact_g $EXACT_REF
check cap_g cap_g $CAP_REF --cap 10000
fof3 cap_np8 "${SET[cap]}" 8
echo "  cap_np8 at G = 1 after the rebuild: $(digests cap_np8) (job 5916950: digest_subhaloes e478358bcfe0e681 digest_particles b7e7a974def45be1)"
fi
echo "=== done $(date +%T)"
