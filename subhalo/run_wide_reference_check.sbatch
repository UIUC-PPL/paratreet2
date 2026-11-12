#!/bin/bash
# Three checks job 5910905 left out (design/subhalo-checks.md, "HBT-HERONS on
# Frontier"). CPU only. For each wide input X (cube45, cube40, w565, w580):
#
#   1. The reference without HBT-HERONS: unbindspec --carried-kernel from the
#      lowest id (no --first-origin), and the kernel dumps of job 5904186
#      (first frame from the lowest id) against it with unbindcheck: f64 and
#      f64_rsqrt at tau, the four single-precision forms at tau with
#      --single-against the f64 dump.
#   2. The five other forms from HBT-HERONS's first particles (job 5910905
#      checked f64 only): the dumps kernel_X_hbtorigin_<form>.hdf5 against
#      carried.5910905/X/carried-kernel.particles.bin, with --first-origin and
#      --single-against the f64 dump.
#   3. The kernel's own count of the stale-frame case in its logs from
#      HBT-HERONS's first particles (unbind_hbtorigin_<form>.log), and, for
#      comparison, in its logs from the lowest id (job 5904186).
#
#   CHECK_ONLY=1 bash subhalo/run_wide_reference_check.sbatch   # inputs only
#   sbatch subhalo/run_wide_reference_check.sbatch
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:20:00
#SBATCH --nodes=1
#SBATCH --job-name=widecheck
#SBATCH --exclusive

PT=${PT:-/ccs/home/rrao/paratreet2}
SW=${SW:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/scalewide}
KRUN=$SW/run.5904186                     # snapshots, lowest-id kernel dumps and logs
CARRIED=$SW/carried.5910905              # --carried-kernel from HBT-HERONS's first particles
TAU=0.00141
THREADS=56
INPUTS=${INPUTS:-"cube45 cube40 w565 w580"}
SINGLE="f32 f32_rsqrt f32f32 f32f32_rsqrt"
OTHER5="f64_rsqrt $SINGLE"
refdir() { [ $1 = w580 ] && echo $SW/hbt.5904769/w580 || echo $SW/hbt.5904424/$1; }

missing=0
for X in $INPUTS; do
  H=$(refdir $X)
  for f in $KRUN/$X/snap_0000.hdf5 $H/hbt_origin.txt $CARRIED/$X/carried-kernel.particles.bin; do
    [ -s $f ] || { echo "### missing: $f"; missing=1; }
  done
  for form in f64 $OTHER5; do
    for f in $KRUN/kernel_${X}_$form.hdf5 $KRUN/unbind_${X}_$form.log \
             $H/kernel_${X}_hbtorigin_$form.hdf5 $H/unbind_hbtorigin_$form.log; do
      [ -s $f ] || { echo "### missing: $f"; missing=1; }
    done
  done
done
for b in unbindspec unbindcheck; do
  [ -x $PT/subhalo/$b ] || { echo "### missing: $PT/subhalo/$b"; missing=1; }
done
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
OUT=$SW/widecheck.${SLURM_JOB_ID:-nojob}
B=$OUT/bin
mkdir -p $B && cp $PT/subhalo/unbindspec $PT/subhalo/unbindcheck $B/ || exit 1
echo "### node $(hostname); paratreet2 $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet -- subhalo || echo ' + uncommitted changes in subhalo/')"
echo "### md5: unbindspec $(md5sum $B/unbindspec | cut -c1-12) unbindcheck $(md5sum $B/unbindcheck | cut -c1-12)"

cpu() {
  OMP_NUM_THREADS=$THREADS OMP_PROC_BIND=spread OMP_PLACES=cores \
    timeout --signal=TERM --kill-after=15 600 \
    srun --nodes=1 --ntasks=1 --cpus-per-task=$THREADS --unbuffered --export=ALL "$@"
}
show() { grep -v '^srun' "$1" | sed 's/^/    /'; }
SUMMARY=()

for X in $INPUTS; do
  H=$(refdir $X) snap=$KRUN/$X/snap_0000.hdf5 d=$OUT/$X
  org=$H/hbt_origin.txt
  mkdir -p $d
  echo "=== $X"

  echo "--- 1. unbindspec --carried-kernel from the lowest id, and the kernel of job 5904186 against it"
  ref=$d/lowest_carried_kernel.particles.bin
  cpu $B/unbindspec $snap $ref --carried-kernel > $d/lowest_carried_kernel.log 2>&1
  echo "  unbindspec --carried-kernel: exit $?"; show $d/lowest_carried_kernel.log
  for form in f64 $OTHER5; do
    extra=""; label="$form"
    case " $SINGLE " in *" $form "*) extra="--single-against $KRUN/kernel_${X}_f64.hdf5"; label="$form with --single-against";; esac
    cpu $B/unbindcheck $snap $ref $KRUN/kernel_${X}_$form.hdf5 --tau $TAU $extra \
      > $d/check1_$form.log 2>&1
    rc=$?
    echo "  unbindcheck, kernel_${X}_$form ($label): exit $rc (0 = pass, 1 = fail, 2 = input error)"
    show $d/check1_$form.log
    SUMMARY+=("1 $X $form exit $rc")
  done

  echo "--- 2. the five other forms from HBT-HERONS's first particles against carried.5910905"
  for form in $OTHER5; do
    cpu $B/unbindcheck $snap $CARRIED/$X/carried-kernel.particles.bin $H/kernel_${X}_hbtorigin_$form.hdf5 \
      --first-origin $org --tau $TAU --single-against $H/kernel_${X}_hbtorigin_f64.hdf5 \
      > $d/check2_$form.log 2>&1
    rc=$?
    echo "  unbindcheck, kernel_${X}_hbtorigin_$form with --single-against: exit $rc"
    show $d/check2_$form.log
    SUMMARY+=("2 $X $form exit $rc")
  done

  echo "--- 3. the kernel's own count of the stale-frame case"
  for form in f64 $OTHER5; do
    a=$(grep 'stale frame' $H/unbind_hbtorigin_$form.log | awk -F': ' '{print $NF}')
    b=$(grep 'stale frame' $KRUN/unbind_${X}_$form.log | awk -F': ' '{print $NF}')
    echo "  $form: from HBT-HERONS's first particles ${a:-no line}; from the lowest id (job 5904186) ${b:-no line}"
    SUMMARY+=("3 $X $form hbt-origin ${a:-none} lowest-id ${b:-none}")
  done
done

echo "=== summary"
printf '  %s\n' "${SUMMARY[@]}"
echo "=== done $(date +%T)"
