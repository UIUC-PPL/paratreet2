#!/bin/bash
# The velocity convention before z = 0 (design/subhalo-checks.md, "The
# velocity convention before z = 0"). FoF3 -U and tipsy2hbt take peculiar
# velocity = a times the Tipsy (or NChilada) velocity. ChaNGa writes its
# internal velocity divided by a^2 (ParallelGravity.cpp, Main::writeOutput,
# dvFac = 1/a^2), which gives that convention if the internal velocity is
# a^2 dx/dt. Every output of a tracking series but the last is at a < 1, so
# this is checked on a real epoch: h34462gs.000616, a = 0.2447, a zoom run
# in a 600 Mpc box (Planck: Omega_m 0.3086, Omega_Lambda 0.6914).
#
#   CHECK_ONLY=1 bash subhalo/run_velconv.sbatch
#   sbatch subhalo/run_velconv.sbatch
#
# On one node:
#   1. subbox.py: the dark particles in the cube of side 0.02 at the box's
#      centre (inside the high-resolution region; positions unchanged)
#   2. tipsyvel.py: four copies with the velocities times a, 1, 1/a, 1/a^2,
#      so that FoF3's factor a makes the effective factor a^2, a (the
#      convention), 1 and 1/a
#   3. FoF3 -b 0.2 -K -m 20 -U on each, at a = 0.2447 with the run's
#      cosmology, the high-resolution softening and the references' G. The
#      right factor gives bound shares below 1 that rise with group size; a
#      factor too small binds nearly everything, one too large almost nothing
#   4. on the convention's copy: tipsy2hbt, HBT-HERONS with its defaults,
#      hbtref and sharecmp.py, FoF3 against HBT-HERONS group by group at
#      this epoch

#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:45:00
#SBATCH --job-name=velconv
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/velconv.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/velconv.%j.err

PT=${PT:-/ccs/home/rrao/paratreet2}
S=${S:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11}
HBTDIR=${HBTDIR:-/lustre/orion/csc710/scratch/rrao/hbt-herons-2026-11-11}
WRAP=${WRAP:-/lustre/orion/csc710/scratch/rrao/lazycanopy-2026-11-02/v7/gcd_wrap.sh}
SRC=/lustre/orion/csc710/proj-shared/h34462gs.000616
A=0.2446784278961141
OM=0.3086
OL=0.6914
EPS=5.859281486664258e-07     # the high-resolution particles' softening in the files
GEFF=0.9999517944622952
SIDE=0.02
FACT=$(python3 -c "a=$A; print(' '.join('%.17g' % f for f in (a, 1.0, 1/a, 1/a**2)))")
NAMES="eff_a2 eff_a eff_1 eff_1_over_a"   # the effective factor in each copy

missing=0
for f in $SRC/dark/pos $SRC/dark/vel $SRC/dark/mass $SRC/dark/soft $WRAP $PT/examples/fof3/FoF3 \
         $PT/subhalo/tipsy2hbt $PT/subhalo/hbtref $PT/subhalo/subbox.py $PT/subhalo/tipsyvel.py \
         $PT/subhalo/sharecmp.py $PT/subhalo/hbt_production.conf $HBTDIR/build-real8/HBT; do
  [ -s $f ] || { echo "### MISSING: $f"; missing=1; }
done
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run (factors $FACT)"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
ulimit -c 0
J=${SLURM_JOB_ID:-nojob}
OUT=$S/velconv.$J
B=$OUT/bin
mkdir -p $B $OUT/lbl $OUT/snap $OUT/hbt && cp $PT/examples/fof3/FoF3 $PT/subhalo/tipsy2hbt $PT/subhalo/hbtref \
  $PT/subhalo/subbox.py $PT/subhalo/tipsyvel.py $PT/subhalo/sharecmp.py $HBTDIR/build-real8/HBT $B/ || exit 1
echo "### paratreet2 $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet -- subhalo examples fof || echo ' + uncommitted changes'); md5 FoF3 $(md5sum $B/FoF3 | cut -c1-12) HBT $(md5sum $B/HBT | cut -c1-12)"

