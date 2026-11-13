#!/bin/bash
# The centre refinement with the split kernel (unbind_kernel.h,
# potentialSplit; unbind --refine-chunk S, default 1,024): each candidate's
# sum over its group's bound particles is cut into chunks of S sources, one
# thread per chunk, and the chunk sums are added in a fixed order. Job
# 5916562 (run_unbind_refine.sbatch) ran the kernel with one thread per
# candidate, whose time followed the largest refined group
# (design/subhalo-checks.md, "Results on Frontier (job 5916562)"). One node:
# unbind on one MI250X die; unbindcheck and centrecheck.py on 56 cores.
#
#   make -C subhalo all compare
#   CHECK_ONLY=1 bash subhalo/run_unbind_refine_split.sbatch    # inputs only
#   sbatch subhalo/run_unbind_refine_split.sbatch
#
# 0. unbind_rate --no-rate: the kernel self-check, now with the split form at
#    chunks of 1, 7, 64 and 5,000, and the unbinding self-check.
# 1. Nothing else changed: with --refine-chunk 0 (one thread per candidate)
#    unbind writes job 5916562's dumps byte for byte (h5diff of the whole
#    file): 8m-lambb at caps 1,000 and 10,000 in all six forms, the other six
#    inputs in f64; and without --refine, f64 on all eight.
# 2. The split kernel, chunks of 1,024, on job 5916562's 36 runs (8m-lambb
#    at caps 1,000 and 10,000 in six forms; a0.5, a1.25, w565, w580, cube45,
#    cube40 at 10,000 in f64, f64 rsqrt, f32 rsqrt, f32f32), against job
#    5916562's references (unbindspec --cap C --refine 1000; unbindspec is
#    unchanged): unbindcheck --cap and centrecheck.py at tau, the
#    single-precision forms also with --single-against this kernel's double
#    form. Bound, Energy and MostBound must equal job 5916562's dump of the
#    same form (h5diff); RefinedEnergy changes only by rounding.
# 3. Time: chunks of 0 (one thread per candidate), 256, 512, 1,024, 2,048
#    and 4,096, f64 and f32 rsqrt, on 8m-lambb at both caps, a1.25, w565 and
#    cube40 at 10,000.
# 4. Three broken split kernels (built by split-mutants/build.sh on scratch):
#    the last chunk dropped, chunks overlapping by one source, the chunk
#    sums read back transposed. Each on 8m-lambb at 1,000 and cube40 at
#    10,000 in f64; centrecheck.py must FAIL every one.
#
# Expected: 0 PASS; 1 identical everywhere; 2 every exit 0 and Bound,
# Energy, MostBound identical, refined |dE|/S as in job 5916562 (f64 about
# 1e-9); 3 the refinement's time much shorter than with one thread per
# candidate and no longer the same on 8m-lambb at both caps; 4 six FAILs.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:40:00
#SBATCH --job-name=refsplit
#SBATCH --nodes=1
#SBATCH --exclusive
#SBATCH --gpus-per-node=8

PT=${PT:-/ccs/home/rrao/paratreet2}
S=${S:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11}
KRUN=$S/scalewide/run.5904186
OLD=$S/refine.5916562                      # the one-thread-per-candidate run
MUT=$S/split-mutants
K=1000
CHUNK=1024
TAU=0.00141
THREADS=${THREADS:-56}
HBIN=${HBIN:-/opt/cray/pe/hdf5/1.14.3.9/gnu/12.3/bin}
OTHERS="a0.5 a1.25 w565 w580 cube45 cube40"
FORMS8="f64 f64_rsqrt f32 f32_rsqrt f32f32 f32f32_rsqrt"
FORMSX="f64 f64_rsqrt f32_rsqrt f32f32"
MUTANTS="last_chunk_dropped chunks_overlap sums_transposed"
snapof() { [ $1 = 8m-lambb ] && echo $S/8m/snap/snap_0000.hdf5 || echo $KRUN/$1/snap_0000.hdf5; }
formsof() { [ $1 = 8m-lambb ] && echo $FORMS8 || echo $FORMSX; }
capsof() { [ $1 = 8m-lambb ] && echo "1000 10000" || echo 10000; }

missing=0
for X in 8m-lambb $OTHERS; do
  [ -s $(snapof $X) ] || { echo "### MISSING: $(snapof $X)"; missing=1; }
  for C in $(capsof $X); do
    d=$OLD/$X/cap$C
    for f in spec.particles.bin spec.centre.bin kernel_f64_norefine.hdf5; do
      [ -s $d/$f ] || { echo "### MISSING: $d/$f"; missing=1; }
    done
    for form in $(formsof $X); do
      [ -s $d/kernel_$form.hdf5 ] || { echo "### MISSING: $d/kernel_$form.hdf5"; missing=1; }
    done
  done
