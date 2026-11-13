#!/bin/bash
# Step 2, part 2c (design/subhalo-checks.md, "Step 2: unbinding inside
# FoF3"): groups that stay spread over processes, unbound where their
# particles are (subhalo/unbind_spread.h). 8m-lambb, cap 10,000, refinement
# 1,000, every check run until a pass removes nothing (bf=1). With
# gather=10000 every group above the cap (60 of them) stays spread.
#
#   make -C subhalo device all compare      # libsubhalodevice.a, unbind, unbindcheck
#   make -C fof GPU=1 ...; make -C examples/fof3 GPU=1 ...   # (fof/gpu/rebuild_deps.sh)
#   CHECK_ONLY=1 bash subhalo/run_fof3_spread.sbatch          # inputs only
#   PARTS="1 2 4" sbatch --nodes=1 subhalo/run_fof3_spread.sbatch
#   PARTS="3 4" sbatch subhalo/run_fof3_spread.sbatch         # two nodes
#
# Parts:
#   1. The standalone unbind on the snapshot, one die: gathered, and with
#      --spread H (the rounds in one process, H holders): H = 1, H = 4 (id
#      mod 4) twice, H = 8 slabs in x; f64, and f32 rsqrt at H = 4.
#      Checks: each spread run passes unbindcheck --cap and centrecheck.py at
#      tau against unbindspec --cap 10000 --refine 1000 (job 5916562's
#      files), f32 rsqrt also with the single-precision allowance against
#      the f64 run at H = 4; the two H = 4 runs give the same digest and the
#      same Bound, Energy, MostBound and RefinedEnergy; each prints how it
#      differs from the gathered run.
#   2. FoF3 on one node, G = 0.9999517944622952 (the snapshot's, so the
#      references apply without the G term; see "Results of 2a and 2b"):
#      gathered on 1 process; gather=10000 on 1 and 8 processes, 8 twice;
#      gather=10000 in f32 rsqrt on 8; the defaults (G = 1; gather 1,000,000,
#      which spreads nothing on 8m-lambb) on 8, against job 5916950's digests.
#   3. FoF3 on 16 processes (two nodes): gather=10000 (G as in 2), and the
#      defaults with and without gather=10000 (G = 1) for the time.
#   4. Checks of the FoF3 runs present: FoF3's label and catalogue digests
#      (printed with each run); the two 8-process runs give the same digests;
#      each spread run through fof3ubd.py bin and unbindcheck --cap at tau
#      against job 5916562's reference (f32 rsqrt also with the allowance
#      against the f64 run on 8), and through fof3ubd.py compare against the
#      standalone gathered unbind (bound sets, energies, most bound particle).
#
# Pass rule: unchanged (tau 0.00141, the amended rule, the single-precision
# allowance); spread groups are held to it across layouts (decision 3).

#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:45:00
#SBATCH --job-name=fof3spread
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/fof3spread.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/fof3spread.%j.err

PT=${PT:-/ccs/home/rrao/paratreet2}
S=${S:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11}
T8=$S/8m/8m-lambb.tipsy
SNAP=$S/8m/snap/snap_0000.hdf5
CAP_REF=$S/refine.5916562/8m-lambb/cap10000/spec.particles.bin
CAP_CEN=$S/refine.5916562/8m-lambb/cap10000/spec.centre.bin
WRAP=${WRAP:-/lustre/orion/csc710/scratch/rrao/lazycanopy-2026-11-02/v7/gcd_wrap.sh}
HBIN=${HBIN:-/opt/cray/pe/hdf5/1.14.3.9/gnu/12.3/bin}
TAU=0.00141
EPS=9.999999776482582e-05
GEFF=0.9999517944622952
LABEL_DIGEST=c4f41054eacf794c
CATALOG_DIGEST=57c1e840e74a8aae
PROD_NP8_DIGESTS="digest_subhaloes dd4e9389c2cc2b26 digest_particles eac4ff3d4463199d"
PARTS=${PARTS:-"1 2 4"}
part() { [[ " $PARTS " == *" $1 "* ]]; }
CAPG="eps=$EPS,cap=10000,refine=1000,bf=1,G=$GEFF"
declare -A SET=(
  [gath]="$CAPG,gather=0"
  [spr]="$CAPG,gather=10000"
  [sprr]="$CAPG,gather=10000,kernel=f32,rsqrt=1"
  [def]="eps=$EPS"
  [defspr]="eps=$EPS,gather=10000"
)

missing=0
for f in $T8 $SNAP $CAP_REF $CAP_CEN $WRAP $PT/examples/fof3/FoF3 $PT/subhalo/unbind \
         $PT/subhalo/unbindcheck $PT/subhalo/fof3ubd.py $PT/subhalo/capdistance.py \
         $PT/subhalo/centrecheck.py; do
  [ -s $f ] || { echo "### MISSING: $f"; missing=1; }
