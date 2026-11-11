#!/bin/bash
# unbind against the HBT-HERONS references on the 8M lambb sub-sample
# (design/subhalo-checks.md, "Comparing `unbind` with the references").
# One node: unbind on one MI250X die, unbindcheck on the node's cores.
# Six kernel forms: the two double-precision ones are held to tau; each
# single-precision one is checked at tau and again with the allowance
# measured against the double-precision run of the same form
# (unbindcheck --single-against).
#
#   make -C subhalo all compare
#   SNAPDIR=<directory holding snap_0000.hdf5> sbatch subhalo/run_unbind_compare.sbatch
#
# SNAPDIR must also hold, copied from the Mac and not regenerated:
#   hbt_reference.txt            per-group table of the HBT-HERONS rerun
#   exact-double.particles.bin   the exact reference, snapshot numbering
#                                (not exact-double.fof3.particles.bin)
# Everything the job writes goes to SNAPDIR/compare.<jobid>/.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:15:00
#SBATCH --job-name=unbindcmp
#SBATCH --nodes=1
#SBATCH --exclusive
#SBATCH --gpus-per-node=8

PT=${PT:-/ccs/home/rrao/paratreet2}
SNAPDIR=${SNAPDIR:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/8m/snap}
TAU=0.00141
THREADS=${THREADS:-56}
SNAP=$SNAPDIR/snap_0000.hdf5
REF_TABLE=$SNAPDIR/hbt_reference.txt
REF_EXACT=$SNAPDIR/exact-double.particles.bin
H5DUMP=${H5DUMP:-/opt/cray/pe/hdf5/1.14.3.9/gnu/12.3/bin/h5dump}

missing=0
for f in "$SNAP" "$REF_TABLE" "$REF_EXACT" $PT/subhalo/unbind $PT/subhalo/unbindcheck; do
  [ -s "$f" ] || { echo "### MISSING: $f"; missing=1; }
done
# The exact reference must be in the snapshot's numbering: group ids 1..G.
if [ -s "$REF_EXACT" ]; then
  python3 - "$REF_EXACT" <<'PY' || missing=1
import sys
import numpy as np
r = np.fromfile(sys.argv[1], dtype=[("id", "<i8"), ("g", "<i8"), ("e", "<f8")])
groups = np.unique(r["g"]).size
print(f"### exact reference: {len(r)} records, {groups} groups, group ids "
      f"{r['g'].min()}..{r['g'].max()}, particle ids {r['id'].min()}..{r['id'].max()}")
if r["g"].max() > 10 * groups:
    print("### WRONG FILE: group ids are not 1..G; this is FoF3's numbering")
    sys.exit(1)