done
for f in unbind unbind_rate unbindcheck centrecheck.py; do
  [ -s $PT/subhalo/$f ] || { echo "### MISSING: $PT/subhalo/$f"; missing=1; }
done
grep -aq 'refine-chunk must not be negative' $PT/subhalo/unbind ||
  { echo "### $PT/subhalo/unbind has no --refine-chunk: rebuild (make -C subhalo all compare)"; missing=1; }
for m in $MUTANTS; do
  [ -x $MUT/$m/unbind ] || { echo "### MISSING: $MUT/$m/unbind (run $MUT/build.sh)"; missing=1; }
done
[ -x $HBIN/h5diff ] || { echo "### MISSING: $HBIN/h5diff"; missing=1; }
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
export H5DUMP=$HBIN/h5dump
J=${SLURM_JOB_ID:-nojob}
OUT=$S/refine-split.$J
B=$OUT/bin
mkdir -p $B && cp $PT/subhalo/unbind $PT/subhalo/unbind_rate $PT/subhalo/unbindcheck \
  $PT/subhalo/centrecheck.py $B/ || exit 1
for m in $MUTANTS; do cp $MUT/$m/unbind $B/unbind_$m || exit 1; done
python3 -c 'import numpy' || { echo "### ABORT: python3 has no numpy"; exit 2; }
echo "### node $(hostname) commit $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet -- subhalo || echo ' + uncommitted changes in subhalo/')"
echo "### md5: unbind $(md5sum $B/unbind | cut -c1-12) unbind_rate $(md5sum $B/unbind_rate | cut -c1-12) unbindcheck $(md5sum $B/unbindcheck | cut -c1-12) centrecheck $(md5sum $B/centrecheck.py | cut -c1-12)"

