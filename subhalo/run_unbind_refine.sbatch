#!/bin/bash
# Centre refinement (design/subhalo-checks.md, "Centre refinement"): unbind
# --cap C --refine K against the definition, unbindspec --cap C --refine K,
# with K = 1,000. One node: unbind on one MI250X die; unbindspec,
# unbindcheck and centrecheck.py on the node's cores.
#
#   make -C subhalo all compare                          # the new commit
#   CHECK_ONLY=1 bash subhalo/run_unbind_refine.sbatch   # inputs only
#   sbatch subhalo/run_unbind_refine.sbatch
#
# Inputs: 8m-lambb at caps 1,000 and 10,000 with all six kernel forms;
# a0.5, a1.25, w565, w580, cube45 and cube40 (job 5904186's snapshots) at
# 10,000 with f64, f64_rsqrt, f32_rsqrt and f32f32. For each input and cap:
#   1. unbindspec --cap C --refine K: the reference (.particles.bin, each
#      group's most bound particle first) and its .centre.bin;
#   2. each kernel form with --bound-fraction 1 --cap C --refine K --dump;
#   3. unbindcheck --cap C at tau against the reference, and for a
#      single-precision form also with --single-against its double form
#      (f64 or f64_rsqrt). The refinement must not change a pass, so this
#      repeats the checks of jobs 5903809 and 5904186;
#   4. centrecheck.py at tau, single-precision forms also with
#      --single-against;
#   5. the f64 form again without --refine: Bound and Energy must equal the
#      f64 form's with it (h5diff of those two datasets).
# Last, the refinement's kernel time against the first pass's.
#
# Expected, from the Mac (2026-11-12: unbindspec, and unbind built against a
# host stand-in for Kokkos in the f64 and f32f32 forms, plus f32 on
# 8m-lambb):
#
#   input     cap     bound particles  refined groups  winner's rank: largest
#   8m-lambb  1000    4,214,992        439             66
#   8m-lambb  10000   4,215,879        58              6
#   a0.5      10000   4,294,867        60              23
#   a1.25     10000   2,538,527        31              11
#   w565      10000   514,744          8               5
#   w580      10000   422,173          6               1
#   cube45    10000   79,536           1               1
#   cube40    10000   79,053           1               0
#
# Every form: unbindcheck exit 0; CENTRECHECK PASS with the same most bound
# particle in every refined group; refined energies within 2e-9 of S in
# double precision, 1.3e-6 in f32, 1.3e-3 in f32f32 (a1.25; 1.1e-4 on
# 8m-lambb). The rsqrt forms were NOT tested on the Mac: the stand-in's
# rsqrt is 1 / sqrt.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:40:00
#SBATCH --job-name=unbindref
#SBATCH --nodes=1
#SBATCH --exclusive
#SBATCH --gpus-per-node=8

PT=${PT:-/ccs/home/rrao/paratreet2}
S=${S:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11}
KRUN=$S/scalewide/run.5904186
K=${K:-1000}
TAU=0.00141
THREADS=${THREADS:-56}
HBIN=${HBIN:-/opt/cray/pe/hdf5/1.14.3.9/gnu/12.3/bin}
OTHERS=${OTHERS:-"a0.5 a1.25 w565 w580 cube45 cube40"}
FORMS8="f64 f64_rsqrt f32 f32_rsqrt f32f32 f32f32_rsqrt"
FORMSX="f64 f64_rsqrt f32_rsqrt f32f32"
snapof() { [ $1 = 8m-lambb ] && echo $S/8m/snap/snap_0000.hdf5 || echo $KRUN/$1/snap_0000.hdf5; }

missing=0
for X in 8m-lambb $OTHERS; do
  [ -s $(snapof $X) ] || { echo "### MISSING: $(snapof $X)"; missing=1; }
done
for f in unbind unbindspec unbindcheck centrecheck.py; do
  [ -s $PT/subhalo/$f ] || { echo "### MISSING: $PT/subhalo/$f"; missing=1; }
done
for f in unbind unbindspec; do   # the binaries, not the sources: an old build has no --refine
  grep -aq 'refine needs a positive K' $PT/subhalo/$f 2>/dev/null ||
    { echo "### $PT/subhalo/$f has no --refine: pull and rebuild (make -C subhalo all compare)"; missing=1; }
done
[ -x $HBIN/h5dump ] && [ -x $HBIN/h5diff ] || { echo "### MISSING: $HBIN/h5dump or h5diff"; missing=1; }
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
export H5DUMP=$HBIN/h5dump
J=${SLURM_JOB_ID:-nojob}
OUT=$S/refine.$J
B=$OUT/bin
mkdir -p $B && cp $PT/subhalo/unbind $PT/subhalo/unbindspec $PT/subhalo/unbindcheck \
  $PT/subhalo/centrecheck.py $B/ || exit 1
python3 -c 'import numpy' || { echo "### ABORT: python3 has no numpy"; exit 2; }
echo "### node $(hostname) commit $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet -- subhalo || echo ' + uncommitted changes in subhalo/')"
echo "### md5: unbind $(md5sum $B/unbind | cut -c1-12) unbindspec $(md5sum $B/unbindspec | cut -c1-12) unbindcheck $(md5sum $B/unbindcheck | cut -c1-12) centrecheck $(md5sum $B/centrecheck.py | cut -c1-12)"