done
strings $PT/examples/fof3/FoF3 | grep -q 'FOF3STAT unbind_spread' ||
  { echo "### $PT/examples/fof3/FoF3 has no part 2c: rebuild with GPU=1"; missing=1; }
strings $PT/subhalo/unbind | grep -q 'spread: digest' ||
  { echo "### $PT/subhalo/unbind has no --spread: make -C subhalo unbind"; missing=1; }
[ -x $HBIN/h5dump ] && [ -x $HBIN/h5diff ] || { echo "### MISSING: $HBIN/h5dump or h5diff"; missing=1; }
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
export H5DUMP=$HBIN/h5dump
J=${SLURM_JOB_ID:-nojob}
OUT=$S/fof3spread.$J
B=$OUT/bin
mkdir -p $B && cp $PT/examples/fof3/FoF3 $PT/subhalo/unbind $PT/subhalo/unbindcheck \
  $PT/subhalo/fof3ubd.py $PT/subhalo/capdistance.py $PT/subhalo/centrecheck.py $B/ || exit 1
echo "### parts $PARTS; nodes $(scontrol show hostnames $SLURM_JOB_NODELIST | tr '\n' ' ') commit $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet || echo ' + uncommitted changes')"
echo "### md5: FoF3 $(md5sum $B/FoF3 | cut -c1-12) unbind $(md5sum $B/unbind | cut -c1-12) unbindcheck $(md5sum $B/unbindcheck | cut -c1-12)"

cpu() {   # a host program on one node's cores
  OMP_NUM_THREADS=56 OMP_PROC_BIND=spread OMP_PLACES=cores timeout --signal=TERM --kill-after=15 900 \
    srun --nodes=1 --ntasks=1 --cpus-per-task=112 --unbuffered --export=ALL "$@"
}
gpu() {   # unbind on one die
  ROCR_VISIBLE_DEVICES=0 timeout --signal=TERM --kill-after=15 300 \
    srun --nodes=1 --ntasks=1 --cpus-per-task=14 --gpus-per-node=8 --unbuffered --export=ALL "$@"
}

# ---- 1. the standalone unbind ------------------------------------------------
# name, options: one run of unbind on the snapshot at the cap, with a dump.
ub() {
  local name=$1; shift
  gpu $B/unbind $SNAP --bound-fraction 1 --cap 10000 --refine 1000 "$@" \
    --dump $OUT/ub_$name.hdf5 > $OUT/ub_$name.log 2>&1
  local rc=$?
  echo "  unbind $name ($*): exit $rc; $(grep -E '^bound particles' $OUT/ub_$name.log)"
  grep -E '^spread:' $OUT/ub_$name.log | sed 's/^/    /'
  [ $rc = 0 ] || grep -v '^srun' $OUT/ub_$name.log | tail -5 | sed 's/^/    /'
}
ubcheck() {   # name [single-precision allowance against this dump]
  local name=$1 dbl=$2 rc1 rc2
  cpu $B/unbindcheck $SNAP $CAP_REF $OUT/ub_$name.hdf5 --cap 10000 --tau $TAU \
    ${dbl:+--single-against $dbl} > $OUT/ubcheck_$name${dbl:+_allowance}.log 2>&1; rc1=$?
  python3 $B/centrecheck.py $CAP_REF $CAP_CEN $OUT/ub_$name.hdf5 --tau $TAU \
    ${dbl:+--single-against $dbl} > $OUT/cencheck_$name${dbl:+_allowance}.log 2>&1; rc2=$?
  echo "    unbindcheck exit $rc1, centrecheck exit $rc2 (0 = pass)${dbl:+ with the allowance against $(basename $dbl)}"
  [ $rc1 = 0 ] || grep -v '^srun' $OUT/ubcheck_$name${dbl:+_allowance}.log | tail -8 | sed 's/^/    /'
  [ $rc2 = 0 ] || sed -n '1,6p;/^FAIL/p' $OUT/cencheck_$name${dbl:+_allowance}.log | sed 's/^/    /'
}
same_dumps() {   # a b: Bound, Energy, MostBound, RefinedEnergy identical?
  local same=yes ds
  for ds in /Bound /Energy /MostBound /RefinedEnergy; do
    $HBIN/h5diff $OUT/ub_$1.hdf5 $OUT/ub_$2.hdf5 $ds $ds > /dev/null 2>&1 || same=NO
  done
  echo $same
}

