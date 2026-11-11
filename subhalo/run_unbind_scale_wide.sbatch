#!/bin/bash
# unbind at a scale factor other than 1 and on groups at least half the box
# wide (design/subhalo-checks.md, "A scale factor other than 1" and "Groups
# at least half the box wide"). One node: FoF3 on the node's cores, unbind
# on one MI250X die, unbindspec and unbindcheck on the node's cores.
#
# The reference here is unbindspec, the written definition. It is NOT an
# HBT-HERONS run: those are made on the Mac from the same inputs.
#
#   make -C subhalo all convert compare
#   sbatch subhalo/run_unbind_scale_wide.sbatch
#
# Inputs, all made from real particles (lambb.00500):
#   a0.5, a1.25   8m-lambb with its FoF3 groups (b = 0.2), written with
#                 tipsy2hbt --scale-factor. Not real epochs.
#   w565, w580    every tenth particle of 8m-lambb (806,216), FoF3 with
#                 b = 0.565 and 0.58 in the real periodic box: groups a
#                 little over half the box wide, and one that spans it.
#   cube45, cube40  a cube of side 0.045 / 0.04 around the 79,173-particle
#                 halo of 8m-lambb, treated as a periodic box (subcube_tipsy.py),
#                 FoF3 with 8m-lambb's linking length: one bound halo that
#                 wraps around the box.
#
#   0. Nothing changed at a = 1: tipsy2hbt rebuilds the old snapshot, unbind
#      rebuilds the old dump and passes against the exact reference.
#   1. FoF3 labels and snapshots for the six inputs.
#   2. Per input: unbindspec makes the reference; six kernel forms run to the
#      end and are judged by unbindcheck at tau, the single-precision forms
#      again with the allowance; the same at a cap of CAP sources.
#   3. Deliberately broken kernels (MUTANTS/unbind_<name>): each must fail on
#      the inputs it is meant for.
#   4. Wide groups: unbind --open must fail; how much the first frame and the
#      result depend on the particle the periodic offsets are taken from.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:40:00
#SBATCH --job-name=unbindsw
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive

PT=${PT:-/ccs/home/rrao/paratreet2}
S=${S:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11}
SW=${SW:-$S/scalewide}
MUTANTS=${MUTANTS:-$SW/mutants}
OLDDUMP=${OLDDUMP:-$S/8m/snap/cap.5903809/kernel_nocap_f64.hdf5}
WRAP=${WRAP:-/lustre/orion/csc710/scratch/rrao/lazycanopy-2026-11-02/v7/gcd_wrap.sh}
TAU=0.00141
CAP=${CAP:-10000}
THREADS=${THREADS:-56}
HBIN=${HBIN:-/opt/cray/pe/hdf5/1.14.3.9/gnu/12.3/bin}
T8=$S/8m/8m-lambb.tipsy
T806=$SW/inputs/806k-lambb.tipsy
LINK8M=0.000997425          # 8m-lambb's linking length, 0.2 (V/N)^(1/3)

missing=0
for f in $T8 $S/8m/labels/8m.0.lbl $S/8m/snap/snap_0000.hdf5 $S/8m/snap/exact-double.particles.bin \
         $OLDDUMP $T806 $WRAP $PT/examples/fof3/FoF3 \
         $PT/subhalo/unbind $PT/subhalo/unbindspec $PT/subhalo/unbindcheck $PT/subhalo/tipsy2hbt \
         $PT/subhalo/subcube_tipsy.py $PT/subhalo/wideorigins.py $PT/subhalo/capdistance.py; do
  [ -s "$f" ] || { echo "### MISSING: $f"; missing=1; }
done
for m in ga_no_a ha_no_a soft_no_a no_pair_wrap no_frame_wrap; do
  [ -x "$MUTANTS/unbind_$m" ] || { echo "### MISSING: $MUTANTS/unbind_$m"; missing=1; }
done
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
export LCI_ATTR_BACKEND=ofi FI_CXI_RX_MATCH_MODE=hybrid \
       FI_MR_CACHE_MONITOR=userfaultfd PMI_MAX_KVS_ENTRIES=4194304