MAP=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
fof3() {   # log, FoF3 options...
  local log=$1; shift
  ( cd $PT/examples/fof3 && PARATREET_DEVICE_TREE=0 timeout --signal=TERM --kill-after=15 600 \
    srun --mpi=cray_shasta --network=single_node_vni --unbuffered --cpu-bind=none \
      --distribution=block:block --ntasks=8 --gpus-per-node=8 --export=ALL \
      $WRAP $B/FoF3 -d oct -u dist -c stats -l 12 -i 1 "$@" \
      +ppn 7 +pemap $MAP +lci_ndevices 7 +backend_poll_thread 1 > $log 2>&1 )
}
one() {
  timeout --signal=TERM --kill-after=15 ${T:-900} \
    srun --ntasks=1 --cpus-per-task=14 --unbuffered --export=ALL "$@"
}

echo "=== h34462 dark particles, cube of side $SIDE at the centre ($(date +%T))"
python3 -I $B/subbox.py $SRC $OUT/cube.tipsy $SIDE --families dark --workers 48 > $OUT/subbox.log 2>&1
rc=$?; sed 's/^/  /' $OUT/subbox.log; [ $rc = 0 ] || exit 1
python3 -I $B/tipsyvel.py $OUT/cube.tipsy $OUT/cube $FACT | sed 's/^/  /'

i=0
for name in $NAMES; do
  extra=""
  [ $name = eff_a ] && extra="-L $OUT/lbl/l"
  setting="eps=$EPS,a=$A,om=$OM,ol=$OL,G=$GEFF"
  [ $name = eff_a ] && setting="$setting,out=$OUT/u"
  fof3 $OUT/fof3_$name.log -f $OUT/cube.$i.tipsy -b 0.2 -K -m 20 $extra -U "$setting"
  rc=$?
  echo "=== FoF3 $name (velocities in the file times $(echo $FACT | cut -d' ' -f$((i + 1)))): exit $rc ($(date +%T))"
  grep -E 'linking_length_exact|label_digest|FOF3STAT catalog|FOF3STAT unbind:|unbind_by_size|unbind_by_family|unbind_rows' \
    $OUT/fof3_$name.log | cut -c1-3000 | sed 's/^/    /'
  [ $rc = 0 ] || grep -m6 -E 'Fatal|Abort|abort|rror' $OUT/fof3_$name.log | sed 's/^/    /'
  i=$((i + 1))
done

T=900 one $B/tipsy2hbt -i $OUT/cube.1.tipsy -l $OUT/lbl/l -o $OUT/snap/snap_0000.hdf5 --box-mpc 600 \
  --h 0.6777 --omega-m $OM --omega-l $OL --period 1 > $OUT/tipsy2hbt.log 2>&1
echo "=== tipsy2hbt: exit $? ($(date +%T))"; sed 's/^/  /' $OUT/tipsy2hbt.log
sed -e "s#SNAPDIR#$OUT/snap#" -e "s#OUTDIR#$OUT/hbt#" $PT/subhalo/hbt_production.conf > $OUT/hbt.conf
t0=$(date +%s)
OMP_NUM_THREADS=7 OMP_PLACES=cores OMP_PROC_BIND=close timeout --signal=TERM --kill-after=30 1500 \
  srun --ntasks=8 --cpus-per-task=14 --cpu-bind=cores --unbuffered --export=ALL \
    $B/HBT $OUT/hbt.conf > $OUT/hbt.log 2>&1
rc=$?
echo "=== HBT-HERONS: exit $rc, $(( $(date +%s) - t0 )) s"
[ $rc = 0 ] || { tail -8 $OUT/hbt.log | sed 's/^/  /'; exit 1; }
grep -i -E 'scale factor|ScaleFactor|redshift' $OUT/hbt.log | head -3 | sed 's/^/  /'
T=600 one $B/hbtref $OUT/hbt/000 $OUT/hbt $OUT/snap/snap_0000.hdf5 > $OUT/hbtref.log 2>&1
echo "=== hbtref: exit $?"; sed 's/^/  /' $OUT/hbtref.log | head -4
python3 -I $B/sharecmp.py $OUT/u $OUT/hbt.fof3.particles.bin > $OUT/sharecmp.log 2>&1
echo "=== sharecmp: exit $?"; sed 's/^/  /' $OUT/sharecmp.log
echo "=== done $(date +%T)"
sacct -j $J -X -n -o elapsed,nnodes,state