gpu() {   # unbind on one die
  ROCR_VISIBLE_DEVICES=0 timeout --signal=TERM --kill-after=15 300 \
    srun --ntasks=1 --cpus-per-task=14 --gpus-per-node=8 --unbuffered --export=ALL \
      $B/unbind "$@"
}
cpu() {   # a host program on the node's cores
  OMP_NUM_THREADS=$THREADS OMP_PROC_BIND=spread OMP_PLACES=cores \
    timeout --signal=TERM --kill-after=15 900 \
    srun --ntasks=1 --cpus-per-task=$THREADS --unbuffered --export=ALL "$@"
}
opts() {   # form -> unbind options
  case $1 in
    f64) echo "--kernel f64";;            f64_rsqrt) echo "--kernel f64 --rsqrt";;
    f32) echo "--kernel f32";;            f32_rsqrt) echo "--kernel f32 --rsqrt";;
    f32f32) echo "--kernel f32f32";;      f32f32_rsqrt) echo "--kernel f32f32 --rsqrt";;
  esac
}
double_of() { case $1 in *_rsqrt) echo f64_rsqrt;; *) echo f64;; esac; }
SUMMARY=()

run_input() {   # input, cap, forms...
  local X=$1 C=$2; shift 2
  local snap=$(snapof $X) d=$OUT/$X/cap$C
  mkdir -p $d
  echo "=== $X, cap $C, refine $K"
  local ref=$d/spec.particles.bin cen=$d/spec.centre.bin
  cpu $B/unbindspec $snap $ref --cap $C --refine $K > $d/unbindspec.log 2>&1
  echo "  unbindspec --cap $C --refine $K: exit $?"; grep -v '^srun' $d/unbindspec.log | sed 's/^/    /'
  for form in "$@"; do
    local dump=$d/kernel_$form.hdf5 dbl=$d/kernel_$(double_of $form).hdf5 rc1 rc2 rc3=- rc4=-
    gpu $snap --bound-fraction 1 --cap $C --refine $K $(opts $form) --dump $dump > $d/unbind_$form.log 2>&1
    echo "  unbind $form: exit $?"
    grep -E '^bound particles|^centre refinement|^time' $d/unbind_$form.log | sed 's/^/    /'
    cpu $B/unbindcheck $snap $ref $dump --cap $C --tau $TAU > $d/unbindcheck_$form.log 2>&1; rc1=$?
    python3 $B/centrecheck.py $ref $cen $dump --tau $TAU > $d/centrecheck_$form.log 2>&1; rc2=$?
    echo "  $form: unbindcheck exit $rc1, centrecheck exit $rc2 (0 = pass)"
    sed -n '2,5p;/^FAIL/p;/a tie)/p' $d/centrecheck_$form.log | sed 's/^/    /'
    if [ $form != f64 ] && [ $form != f64_rsqrt ] && [ ! -s $dbl ]; then
      echo "  $form: no $(double_of $form) dump, so no allowance run"
    elif [ $form != f64 ] && [ $form != f64_rsqrt ]; then
      cpu $B/unbindcheck $snap $ref $dump --cap $C --tau $TAU --single-against $dbl \
        > $d/unbindcheck_allowance_$form.log 2>&1; rc3=$?
      python3 $B/centrecheck.py $ref $cen $dump --tau $TAU --single-against $dbl \
        > $d/centrecheck_allowance_$form.log 2>&1; rc4=$?
      echo "  $form with --single-against $(double_of $form): unbindcheck exit $rc3, centrecheck exit $rc4"
      head -1 $d/centrecheck_allowance_$form.log | sed 's/.*; single/    single/'
    fi
    SUMMARY+=("$X cap $C $form: unbindcheck $rc1, centrecheck $rc2, with allowance $rc3 / $rc4")
  done
  gpu $snap --bound-fraction 1 --cap $C $(opts f64) --dump $d/kernel_f64_norefine.hdf5 > $d/unbind_f64_norefine.log 2>&1
  local same=0
  for ds in /Bound /Energy; do
    $HBIN/h5diff $d/kernel_f64.hdf5 $d/kernel_f64_norefine.hdf5 $ds $ds > /dev/null 2>&1 || same=1
  done
  echo "  f64 without --refine: Bound and Energy identical to f64 with it: $([ $same = 0 ] && echo yes || echo NO)"
  SUMMARY+=("$X cap $C: without --refine identical: $([ $same = 0 ] && echo yes || echo NO)")
}

run_input 8m-lambb 1000 $FORMS8
run_input 8m-lambb 10000 $FORMS8
for X in $OTHERS; do run_input $X 10000 $FORMSX; done

echo "=== the refinement's kernel time on one die, f64 and f32_rsqrt (s; lower is better)"
for f in $OUT/*/cap*/unbind_f64.log $OUT/*/cap*/unbind_f32_rsqrt.log; do
  echo "  ${f#$OUT/}: $(grep -E '^time: unbinding' $f | sed 's/.*(first pass \([0-9.]*\).*/first pass \1/'), $(grep 'centre refinement [0-9]' $f | sed 's/.*refinement //')"
done
echo "=== summary"
printf '  %s\n' "${SUMMARY[@]}"
echo "=== done $(date +%T)"