PY
fi
[ $missing = 0 ] || { echo "### ABORT: inputs missing or wrong"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
J=${SLURM_JOB_ID:-nojob}
OUT=$SNAPDIR/compare.$J
mkdir -p $OUT && cp $PT/subhalo/unbind $PT/subhalo/unbindcheck $OUT/ || exit 1
cd $SNAPDIR || exit 1
echo "### node $(hostname) commit $(git -C $PT rev-parse --short HEAD) unbind md5 $(md5sum $OUT/unbind | cut -c1-12) unbindcheck md5 $(md5sum $OUT/unbindcheck | cut -c1-12)"
echo "### md5: table $(md5sum $REF_TABLE | cut -c1-12) exact $(md5sum $REF_EXACT | cut -c1-12) snapshot $(md5sum $SNAP | cut -c1-12)"

gpu() {   # unbind on one die
  ROCR_VISIBLE_DEVICES=0 timeout --signal=TERM --kill-after=15 240 \
    srun --ntasks=1 --cpus-per-task=14 --gpus-per-node=8 --unbuffered --export=ALL \
      $OUT/unbind "$@"
}
cpu() {   # unbindcheck on the node's cores
  OMP_NUM_THREADS=$THREADS OMP_PROC_BIND=spread OMP_PLACES=cores \
    timeout --signal=TERM --kill-after=15 420 \
    srun --ntasks=1 --cpus-per-task=$THREADS --unbuffered --export=ALL \
      $OUT/unbindcheck "$@"
}
# Groups in a dump by bound count: none may have 1..19 bound particles.
dumpcheck() {
  $H5DUMP -d /Bound -b LE -o $OUT/bound.raw $1 > /dev/null &&
  $H5DUMP -d /GroupIDs -b LE -o $OUT/group.raw $1 > /dev/null &&
  python3 - $OUT/bound.raw $OUT/group.raw <<'PY'
import sys
import numpy as np
b = np.fromfile(sys.argv[1], dtype=np.uint8)
g = np.fromfile(sys.argv[2], dtype="<i8")
assert len(b) == len(g), (len(b), len(g))
total = np.bincount(g)
bound = np.bincount(g, weights=b).astype(np.int64)
have = total > 0
partial = int(((bound > 0) & (bound < 20)).sum())
print(f"DUMPCHECK particles {len(b)} groups {int(have.sum())} with a sub-halo "
      f"{int((bound >= 20).sum())} bound {int(bound.sum())} groups with 1..19 bound "
      f"{partial} (must be 0) values other than 0/1 {int((b > 1).sum())} "
      f"{'ok' if partial == 0 and (b <= 1).all() else 'FAIL'}")
PY
  rm -f $OUT/bound.raw $OUT/group.raw
}

echo "=== per group, both sides stopping early at 0.995"
gpu $SNAP --ref $REF_TABLE --float-positions > $OUT/pergroup.log 2>&1
echo "  exit $? (3 = some group differs)"
sed -n '/^groups with a self-bound/,$p' $OUT/pergroup.log

# One kernel form run to the end, with its dump checked.
run_kernel() {   # name, unbind options...
  local name=$1; shift
  echo "=== per particle, kernel $name ($*)"
  gpu $SNAP --bound-fraction 1 --dump $OUT/kernel_$name.hdf5 "$@" > $OUT/unbind_$name.log 2>&1
  echo "  unbind exit $?"
  grep -E '^unbind: kernel|^groups with a self-bound|^bound particles|^unbinding passes|^groups where|^pair terms|^time:|^rate:' $OUT/unbind_$name.log | sed 's/^/  /'
  dumpcheck $OUT/kernel_$name.hdf5 | sed 's/^/  /'
}
# The dump against the exact reference at tau. Sets RC.
at_tau() {   # name
  cpu $SNAP $REF_EXACT $OUT/kernel_$1.hdf5 --tau $TAU > $OUT/unbindcheck_$1.log 2>&1
  RC=$?
  echo "  unbindcheck at tau: exit $RC (0 = pass, 1 = fail, 2 = input error)"
  cat $OUT/unbindcheck_$1.log
}
# A single-precision form with the allowance of "Tolerance for
# single-precision kernels": its rounding is measured against the same
# kernel in double precision, which must have passed at tau.
allowance() {   # name, name of the double-precision run, that run's exit status
  if [ "$3" != 0 ]; then
    echo "  allowance not applied: the double-precision kernel $2 did not pass at tau"
    return
  fi
  cpu $SNAP $REF_EXACT $OUT/kernel_$1.hdf5 --tau $TAU \
      --single-against $OUT/kernel_$2.hdf5 > $OUT/unbindcheck_allowance_$1.log 2>&1
  echo "  unbindcheck with the single-precision allowance, against $2: exit $?"
  cat $OUT/unbindcheck_allowance_$1.log
}

run_kernel f64;                             at_tau f64;        F64=$RC
run_kernel f64_rsqrt --kernel f64 --rsqrt;  at_tau f64_rsqrt;  F64R=$RC
for k in f32 f32f32; do
  run_kernel $k --kernel $k;                  at_tau $k;          allowance $k f64 $F64
  run_kernel ${k}_rsqrt --kernel $k --rsqrt;  at_tau ${k}_rsqrt;  allowance ${k}_rsqrt f64_rsqrt $F64R
done
sacct -j $J -X -n -o state,elapsed
