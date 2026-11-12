#!/bin/bash
# HBT-HERONS on Frontier as the reference for unbind (design/subhalo-checks.md,
# "HBT-HERONS on Frontier"). Each node runs its own list of inputs, one after
# another; for each input:
#   1. HBT-HERONS, HBT_REAL8=ON build, with the settings of
#      hbt_8m_lambb_exact.conf (no sampling, opening angle 0, full stop);
#   2. hbtref turns its output into exact-double.particles.bin;
#   3. unbindcmp: unbindspec's result against it (is the written definition
#      what HBT-HERONS computes?);
#   4. unbindcheck: the kernel's dumps against it at tau, the single-precision
#      forms again with the allowance;
#   5. for the inputs in SINGLE_LANES, the single-precision build's run, to
#      measure the reference's own rounding n there (tau = max(1e-4, 10 n)).
#      A run is skipped when the time left is shorter than its
#      double-precision run took.
# Inputs (LANES and SINGLE_LANES: one word per node, inputs separated by
# commas; "-" = none):
#   val            8m-lambb at a = 1. The Frontier build must reproduce the
#                  Mac's exact-double.particles.bin, which is not regenerated.
#   a0.5, a1.25    8m-lambb written at another scale factor (job 5904186).
#   w565, w580, cube45, cube40   groups at least half the box wide. These use
#                  the build with one added line that prints each group's
#                  first particle (FIRSTPARTICLE), and the kernel is rerun
#                  with unbind --first-origin from those lines.
#
# PRE lists wide inputs whose HBT-HERONS reference and first particles are
# already in HBTRUN (an earlier job's hbt.<jobid> directory): for those only
# the written definition is run again, with HBT-HERONS's first particles
# (unbindspec --first-origin), on the first node before its lane starts.
#
#   LANES="cube45,cube40,w565,w580" LIMIT_MIN=40 sbatch --nodes=1 --time=00:40:00 subhalo/run_hbt_compare.sbatch
#   LANES="val,a0.5 a1.25,w580" SINGLE_LANES="cube45,cube40,w565,a0.5,val a1.25,w580" \
#     PRE="cube45 cube40 w565" HBTRUN=<scalewide>/hbt.5904424 sbatch --nodes=2 subhalo/run_hbt_compare.sbatch
# (These go through the environment: their commas cannot go in --export.)
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=02:00:00
#SBATCH --job-name=hbtcmp
#SBATCH --exclusive
#SBATCH --gpus-per-node=8

PT=${PT:-/ccs/home/rrao/paratreet2}
S=${S:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11}
SW=${SW:-$S/scalewide}
KRUN=${KRUN:-$SW/run.5904186}                 # snapshots and kernel dumps
HBTDIR=${HBTDIR:-/lustre/orion/csc710/scratch/rrao/hbt-herons-2026-11-11}
LANES=${LANES:-"cube45,cube40,w565,w580"}
SINGLE_LANES=${SINGLE_LANES:-}
PRE=${PRE:-}
HBTRUN=${HBTRUN:-}
LIMIT_MIN=${LIMIT_MIN:-120}                   # the job's time limit
TAU=0.00141
THREADS=${THREADS:-56}
HBIN=${HBIN:-/opt/cray/pe/hdf5/1.14.3.9/gnu/12.3/bin}
FORMS="f64 f64_rsqrt f32 f32_rsqrt f32f32 f32f32_rsqrt"

snap_of() { [ $1 = val ] && echo $S/8m/snap/snap_0000.hdf5 || echo $KRUN/$1/snap_0000.hdf5; }
missing=0
for f in $HBTDIR/build-real8/HBT $HBTDIR/build-single/HBT $HBTDIR/build-real8-print/HBT \
         $HBTDIR/build-single-print/HBT $PT/subhalo/hbt_8m_lambb_exact.conf $PT/subhalo/hbtref \
         $PT/subhalo/unbind $PT/subhalo/unbindcmp $PT/subhalo/unbindcheck $PT/subhalo/unbindspec \
         $PT/subhalo/wideorigins.py $PT/subhalo/capdistance.py; do
  [ -s "$f" ] || { echo "### MISSING: $f"; missing=1; }