J=${SLURM_JOB_ID:-nojob}
OUT=$SW/run.$J
B=$OUT/bin
mkdir -p $B && cp $PT/subhalo/unbind $PT/subhalo/unbindspec $PT/subhalo/unbindcheck \
  $PT/subhalo/tipsy2hbt $PT/examples/fof3/FoF3 $PT/subhalo/subcube_tipsy.py \
  $PT/subhalo/wideorigins.py $PT/subhalo/capdistance.py $B/ || exit 1
cd $OUT || exit 1
echo "### node $(hostname) commit $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet -- subhalo || echo ' + uncommitted changes in subhalo/')"
echo "### md5: unbind $(md5sum $B/unbind | cut -c1-12) unbindspec $(md5sum $B/unbindspec | cut -c1-12) unbindcheck $(md5sum $B/unbindcheck | cut -c1-12) tipsy2hbt $(md5sum $B/tipsy2hbt | cut -c1-12) FoF3 $(md5sum $B/FoF3 | cut -c1-12)"

one() {   # a serial host program
  timeout --signal=TERM --kill-after=15 ${T:-300} \
    srun --ntasks=1 --cpus-per-task=14 --unbuffered --export=ALL "$@"
}
gpu() {   # a build of unbind on one die: binary, arguments
  ROCR_VISIBLE_DEVICES=0 timeout --signal=TERM --kill-after=15 300 \
    srun --ntasks=1 --cpus-per-task=14 --gpus-per-node=8 --unbuffered --export=ALL "$@"
}
cpu() {   # a host program on the node's cores: one thread per core
  OMP_NUM_THREADS=$THREADS OMP_PROC_BIND=spread OMP_PLACES=cores \
    timeout --signal=TERM --kill-after=15 900 \
    srun --ntasks=1 --cpus-per-task=$CPUS --unbuffered --export=ALL "$@"
}
# The job is laid out for FoF3 (8 processes x 14 hardware threads, two per
# core), so a step over all 56 cores asks for 112. Fall back to 56 hardware
# threads if that is refused.
CPUS=112
srun --ntasks=1 --cpus-per-task=$CPUS true > /dev/null 2>&1 || CPUS=56
echo "### host steps: $THREADS threads on $CPUS hardware threads"
dumpcheck() {   # no group of a dump may have 1..19 bound particles
  $HBIN/h5dump -d /Bound -b LE -o $OUT/bound.raw $1 > /dev/null &&
  $HBIN/h5dump -d /GroupIDs -b LE -o $OUT/group.raw $1 > /dev/null &&
  python3 - $OUT/bound.raw $OUT/group.raw <<'PY'
import sys
import numpy as np
b = np.fromfile(sys.argv[1], dtype=np.uint8)
g = np.fromfile(sys.argv[2], dtype="<i8")
bound = np.bincount(g, weights=b).astype(np.int64)
partial = int(((bound > 0) & (bound < 20)).sum())
print(f"DUMPCHECK with a sub-halo {int((bound >= 20).sum())} bound {int(bound.sum())} "
      f"groups with 1..19 bound {partial} {'ok' if partial == 0 and (b <= 1).all() else 'FAIL'}")
PY
}
kernel() {   # tag, unbind options... : one form run to the end on $SNAP, dump checked
  local tag=$1; shift
  gpu $B/unbind $SNAP --bound-fraction 1 --dump $OUT/kernel_$tag.hdf5 "$@" > $OUT/unbind_$tag.log 2>&1
  echo "  unbind $tag ($*): exit $?"
  grep -E '^unbind: cap|^groups with a self-bound|^bound particles|^unbinding passes|^groups where|^pair terms|^capped sampling|^time:' $OUT/unbind_$tag.log | sed 's/^/    /'
  dumpcheck $OUT/kernel_$tag.hdf5 | sed 's/^/    /'
}
check() {   # tag, reference, extra unbindcheck options... Sets RC.
  local tag=$1 ref=$2; shift 2
  cpu $B/unbindcheck $SNAP $ref $OUT/kernel_$tag.hdf5 --tau $TAU "$@" > $OUT/unbindcheck_$tag.log 2>&1
  RC=$?
  echo "  unbindcheck $tag at tau: exit $RC (0 = pass, 1 = fail, 2 = input error)"
  sed 's/^/    /' $OUT/unbindcheck_$tag.log
}
allowance() {   # tag, reference, tag of the double-precision run, its exit status, options...
  local tag=$1 ref=$2 dbl=$3 dblrc=$4; shift 4
  if [ "$dblrc" != 0 ]; then
    echo "  allowance not applied to $tag: the double-precision kernel $dbl did not pass at tau"
    return
  fi
  cpu $B/unbindcheck $SNAP $ref $OUT/kernel_$tag.hdf5 --tau $TAU \
      --single-against $OUT/kernel_$dbl.hdf5 "$@" > $OUT/unbindcheck_allowance_$tag.log 2>&1
  echo "  unbindcheck $tag with the allowance against $dbl: exit $?"
  grep -E 'single-precision allowance|tolerance =|^PASS|^FAIL' $OUT/unbindcheck_allowance_$tag.log | sed 's/^/    /'
}
# FoF3 (CPU arm, periodic, -K -m 20 -L): tipsy, b, period, label prefix, log, processes
fof3_try() {
  local tipsy=$1 b=$2 period=$3 prefix=$4 log=$5 np=$6 map
  case $np in 8) map=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63 ;; 2) map=1-7,9-15 ;; *) map=1-7 ;; esac
  rm -f $prefix.*.lbl
  ( cd $PT/examples/fof3 && PARATREET_DEVICE_TREE=0 timeout --signal=TERM --kill-after=15 240 \
    srun --mpi=cray_shasta --network=single_node_vni --unbuffered --cpu-bind=none \
      --distribution=block:block --ntasks=$np --gpus-per-node=8 --export=ALL \
      $WRAP $B/FoF3 -f $tipsy -d oct -u dist -b $b -c stats -l 12 -i 1 -K -m 20 -P $period \
      -L $prefix +ppn 7 +pemap $map +lci_ndevices 7 +backend_poll_thread 1 > $log 2>&1 )
  local rc=$?
  grep -q 'FOF3STAT label_files' $log && [ $rc = 0 ]
}
fof3() {   # name, tipsy, b, period, first choice of process count
  local name=$1 tipsy=$2 b=$3 period=$4 np=$5 ok=0
  mkdir -p $OUT/$name/labels
  for n in $np 1; do
    if fof3_try $tipsy $b $period $OUT/$name/labels/l $OUT/fof3_$name.log $n; then ok=1; break; fi
    echo "  FoF3 $name with $n processes failed:"; grep -m2 -E 'Fatal|Abort|abort|rror' $OUT/fof3_$name.log | sed 's/^/    /'
  done
  echo "  FoF3 $name (b $b, period $period, $n processes): $([ $ok = 1 ] && echo ok || echo FAILED)"
  grep -E 'linking_length_exact|label_digest|FOF3STAT catalog|label_files' $OUT/fof3_$name.log | cut -c1-200 | sed 's/^/    /'
  [ $ok = 1 ]
}
snapshot() {   # name, tipsy, label prefix, tipsy2hbt options...
  local name=$1 tipsy=$2 lbl=$3; shift 3
  mkdir -p $OUT/$name
  T=300 one $B/tipsy2hbt -i $tipsy -l $lbl -o $OUT/$name/snap_0000.hdf5 "$@" > $OUT/tipsy2hbt_$name.log 2>&1
  echo "  tipsy2hbt $name ($*): exit $?"; sed 's/^/    /' $OUT/tipsy2hbt_$name.log
}
# Everything for one input: reference by the definition, six kernel forms,
# and the cap.
run_case() {   # name
  local c=$1
  SNAP=$OUT/$c/snap_0000.hdf5
  echo "=== 2. $c"
  [ -s $SNAP ] || { echo "  no snapshot: skipped"; return; }
  cpu $B/unbindspec $SNAP $OUT/spec_$c.particles.bin > $OUT/unbindspec_$c.log 2>&1
  echo "  unbindspec: exit $?"; sed 's/^/    /' $OUT/unbindspec_$c.log
  local ref=$OUT/spec_$c.particles.bin F64 F64R
  kernel ${c}_f64;                             check ${c}_f64 $ref;        F64=$RC
  grep -E '^unbind: (box|groups at least|[0-9]+ particles)' $OUT/unbind_${c}_f64.log | sed 's/^/  /'
  kernel ${c}_f64_rsqrt --kernel f64 --rsqrt;  check ${c}_f64_rsqrt $ref;  F64R=$RC
  for k in f32 f32f32; do
    kernel ${c}_$k --kernel $k;                  check ${c}_$k $ref
    allowance ${c}_$k $ref ${c}_f64 $F64
    kernel ${c}_${k}_rsqrt --kernel $k --rsqrt;  check ${c}_${k}_rsqrt $ref
    allowance ${c}_${k}_rsqrt $ref ${c}_f64_rsqrt $F64R
  done
  echo "--- $c at a cap of $CAP sources"
  cpu $B/unbindspec $SNAP $OUT/spec_${c}_cap.particles.bin --cap $CAP > $OUT/unbindspec_${c}_cap.log 2>&1
  echo "  unbindspec --cap $CAP: exit $?"; sed 's/^/    /' $OUT/unbindspec_${c}_cap.log
  ref=$OUT/spec_${c}_cap.particles.bin
  kernel ${c}_cap_f64 --cap $CAP;                             check ${c}_cap_f64 $ref --cap $CAP;        F64=$RC
  kernel ${c}_cap_f64_rsqrt --cap $CAP --kernel f64 --rsqrt;  check ${c}_cap_f64_rsqrt $ref --cap $CAP;  F64R=$RC
  kernel ${c}_cap_f32_rsqrt --cap $CAP --kernel f32 --rsqrt;  check ${c}_cap_f32_rsqrt $ref --cap $CAP
  allowance ${c}_cap_f32_rsqrt $ref ${c}_cap_f64_rsqrt $F64R --cap $CAP
}
mutant() {   # mutant name, input, expected unbindcheck exit
  local m=$1 c=$2 want=$3 rc
  SNAP=$OUT/$c/snap_0000.hdf5
  [ -s $SNAP ] || { echo "  $m on $c: no snapshot, skipped"; return; }
  gpu $MUTANTS/unbind_$m $SNAP --bound-fraction 1 --dump $OUT/mutant_${m}_$c.hdf5 > $OUT/mutant_${m}_$c.log 2>&1
  cpu $B/unbindcheck $SNAP $OUT/spec_$c.particles.bin $OUT/mutant_${m}_$c.hdf5 --tau $TAU \
    > $OUT/mutantcheck_${m}_$c.log 2>&1
  rc=$?
  echo "  $m on $c: unbindcheck exit $rc (expected $want) $([ $rc = $want ] && echo ok || echo UNEXPECTED)"
  grep -E '^bound in both|^groups: |^1\. |^2\. |^3\. ' $OUT/mutantcheck_${m}_$c.log | cut -c1-200 | sed 's/^/    /'
}

