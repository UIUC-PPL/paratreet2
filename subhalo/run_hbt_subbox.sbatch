#!/bin/bash
# cosmo25: HBT-HERONS on a sub-box (design/subhalo-checks.md, "cosmo25:
# HBT-HERONS on a sub-box"). cosmo25 gives a sub-halo to fewer of its groups
# than cosmo2b at every group size (jobs 5965868 and 5966019). Does
# HBT-HERONS, given the same groups, find the same shares?
#
#   CHECK_ONLY=1 bash subhalo/run_hbt_subbox.sbatch
#   sbatch subhalo/run_hbt_subbox.sbatch
#
# For each input (INPUTS, default "cosmo25 cosmo2b"), on one node:
#   1. subbox.py: the cube of side SIDE at the box's centre, positions
#      unchanged, families kept (cosmo25 1/8: about 48 M particles; cosmo2b
#      1/4: about 31 M)
#   2. FoF3 -b 0.2 once to read the sub-box's volume, then FoF3 with the -b
#      factor that gives the full box's linking length, open boundaries,
#      -K -m 20 -L, and -U with the production settings (cap 10,000,
#      refinement 1,000, bf 0.995) and the references' G, out= for the
#      per-particle results
#   3. tipsy2hbt (period 1, the full box's: no group in the sub-box is near
#      its faces, so HBT-HERONS's periodic wrap changes nothing)
#   4. HBT-HERONS (double precision build) with its own defaults: 1,000
#      sampled sources, tree opening angle 0.45, BoundMassPrecision 0.995
#      (subhalo/hbt_production.conf), 8 processes x 7 threads
#   5. hbtstat, hbtref, and sharecmp.py: per group and by size, FoF3 against
#      HBT-HERONS

#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=01:00:00
#SBATCH --job-name=hbtsubbox
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/hbtsubbox.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/hbtsubbox.%j.err

PT=${PT:-/ccs/home/rrao/paratreet2}
S=${S:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11}
HBTDIR=${HBTDIR:-/lustre/orion/csc710/scratch/rrao/hbt-herons-2026-11-11}
WRAP=${WRAP:-/lustre/orion/csc710/scratch/rrao/lazycanopy-2026-11-02/v7/gcd_wrap.sh}
GEFF=0.9999517944622952
INPUTS=${INPUTS:-"cosmo25 cosmo2b"}
declare -A SRC=([cosmo25]=/lustre/orion/csc710/proj-shared/cosmo25PLK.2304g_dm.004096
                [cosmo2b]=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024)
declare -A SIDE=([cosmo25]=0.125 [cosmo2b]=0.25)
declare -A EPS=([cosmo25]=4.721853201772319e-06 [cosmo2b]=1.3799999578623101e-05)
declare -A BFULL=([cosmo25]=6.8897746514307755e-05 [cosmo2b]=0.00015922463116076563)
declare -A OM=([cosmo25]=0.3086 [cosmo2b]=0.27)
declare -A HUB=([cosmo25]=0.6777 [cosmo2b]=0.7)

missing=0
for f in ${SRC[cosmo25]}/dark/pos ${SRC[cosmo2b]} $WRAP $PT/examples/fof3/FoF3 $PT/subhalo/tipsy2hbt \
         $PT/subhalo/hbtref $PT/subhalo/hbtstat $PT/subhalo/subbox.py $PT/subhalo/sharecmp.py \
         $PT/subhalo/hbt_production.conf $HBTDIR/build-real8/HBT; do
  [ -s $f ] || { echo "### MISSING: $f"; missing=1; }
done
strings $PT/examples/fof3/FoF3 | grep -q 'unbind_by_family' || { echo "### FoF3 has no unbind_by_family"; missing=1; }
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
ulimit -c 0
J=${SLURM_JOB_ID:-nojob}
OUT=$S/hbtsubbox.$J
B=$OUT/bin
mkdir -p $B && cp $PT/examples/fof3/FoF3 $PT/subhalo/tipsy2hbt $PT/subhalo/hbtref $PT/subhalo/hbtstat \
  $PT/subhalo/subbox.py $PT/subhalo/sharecmp.py $HBTDIR/build-real8/HBT $B/ || exit 1
echo "### inputs $INPUTS; paratreet2 $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet -- subhalo examples fof || echo ' + uncommitted changes'); md5 FoF3 $(md5sum $B/FoF3 | cut -c1-12) HBT $(md5sum $B/HBT | cut -c1-12)"