done
for c in $PRE; do
  for f in $HBTRUN/$c/exact-double.particles.bin $HBTRUN/$c/hbt_origin.txt \
           $HBTRUN/$c/kernel_${c}_hbtorigin_f64.hdf5 $KRUN/spec_$c.particles.bin; do
    [ -s "$f" ] || { echo "### MISSING: $f"; missing=1; }
  done
done
for c in ${LANES//,/ }; do
  [ $c = - ] && continue
  [ -s "$(snap_of $c)" ] || { echo "### MISSING: snapshot of $c"; missing=1; }
  if [ $c = val ]; then
    for f in $S/8m/snap/exact-double.particles.bin $S/8m/snap/cap.5903809/kernel_nocap_f64.hdf5 \
             $S/8m/snap/cap.5903809/spec_nocap.particles.bin; do
      [ -s $f ] || { echo "### MISSING: $f"; missing=1; }
    done
  else
    for f in $KRUN/spec_$c.particles.bin $KRUN/kernel_${c}_f64.hdf5; do
      [ -s $f ] || { echo "### MISSING: $f"; missing=1; }
    done
  fi
done
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
J=${SLURM_JOB_ID:-nojob}
OUT=$SW/hbt.$J
B=$OUT/bin
mkdir -p $B && cp $PT/subhalo/unbind $PT/subhalo/unbindcmp $PT/subhalo/unbindcheck $PT/subhalo/unbindspec $PT/subhalo/hbtref \
  $PT/subhalo/wideorigins.py $PT/subhalo/capdistance.py $B/ || exit 1
for b in real8 single real8-print single-print; do cp $HBTDIR/build-$b/HBT $B/HBT-$b || exit 1; done
NODES=($(scontrol show hostnames $SLURM_JOB_NODELIST))
DEADLINE=$(( $(date +%s) + LIMIT_MIN * 60 - 420 ))
echo "### nodes ${NODES[*]}; paratreet2 $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet -- subhalo || echo ' + uncommitted changes in subhalo/'); HBT-HERONS $(git -C $HBTDIR/../hbt-herons-src-2026-11-11/src rev-parse --short HEAD)"
echo "### md5: unbindspec $(md5sum $B/unbindspec | cut -c1-12) HBT real8 $(md5sum $B/HBT-real8 | cut -c1-12) single $(md5sum $B/HBT-single | cut -c1-12) real8-print $(md5sum $B/HBT-real8-print | cut -c1-12) single-print $(md5sum $B/HBT-single-print | cut -c1-12) unbind $(md5sum $B/unbind | cut -c1-12) unbindcheck $(md5sum $B/unbindcheck | cut -c1-12) hbtref $(md5sum $B/hbtref | cut -c1-12)"

# All steps of one lane run on one node ($NODE), one at a time.
gpu() {
  ROCR_VISIBLE_DEVICES=0 timeout --signal=TERM --kill-after=15 300 \
    srun --nodes=1 --nodelist=$NODE --ntasks=1 --cpus-per-task=14 --gpus-per-node=8 --unbuffered --export=ALL "$@"
}
cpu() {
  OMP_NUM_THREADS=$THREADS OMP_PROC_BIND=spread OMP_PLACES=cores \
    timeout --signal=TERM --kill-after=15 1500 \
    srun --nodes=1 --nodelist=$NODE --ntasks=1 --cpus-per-task=$THREADS --unbuffered --export=ALL "$@"
}
# HBT-HERONS: build, input, name of the run (double, single, ...). 8m-sized
# inputs: 8 processes x 7 threads; the small wide inputs: 1 x 56. A process
# whose largest group holds a tenth of its particles unbinds one group at a
# time with all its threads (subhalo_tracking.cpp:670).
hbt() {
  local build=$1 c=$2 run=$3 np=8 nt=7 left
  case $c in w*|cube*) np=1; nt=56 ;; esac
  local snap=$(snap_of $c) d=$OUT/$c
  mkdir -p $d/out-$run
  sed -e "s#^SnapshotPath .*#SnapshotPath      $(dirname $snap)#" \
      -e "s#^HaloPath .*#HaloPath          $(dirname $snap)#" \
      -e "s#^SubhaloPath .*#SubhaloPath       $d/out-$run#" \
      $PT/subhalo/hbt_8m_lambb_exact.conf > $d/hbt_$run.conf
  left=$(( DEADLINE - $(date +%s) ))
  if [ $left -lt ${NEED:-180} ]; then
    echo "  HBT-HERONS $build on $c: not started, $left s left and about ${NEED:-180} s needed"; return 1
  fi
  local t0=$(date +%s)
  OMP_NUM_THREADS=$nt OMP_PLACES=cores OMP_PROC_BIND=close timeout --signal=TERM --kill-after=30 $left \
    srun --nodes=1 --nodelist=$NODE --ntasks=$np --cpus-per-task=$nt --cpu-bind=cores --unbuffered \
      --export=ALL $B/HBT-$build $d/hbt_$run.conf > $d/hbt_$run.log 2>&1
  local rc=$?
  echo "  HBT-HERONS $build on $c ($np processes x $nt threads): exit $rc, $(( $(date +%s) - t0 )) s"
  [ $rc = 0 ] && echo $(( $(date +%s) - t0 )) > $d/seconds_$run.txt
  grep -E '^Unbinding' $d/hbt_$run.log | sed 's/^/    /'
  [ $rc = 0 ] || { tail -5 $d/hbt_$run.log | sed 's/^/    /'; return 1; }
  $B/hbtref $d/out-$run/000 $d/exact-$run > $d/hbtref_$run.log 2>&1
  echo "  hbtref: exit $?"; sed 's/^/    /' $d/hbtref_$run.log
  [ -s $d/exact-$run.particles.bin ]
}
check() {   # label, snapshot, reference, candidate, extra options...  Sets RC.
  local label=$1 snap=$2 ref=$3 cand=$4; shift 4
  cpu $B/unbindcheck $snap $ref $cand --tau $TAU "$@" > $LOGD/unbindcheck_$label.log 2>&1
  RC=$?
  echo "  unbindcheck $label: exit $RC (0 = pass, 1 = fail, 2 = input error)"
  grep -v '^srun' $LOGD/unbindcheck_$label.log | sed 's/^/    /'
}
allowance() {   # label, snapshot, reference, candidate, double-precision result, its exit, options...
  local label=$1 snap=$2 ref=$3 cand=$4 dbl=$5 dblrc=$6; shift 6
  if [ "$dblrc" != 0 ]; then
    echo "  allowance not applied to $label: the double-precision kernel did not pass at tau"; return
  fi
  cpu $B/unbindcheck $snap $ref $cand --tau $TAU --single-against $dbl "$@" > $LOGD/unbindcheck_allowance_$label.log 2>&1
  echo "  unbindcheck $label with the allowance: exit $?"
  grep -E 'single-precision allowance|tolerance =|^PASS|^FAIL' $LOGD/unbindcheck_allowance_$label.log | sed 's/^/    /'
}
# The six kernel forms against a reference: directory and prefix of the dumps.
six_forms() {   # input, snapshot, reference, dump prefix, extra unbindcheck options...
  local c=$1 snap=$2 ref=$3 pre=$4 F64 F64R; shift 4
  check ${c}_f64 $snap $ref ${pre}f64.hdf5 "$@";              F64=$RC
  check ${c}_f64_rsqrt $snap $ref ${pre}f64_rsqrt.hdf5 "$@";  F64R=$RC
  for k in f32 f32f32; do
    check ${c}_$k $snap $ref ${pre}$k.hdf5 "$@"
    allowance ${c}_$k $snap $ref ${pre}$k.hdf5 ${pre}f64.hdf5 $F64 "$@"
    check ${c}_${k}_rsqrt $snap $ref ${pre}${k}_rsqrt.hdf5 "$@"
    allowance ${c}_${k}_rsqrt $snap $ref ${pre}${k}_rsqrt.hdf5 ${pre}f64_rsqrt.hdf5 $F64R "$@"
  done
}
# The written definition with HBT-HERONS's first particles, against
# HBT-HERONS: input, snapshot, reference, first-particle file.
spec_origin() {
  local c=$1 snap=$2 ref=$3 org=$4 d=$OUT/$1
  cpu $B/unbindspec $snap $d/spec_hbtorigin.particles.bin --first-origin $org > $d/unbindspec_hbtorigin.log 2>&1
  echo "  unbindspec --first-origin: exit $?"; grep -v '^srun' $d/unbindspec_hbtorigin.log | sed 's/^/    /'
  $B/unbindcmp $ref $d/spec_hbtorigin.particles.bin --tau $TAU > $d/unbindcmp_spec_hbtorigin.log 2>&1
  echo "  unbindcmp, HBT-HERONS against unbindspec --first-origin: exit $?"
  sed 's/^/    /' $d/unbindcmp_spec_hbtorigin.log
  check ${c}_spec_hbtorigin $snap $ref $d/spec_hbtorigin.particles.bin --first-origin $org
}
# An input whose HBT-HERONS reference is in HBTRUN: the definition again.
do_pre() {
  local c=$1 snap=$(snap_of $1) h=$HBTRUN/$1 d=$OUT/$1
  LOGD=$d
  mkdir -p $d
  echo "=== $c: the written definition with HBT-HERONS's first particles (reference of $(basename $HBTRUN))"
  cpu $B/unbindspec $snap $d/spec_again.particles.bin > $d/unbindspec_again.log 2>&1
  cmp -s $d/spec_again.particles.bin $KRUN/spec_$c.particles.bin \
    && echo "  unbindspec without the option: the file of job 5904186, byte for byte" \
    || echo "  unbindspec without the option: NOT the file of job 5904186"
  spec_origin $c $snap $h/exact-double.particles.bin $h/hbt_origin.txt
  echo "  the kernel dump of that job again, unbindcheck now with --first-origin:"
  check ${c}_kernel_hbtorigin_f64 $snap $h/exact-double.particles.bin $h/kernel_${c}_hbtorigin_f64.hdf5 \
    --first-origin $h/hbt_origin.txt
}
do_case() {
  local c=$1 snap=$(snap_of $1) d=$OUT/$1 wide=0 build=real8
  case $c in w*|cube*) wide=1; build=real8-print ;; esac
  LOGD=$d
  mkdir -p $d
  echo "=== $c"
  local need=180
  [ $c = w580 ] && need=3000      # ran 1,129 s without finishing in job 5904424
  NEED=$need hbt $build $c double || { echo "  no reference for $c"; return; }
  local ref=$d/exact-double.particles.bin spec=$KRUN/spec_$c.particles.bin
  if [ $c = val ]; then
    spec=$S/8m/snap/cap.5903809/spec_nocap.particles.bin
    local mac=$S/8m/snap/exact-double.particles.bin
    cmp -s $mac $ref && echo "  the Frontier reference is byte for byte the Mac's" || echo "  the Frontier reference is not byte for byte the Mac's"
    $B/unbindcmp $mac $ref --tau $TAU > $d/unbindcmp_mac.log 2>&1
    echo "  unbindcmp, the Mac's exact reference against the Frontier build's: exit $?"
    sed 's/^/    /' $d/unbindcmp_mac.log
    check val_frontier_against_mac $snap $mac $ref
  fi
  $B/unbindcmp $ref $spec --tau $TAU > $d/unbindcmp_spec.log 2>&1
  echo "  unbindcmp, HBT-HERONS against unbindspec (the written definition): exit $?"
  sed 's/^/    /' $d/unbindcmp_spec.log
  if [ $c = val ]; then
    check val_f64 $snap $ref $S/8m/snap/cap.5903809/kernel_nocap_f64.hdf5
  elif [ $wide = 0 ]; then
    six_forms $c $snap $ref $KRUN/kernel_${c}_
  else
    echo "--- $c: HBT-HERONS's first particle of each group"
    H5DUMP=$HBIN/h5dump python3 -B $B/wideorigins.py hbt $snap $d/hbt_double.log $d/hbt_origin.txt 2>&1 | sed 's/^/  /'
    echo "--- $c: the kernel as run in job 5904186 (first frame from the lowest id)"
    check ${c}_lowest_id_f64 $snap $ref $KRUN/kernel_${c}_f64.hdf5
    if [ -s $d/hbt_origin.txt ]; then
      echo "--- $c: the kernel with HBT-HERONS's first particles (unbind --first-origin)"
      local k
      for k in $FORMS; do
        local opt=""
        case $k in f64_rsqrt) opt="--kernel f64 --rsqrt" ;; f32) opt="--kernel f32" ;; f32_rsqrt) opt="--kernel f32 --rsqrt" ;;
                   f32f32) opt="--kernel f32f32" ;; f32f32_rsqrt) opt="--kernel f32f32 --rsqrt" ;; esac
        gpu $B/unbind $snap --bound-fraction 1 --first-origin $d/hbt_origin.txt $opt \
          --dump $d/kernel_${c}_hbtorigin_$k.hdf5 > $d/unbind_hbtorigin_$k.log 2>&1
        echo "  unbind $k: exit $?"
        grep -E '^unbind: first-frame|^groups with a self-bound|^bound particles|^unbinding passes' $d/unbind_hbtorigin_$k.log | sed 's/^/    /'
      done
      six_forms ${c}_hbtorigin $snap $ref $d/kernel_${c}_hbtorigin_ --first-origin $d/hbt_origin.txt
      echo "--- $c: the written definition with HBT-HERONS's first particles"
      spec_origin $c $snap $ref $d/hbt_origin.txt
    fi
    if [ $c = cube45 ]; then
      echo "--- $c: the build without the added line"
      hbt real8 $c double-unpatched &&
        { cmp -s $ref $d/exact-double-unpatched.particles.bin && echo "  the same reference, byte for byte" || echo "  A DIFFERENT REFERENCE"; }
    fi
  fi
}
do_single() {
  local c=$1 d=$OUT/$1 build=single dbl=$OUT/$1 need=240
  case $c in w*|cube*) build=single-print ;; esac
  [ -s $dbl/exact-double.particles.bin ] || dbl=$HBTRUN/$c      # the double run of an earlier job
  [ -s $dbl/exact-double.particles.bin ] || { echo "=== $c: no double-precision reference, single run skipped"; return; }
  [ -s $dbl/seconds_double.txt ] && need=$(( $(cat $dbl/seconds_double.txt) * 23 / 20 ))
  mkdir -p $d
  echo "=== $c: the single-precision build (measures n)"
  NEED=$need hbt $build $c single || return
  $B/unbindcmp $dbl/exact-double.particles.bin $d/exact-single.particles.bin > $d/unbindcmp_single.log 2>&1
  echo "  unbindcmp, double against single, no tau: exit $?"; sed 's/^/    /' $d/unbindcmp_single.log
  case $c in w*|cube*) H5DUMP=$HBIN/h5dump python3 -B $B/wideorigins.py hbt $(snap_of $c) $d/hbt_single.log $d/hbt_origin_single.txt 2>&1 | sed 's/^/  /'
             cmp -s $dbl/hbt_origin.txt $d/hbt_origin_single.txt && echo "  the same first particles as the double-precision run" || echo "  first particles differ from the double-precision run" ;; esac
}

SL=($SINGLE_LANES)
i=0
for lane in $LANES; do
  ( NODE=${NODES[$i]}
    [ $i = 0 ] && for c in $PRE; do do_pre $c; done
    for c in ${lane//,/ }; do [ $c = - ] || do_case $c; done
    singles=${SL[$i]:-}
    for c in ${singles//,/ }; do [ $c = - ] || do_single $c; done
    echo "=== lane done at $(date +%H:%M:%S)"
  ) > $OUT/lane_$i.log 2>&1 &
  i=$((i + 1))
done
wait
for f in $OUT/lane_*.log; do echo "######## $f on ${NODES[$(basename $f .log | cut -d_ -f2)]}"; cat $f; done
sacct -j $J -X -n -o state,elapsed