if part 1; then
echo "=== 1. unbind --spread on the snapshot, one die"
ub gath
ub gath_r --kernel f32 --rsqrt
for spec in "s1 --spread 1" "s4 --spread 4" "s4b --spread 4" "s8slab --spread 8 --spread-slabs" \
            "s4r --spread 4 --kernel f32 --rsqrt"; do
  set -- $spec
  name=$1; shift
  ub $name "$@"
  if [ $name = s4r ]; then ubcheck s4r; ubcheck s4r $OUT/ub_s4.hdf5
  elif [ $name != s4b ]; then ubcheck $name; fi
done
d1=$(grep -o 'spread: digest [0-9a-f]*' $OUT/ub_s4.log) d2=$(grep -o 'spread: digest [0-9a-f]*' $OUT/ub_s4b.log)
echo "  H = 4 twice: $d1 / $d2; digests equal: $([ -n "$d1" ] && [ "$d1" = "$d2" ] && echo yes || echo NO); dumps identical: $(same_dumps s4 s4b)"
echo "  H = 1 against gathered: dumps identical: $(same_dumps s1 gath) (the spread lines above say where they differ)"
fi

# ---- 2, 3. FoF3 ------------------------------------------------------------------
fof3() {   # name, settings, processes
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
  echo "  $name ($np processes; $set): exit $rc; label_digest $ld ($([ "$ld" = $LABEL_DIGEST ] && echo unchanged || echo CHANGED)), catalogue $cg ($([ "$cg" = $CATALOG_DIGEST ] && echo unchanged || echo CHANGED))"
  grep -E 'FOF3STAT unbind' $d/fof3.log | sed 's/^/    /'
  [ $rc = 0 ] || grep -m8 -E 'Fatal|Abort|abort|rror|subhalodev|unbind::spread' $d/fof3.log | sed 's/^/    /'
}
digests() { grep -o 'digest_subhaloes [0-9a-f]* digest_particles [0-9a-f]*' $OUT/$1/fof3.log 2>/dev/null; }

if part 2; then
echo "=== 2. FoF3 on one node"
fof3 gath_np1 "${SET[gath]}" 1
fof3 spr_np1 "${SET[spr]}" 1
fof3 spr_np8 "${SET[spr]}" 8
fof3 spr_np8b "${SET[spr]}" 8
fof3 sprr_np8 "${SET[sprr]}" 8
fof3 def_np8 "${SET[def]}" 8
echo "  defaults on 8: $(digests def_np8); job 5916950: $PROD_NP8_DIGESTS; equal: $([ "$(digests def_np8)" = "$PROD_NP8_DIGESTS" ] && echo yes || echo NO)"
fi

if part 3; then
echo "=== 3. FoF3 on 16 processes"
fof3 spr_np16 "${SET[spr]}" 16
fof3 def_np16 "${SET[def]}" 16
fof3 defspr_np16 "${SET[defspr]}" 16
fi

if part 4; then
echo "=== 4. checks of the FoF3 runs"
a=$(digests spr_np8) b=$(digests spr_np8b)
[ -n "$a$b" ] && echo "  gather=10000 on 8 twice: $a / $b; equal: $([ -n "$a" ] && [ "$a" = "$b" ] && echo yes || echo NO)"
[ -s $OUT/ub_gath.hdf5 ] || ub gath
for name in gath_np1 spr_np1 spr_np8 spr_np16 sprr_np8; do
  [ -s $OUT/$name/fof3.log ] && ls $OUT/$name/u.*ubd > /dev/null 2>&1 || continue
  python3 $B/fof3ubd.py bin $SNAP $OUT/$name/u $OUT/$name.particles.bin 2>&1 | sed 's/^/  /'
  cpu $B/unbindcheck $SNAP $CAP_REF $OUT/$name.particles.bin --cap 10000 --tau $TAU \
    > $OUT/fcheck_$name.log 2>&1; rc=$?
  echo "    unbindcheck $name against job 5916562's reference: exit $rc (0 = pass)"
  [ $rc = 0 ] || grep -v '^srun' $OUT/fcheck_$name.log | tail -8 | sed 's/^/    /'
  if [ $name = sprr_np8 ] && [ -s $OUT/spr_np8.particles.bin ]; then
    cpu $B/unbindcheck $SNAP $CAP_REF $OUT/$name.particles.bin --cap 10000 --tau $TAU \
      --single-against $OUT/spr_np8.particles.bin > $OUT/fcheck_${name}_allowance.log 2>&1; rc=$?
    echo "    the same with the allowance against spr_np8: exit $rc (0 = pass)"
  fi
  [ $name = sprr_np8 ] && continue
  echo "    $name against the standalone gathered unbind:"
  python3 $B/fof3ubd.py compare $SNAP $OUT/$name/u $OUT/ub_gath.hdf5 2>&1 | sed 's/^/      /'
done
fi
echo "=== done $(date +%T)"