gpu() {   # a program on one die: binary, arguments
  ROCR_VISIBLE_DEVICES=0 timeout --signal=TERM --kill-after=15 300 \
    srun --ntasks=1 --cpus-per-task=14 --gpus-per-node=8 --unbuffered --export=ALL "$@"
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
reftime() { grep 'centre refinement [0-9]' $1 | sed 's/.*refinement \([0-9.]*\) s/\1/'; }
SUMMARY=()

echo "=== 0. unbind_rate --no-rate (self-checks)"
mkdir -p $OUT/rate
gpu $B/unbind_rate --no-rate > $OUT/rate/unbind_rate.log 2>&1
rc=$?
echo "  unbind_rate: exit $rc"
grep -E '^(==|CHECK|UNBIND_RATE)|split|one thread|softening' $OUT/rate/unbind_rate.log | sed 's/^/    /'
SUMMARY+=("0 unbind_rate exit $rc")

echo "=== 1. --refine-chunk 0 against job 5916562's dumps, whole file"
for X in 8m-lambb $OTHERS; do
  for C in $(capsof $X); do
    d=$OUT/regress/$X/cap$C; o=$OLD/$X/cap$C; mkdir -p $d
    forms=$([ $X = 8m-lambb ] && echo $FORMS8 || echo f64)
    for form in $forms; do
      gpu $B/unbind $(snapof $X) --bound-fraction 1 --cap $C --refine $K --refine-chunk 0 $(opts $form) \
        --dump $d/kernel_$form.hdf5 > $d/unbind_$form.log 2>&1
      rc=$?
      $HBIN/h5diff $o/kernel_$form.hdf5 $d/kernel_$form.hdf5 > $d/h5diff_$form.log 2>&1
      same=$([ $? = 0 ] && echo identical || echo DIFFERENT)
      echo "  $X cap $C $form: unbind exit $rc; against job 5916562: $same"
      SUMMARY+=("1 $X cap $C $form chunk 0: $same")
    done
    gpu $B/unbind $(snapof $X) --bound-fraction 1 --cap $C $(opts f64) --dump $d/kernel_f64_norefine.hdf5 \
      > $d/unbind_f64_norefine.log 2>&1
    $HBIN/h5diff $o/kernel_f64_norefine.hdf5 $d/kernel_f64_norefine.hdf5 > $d/h5diff_f64_norefine.log 2>&1
    same=$([ $? = 0 ] && echo identical || echo DIFFERENT)
    echo "  $X cap $C f64 without --refine: against job 5916562: $same"
    SUMMARY+=("1 $X cap $C f64 without --refine: $same")
  done
done

echo "=== 2. the split kernel, chunks of $CHUNK, against job 5916562's references"
for X in 8m-lambb $OTHERS; do
  for C in $(capsof $X); do
    snap=$(snapof $X) d=$OUT/split/$X/cap$C o=$OLD/$X/cap$C
    ref=$o/spec.particles.bin cen=$o/spec.centre.bin
    mkdir -p $d
    for form in $(formsof $X); do
      dump=$d/kernel_$form.hdf5 dbl=$d/kernel_$(double_of $form).hdf5 rc3=- rc4=-
      gpu $B/unbind $snap --bound-fraction 1 --cap $C --refine $K --refine-chunk $CHUNK $(opts $form) \
        --dump $dump > $d/unbind_$form.log 2>&1
      echo "  $X cap $C $form: unbind exit $?; refinement $(reftime $d/unbind_$form.log) s (job 5916562: $(reftime $o/unbind_$form.log) s)"
      cpu $B/unbindcheck $snap $ref $dump --cap $C --tau $TAU > $d/unbindcheck_$form.log 2>&1; rc1=$?
      python3 $B/centrecheck.py $ref $cen $dump --tau $TAU > $d/centrecheck_$form.log 2>&1; rc2=$?
      echo "    unbindcheck exit $rc1, centrecheck exit $rc2 (0 = pass)"
      sed -n '2,4p;/^FAIL/p' $d/centrecheck_$form.log | sed 's/^/    /'
      if [ $form != f64 ] && [ $form != f64_rsqrt ]; then
        cpu $B/unbindcheck $snap $ref $dump --cap $C --tau $TAU --single-against $dbl \
          > $d/unbindcheck_allowance_$form.log 2>&1; rc3=$?
        python3 $B/centrecheck.py $ref $cen $dump --tau $TAU --single-against $dbl \
          > $d/centrecheck_allowance_$form.log 2>&1; rc4=$?
        echo "    with --single-against $(double_of $form): unbindcheck exit $rc3, centrecheck exit $rc4"
      fi
      same=yes
      for ds in /Bound /Energy /MostBound; do
        $HBIN/h5diff $o/kernel_$form.hdf5 $dump $ds $ds > /dev/null 2>&1 || same=NO
      done
      echo "    Bound, Energy and MostBound identical to job 5916562's: $same"
      SUMMARY+=("2 $X cap $C $form: unbindcheck $rc1, centrecheck $rc2, with allowance $rc3 / $rc4; same Bound, Energy, MostBound: $same")
    done
  done
done

echo "=== 3. the refinement's kernel time on one die by chunk size (s; lower is better)"
for XC in "8m-lambb 1000" "8m-lambb 10000" "a1.25 10000" "w565 10000" "cube40 10000"; do
  set -- $XC; X=$1 C=$2 d=$OUT/time/$X/cap$C; mkdir -p $d
  for form in f64 f32_rsqrt; do
    line="  $X cap $C $form:"
    for ch in 0 256 512 1024 2048 4096; do
      gpu $B/unbind $(snapof $X) --bound-fraction 1 --cap $C --refine $K --refine-chunk $ch $(opts $form) \
        > $d/unbind_${form}_$ch.log 2>&1
      line="$line chunk $ch $(reftime $d/unbind_${form}_$ch.log);"
    done
    echo "$line first pass $(grep '^time: unbinding' $d/unbind_${form}_0.log | sed 's/.*(first pass \([0-9.]*\),.*/\1/')"
  done
done

echo "=== 4. broken split kernels: centrecheck must FAIL"
for m in $MUTANTS; do
  for XC in "8m-lambb 1000" "cube40 10000"; do
    set -- $XC; X=$1 C=$2 d=$OUT/mutants/$m/$X/cap$C o=$OLD/$X/cap$C; mkdir -p $d
    gpu $B/unbind_$m $(snapof $X) --bound-fraction 1 --cap $C --refine $K --refine-chunk $CHUNK \
      $(opts f64) --dump $d/kernel_f64.hdf5 > $d/unbind_f64.log 2>&1
    python3 $B/centrecheck.py $o/spec.particles.bin $o/spec.centre.bin $d/kernel_f64.hdf5 --tau $TAU \
      > $d/centrecheck_f64.log 2>&1
    rc=$?
    echo "  $m on $X cap $C: centrecheck exit $rc ($([ $rc = 1 ] && echo caught || echo NOT CAUGHT))"
    sed -n '2,4p;$p' $d/centrecheck_f64.log | sed 's/^/    /'
    SUMMARY+=("4 $m $X cap $C: centrecheck exit $rc")
  done
done

echo "=== summary"
printf '  %s\n' "${SUMMARY[@]}"
echo "=== done $(date +%T)"
