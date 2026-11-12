#!/bin/bash
# The carried passes (unbindspec --carried, --carried-kernel; design/
# subhalo-checks.md, "HBT-HERONS on the Mac") held to Frontier's all-pairs
# HBT-HERONS references on the four wide inputs ("HBT-HERONS on Frontier"),
# from the first particles HBT-HERONS printed on Frontier. CPU only.
#
# Per input (cube45, cube40, w565, w580):
#   1. unbindspec without options: the file of job 5904186, byte for byte;
#      with --first-origin: the file of job 5904769 (the other version of
#      the option), byte for byte;
#   2. --first-origin --carried against HBT-HERONS (unbindcmp --tau): does the
#      source's form reproduce HBT-HERONS with all pairs?
#   3. --first-origin --carried-kernel against HBT-HERONS, and the kernel's
#      dump (unbind --first-origin, double precision) against it
#      (unbindcheck, the carried-kernel result as reference): is the kernel
#      that rule?
#   4. unbindcheck of the kernel's dump against HBT-HERONS with the merged
#      binary: the verdict of job 5904424 or 5904769 again.
#
#   sbatch subhalo/run_carried_check.sbatch
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:20:00
#SBATCH --nodes=1
#SBATCH --job-name=carried
#SBATCH --exclusive

PT=${PT:-/ccs/home/rrao/paratreet2}
SW=${SW:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/scalewide}
KRUN=$SW/run.5904186                     # snapshots, unbindspec of job 5904186
TAU=0.00141
THREADS=56
INPUTS=${INPUTS:-"cube45 cube40 w565 w580"}
refdir() { [ $1 = w580 ] && echo $SW/hbt.5904769/w580 || echo $SW/hbt.5904424/$1; }

missing=0
for c in $INPUTS; do
  R=$(refdir $c)
  for f in $KRUN/$c/snap_0000.hdf5 $KRUN/spec_$c.particles.bin $R/hbt_origin.txt \
           $R/exact-double.particles.bin $R/kernel_${c}_hbtorigin_f64.hdf5 \
           $SW/hbt.5904769/$c/spec_hbtorigin.particles.bin; do
    [ -s $f ] || { echo "### missing: $f"; missing=1; }
  done
done
for b in unbindspec unbindcmp unbindcheck; do
  [ -x $PT/subhalo/$b ] || { echo "### missing: $PT/subhalo/$b"; missing=1; }
done
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
OUT=$SW/carried.${SLURM_JOB_ID:-nojob}
B=$OUT/bin
mkdir -p $B && cp $PT/subhalo/unbindspec $PT/subhalo/unbindcmp $PT/subhalo/unbindcheck $B/ || exit 1
echo "### node $(hostname); paratreet2 $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet -- subhalo || echo ' + uncommitted changes in subhalo/')"
echo "### md5: unbindspec $(md5sum $B/unbindspec | cut -c1-12) unbindcmp $(md5sum $B/unbindcmp | cut -c1-12) unbindcheck $(md5sum $B/unbindcheck | cut -c1-12)"

cpu() {
  OMP_NUM_THREADS=$THREADS OMP_PROC_BIND=spread OMP_PLACES=cores \
    timeout --signal=TERM --kill-after=15 600 \
    srun --nodes=1 --ntasks=1 --cpus-per-task=$THREADS --unbuffered --export=ALL "$@"
}
show() { grep -v '^srun' "$1" | sed 's/^/    /'; }

for c in $INPUTS; do
  R=$(refdir $c) snap=$KRUN/$c/snap_0000.hdf5 d=$OUT/$c
  org=$R/hbt_origin.txt hbt=$R/exact-double.particles.bin dump=$R/kernel_${c}_hbtorigin_f64.hdf5
  mkdir -p $d
  echo "=== $c (HBT-HERONS reference and first particles from $R)"

  cpu $B/unbindspec $snap $d/plain.particles.bin > $d/plain.log 2>&1
  cmp -s $d/plain.particles.bin $KRUN/spec_$c.particles.bin \
    && echo "  unbindspec without options: the file of job 5904186, byte for byte" \
    || echo "  unbindspec without options: NOT the file of job 5904186"
  cpu $B/unbindspec $snap $d/origin.particles.bin --first-origin $org > $d/origin.log 2>&1
  cmp -s $d/origin.particles.bin $SW/hbt.5904769/$c/spec_hbtorigin.particles.bin \
    && echo "  unbindspec --first-origin: the file of job 5904769, byte for byte" \
    || echo "  unbindspec --first-origin: NOT the file of job 5904769"

  for form in carried carried-kernel; do
    cpu $B/unbindspec $snap $d/$form.particles.bin --first-origin $org --$form > $d/$form.log 2>&1
    echo "  unbindspec --first-origin --$form: exit $?"; show $d/$form.log
    $B/unbindcmp $hbt $d/$form.particles.bin --tau $TAU > $d/unbindcmp_$form.log 2>&1
    echo "  unbindcmp, HBT-HERONS against --$form: exit $?"; show $d/unbindcmp_$form.log
  done

  cpu $B/unbindcheck $snap $d/carried-kernel.particles.bin $dump --tau $TAU --first-origin $org \
    > $d/unbindcheck_kernel_vs_carried_kernel.log 2>&1
  echo "  unbindcheck, the kernel's dump against --carried-kernel: exit $?"
  show $d/unbindcheck_kernel_vs_carried_kernel.log
  cpu $B/unbindcheck $snap $hbt $dump --tau $TAU --first-origin $org > $d/unbindcheck_kernel.log 2>&1
  echo "  unbindcheck, the kernel's dump against HBT-HERONS (merged binary): exit $?"
  show $d/unbindcheck_kernel.log
done
echo "=== done $(date +%T)"