echo "=== 0. nothing changed at a = 1"
snapshot regress $T8 $S/8m/labels/8m --box-mpc 100 --h 0.7
$HBIN/h5diff $S/8m/snap/snap_0000.hdf5 $OUT/regress/snap_0000.hdf5 > $OUT/h5diff_regress_snap.log 2>&1
echo "  rebuilt tipsy2hbt against the snapshot of job 5902394, h5diff: exit $? (0 = identical)"
head -5 $OUT/h5diff_regress_snap.log | sed 's/^/    /'
SNAP=$S/8m/snap/snap_0000.hdf5
kernel regress_f64
$HBIN/h5diff $OLDDUMP $OUT/kernel_regress_f64.hdf5 > $OUT/h5diff_regress_dump.log 2>&1
echo "  rebuilt unbind against the dump of job 5903809, h5diff: exit $? (0 = identical)"
head -5 $OUT/h5diff_regress_dump.log | sed 's/^/    /'
check regress_f64 $S/8m/snap/exact-double.particles.bin
rm -f $OUT/regress/snap_0000.hdf5

echo "=== 1. inputs"
snapshot a0.5 $T8 $S/8m/labels/8m --box-mpc 100 --h 0.7 --scale-factor 0.5
snapshot a1.25 $T8 $S/8m/labels/8m --box-mpc 100 --h 0.7 --scale-factor 1.25
fof3 w565 $T806 0.565 1 8 && snapshot w565 $T806 $OUT/w565/labels/l --box-mpc 100 --h 0.7
fof3 w580 $T806 0.58 1 8 && snapshot w580 $T806 $OUT/w580/labels/l --box-mpc 100 --h 0.7
for side in 0.045 0.04; do
  c=cube$(python3 -c "print(round($side * 1000))")
  mkdir -p $OUT/$c
  python3 $B/subcube_tipsy.py $T8 $OUT/$c/cube.tipsy -0.25730261 0.17688775 0.35168427 $side | sed 's/^/  /'
  # b that gives 8m-lambb's linking length in this cube: FoF3 takes b (V/N)^(1/3)
  bcube=$(python3 - $OUT/$c/cube.tipsy $side $LINK8M <<'PY'
import sys, struct
n = struct.unpack(">i", open(sys.argv[1], "rb").read(12)[8:12])[0]
print(f"{float(sys.argv[3]) / (float(sys.argv[2]) ** 3 / n) ** (1 / 3):.6f}")
PY
)
  fof3 $c $OUT/$c/cube.tipsy $bcube $side 2 &&
    snapshot $c $OUT/$c/cube.tipsy $OUT/$c/labels/l --box-mpc $(python3 -c "print($side * 100)") --h 0.7 --period $side