MAP=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
fof3() {   # log, FoF3 options...
  local log=$1; shift
  ( cd $PT/examples/fof3 && PARATREET_DEVICE_TREE=0 timeout --signal=TERM --kill-after=15 600 \
    srun --mpi=cray_shasta --network=single_node_vni --unbuffered --cpu-bind=none \
      --distribution=block:block --ntasks=8 --gpus-per-node=8 --export=ALL \
      $WRAP $B/FoF3 -d oct -u dist -c stats -l 12 -i 1 "$@" \
      +ppn 7 +pemap $MAP +lci_ndevices 7 +backend_poll_thread 1 > $log 2>&1 )
}
one() {   # a serial host program
  timeout --signal=TERM --kill-after=15 ${T:-900} \
    srun --ntasks=1 --cpus-per-task=14 --unbuffered --export=ALL "$@"
}

for c in $INPUTS; do
  d=$OUT/$c
  mkdir -p $d/lbl $d/snap $d/hbt
  echo "=== $c: sub-box of side ${SIDE[$c]} at the centre ($(date +%T))"
  python3 -I $B/subbox.py ${SRC[$c]} $d/sub.tipsy ${SIDE[$c]} --workers 48 > $d/subbox.log 2>&1
  rc=$?; sed 's/^/  /' $d/subbox.log; [ $rc = 0 ] || { echo "  subbox: exit $rc"; continue; }

  fof3 $d/fof3_volume.log -f $d/sub.tipsy -b 0.2
  line=$(grep -m1 'linking_length_exact' $d/fof3_volume.log)
  V=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i=="volume_exact:") print $(i+1)}')
  N=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i=="n:") print $(i+1)}')
  [ -n "$V" ] && [ -n "$N" ] || { echo "  FoF3 volume run failed"; tail -5 $d/fof3_volume.log | sed 's/^/  /'; continue; }
  FACTOR=$(python3 -c "print('%.17g' % (${BFULL[$c]} / (($V) / ($N)) ** (1.0 / 3.0)))")
  echo "  sub-box volume $V, $N particles: -b $FACTOR for the full box's linking length ${BFULL[$c]}"

  fof3 $d/fof3.log -f $d/sub.tipsy -b $FACTOR -K -m 20 -L $d/lbl/l \
    -U "eps=${EPS[$c]},G=$GEFF,out=$d/u"
  rc=$?
  echo "  FoF3: exit $rc ($(date +%T))"
  grep -E 'linking_length_exact|label_digest|FOF3STAT catalog|FOF3STAT unbind|label_files' $d/fof3.log \
    | grep -v unbind_settings | cut -c1-3000 | sed 's/^/    /'
  [ $rc = 0 ] || { grep -m6 -E 'Fatal|Abort|abort|rror' $d/fof3.log | sed 's/^/    /'; continue; }

  T=900 one $B/tipsy2hbt -i $d/sub.tipsy -l $d/lbl/l -o $d/snap/snap_0000.hdf5 --box-mpc 25 \
    --h ${HUB[$c]} --omega-m ${OM[$c]} --omega-l $(python3 -c "print(1 - ${OM[$c]})") --period 1 \
    > $d/tipsy2hbt.log 2>&1
  echo "  tipsy2hbt: exit $? ($(date +%T))"; sed 's/^/    /' $d/tipsy2hbt.log

  sed -e "s#SNAPDIR#$d/snap#" -e "s#OUTDIR#$d/hbt#" $PT/subhalo/hbt_production.conf > $d/hbt.conf
  t0=$(date +%s)
  OMP_NUM_THREADS=7 OMP_PLACES=cores OMP_PROC_BIND=close timeout --signal=TERM --kill-after=30 1500 \
    srun --ntasks=8 --cpus-per-task=14 --cpu-bind=cores --unbuffered --export=ALL \
      $B/HBT $d/hbt.conf > $d/hbt.log 2>&1
  rc=$?
  echo "  HBT-HERONS: exit $rc, $(( $(date +%s) - t0 )) s"
  [ $rc = 0 ] || { tail -8 $d/hbt.log | sed 's/^/    /'; continue; }

  T=600 one $B/hbtstat $d/snap/snap_0000.hdf5 $d/hbt/000 $d/hbtstat.txt > $d/hbtstat.log 2>&1
  echo "  hbtstat: exit $?"; head -6 $d/hbtstat.log | sed 's/^/    /'
  T=600 one $B/hbtref $d/hbt/000 $d/hbt $d/snap/snap_0000.hdf5 > $d/hbtref.log 2>&1
  echo "  hbtref: exit $?"; sed 's/^/    /' $d/hbtref.log | head -8
  python3 -I $B/sharecmp.py $d/u $d/hbt.fof3.particles.bin > $d/sharecmp.log 2>&1
  echo "  sharecmp: exit $?"; sed 's/^/    /' $d/sharecmp.log
done
echo "=== done $(date +%T)"
sacct -j $J -X -n -o elapsed,nnodes,state