done

for c in a0.5 a1.25 w565 w580 cube45 cube40; do run_case $c; done

echo "=== 3. deliberately broken kernels: each must fail where it is expected to"
mutant ga_no_a a0.5 1;    mutant ga_no_a a1.25 1
mutant ha_no_a a0.5 1;    mutant ha_no_a a1.25 1
mutant soft_no_a a1.25 1
echo "  (soft_no_a on a0.5 must pass: below a = 1 the softening is the comoving one)"
mutant soft_no_a a0.5 0
for c in w565 w580 cube45 cube40; do mutant no_pair_wrap $c 1; mutant no_frame_wrap $c 1; done

echo "=== 4. wide groups"
for c in w565 w580 cube45 cube40; do
  SNAP=$OUT/$c/snap_0000.hdf5
  [ -s $SNAP ] || continue
  echo "--- $c: the periodic box ignored (unbind --open), must fail"
  kernel ${c}_open --open
  cpu $B/unbindcheck $SNAP $OUT/spec_$c.particles.bin $OUT/kernel_${c}_open.hdf5 --tau $TAU > $OUT/unbindcheck_${c}_open.log 2>&1
  echo "  unbindcheck ${c}_open: exit $? (expected 1)"
  grep -E '^bound in both|^groups: |^1\. |^2\. |^3\. ' $OUT/unbindcheck_${c}_open.log | cut -c1-200 | sed 's/^/    /'
  echo "--- $c: the particle the first frame's offsets are taken from"
  H5DUMP=$HBIN/h5dump python3 $B/wideorigins.py list $SNAP --write $OUT/origin_$c | sed 's/^/  /'
  for o in highest_id low_end high_end middle random1 random2; do
    gpu $B/unbind $SNAP --bound-fraction 1 --first-origin $OUT/origin_$c.$o.txt \
      --dump $OUT/kernel_${c}_origin_$o.hdf5 > $OUT/unbind_${c}_origin_$o.log 2>&1
    echo "  origin $o: unbind exit $?; against the lowest-id origin:"
    H5DUMP=$HBIN/h5dump python3 $B/wideorigins.py compare $SNAP $OUT/kernel_${c}_f64.hdf5 \
      $OUT/kernel_${c}_origin_$o.hdf5 | sed 's/^/    /'
  done
done
sacct -j $J -X -n -o state,elapsed
