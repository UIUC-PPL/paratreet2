#!/bin/bash
# Capped sampling: unbind --cap against the written definition, unbindspec
# --cap (design/subhalo-checks.md, "Capped sampling: definition and pass
# rule"). One node: unbind on one MI250X die, unbindspec and unbindcheck on
# the node's cores.
#
#   make -C subhalo all compare
#   SNAPDIR=<directory holding snap_0000.hdf5> sbatch subhalo/run_unbind_cap.sbatch
#
# SNAPDIR must also hold exact-double.particles.bin (snapshot numbering), for
# the runs without a cap. Everything is written to SNAPDIR/cap.<jobid>/.
#
#   A. No cap, as before: unbindspec and the double-precision kernel against
#      the exact HBT-HERONS reference.
#   B. For each cap: unbindspec --cap makes the reference; six forms of the
#      kernel are run to the end and judged by unbindcheck --cap at tau; the
#      single-precision forms again with the allowance against the
#      double-precision kernel of the same form and cap.
#   C. The two exact conditions: groups at or below the cap equal the
#      results without a cap, and a cap above the largest group changes
#      nothing.
#   D. If MUTANTS names a directory of deliberately broken kernels built for
#      the host (unbind_<name>), each is run at the first cap on the node's
#      cores and must FAIL; unbind_control there must pass.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:30:00
#SBATCH --job-name=unbindcap
#SBATCH --nodes=1
#SBATCH --exclusive
#SBATCH --gpus-per-node=8

PT=${PT:-/ccs/home/rrao/paratreet2}
SNAPDIR=${SNAPDIR:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/8m/snap}
CAPS=${CAPS:-"1000 10000 100000"}
BIGCAP=${BIGCAP:-200000}        # above the largest group
MUTANTS=${MUTANTS:-}
TAU=0.00141
THREADS=${THREADS:-56}
SNAP=$SNAPDIR/snap_0000.hdf5
REF_EXACT=$SNAPDIR/exact-double.particles.bin
HBIN=${HBIN:-/opt/cray/pe/hdf5/1.14.3.9/gnu/12.3/bin}

missing=0
for f in "$SNAP" "$REF_EXACT" $PT/subhalo/unbind $PT/subhalo/unbindcheck $PT/subhalo/unbindspec \
         $PT/subhalo/unbindcmp; do
  [ -s "$f" ] || { echo "### MISSING: $f"; missing=1; }
done
[ -z "$MUTANTS" ] || [ -x "$MUTANTS/unbind_control" ] || { echo "### MISSING: $MUTANTS/unbind_control"; missing=1; }
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
J=${SLURM_JOB_ID:-nojob}
OUT=$SNAPDIR/cap.$J
mkdir -p $OUT && cp $PT/subhalo/unbind $PT/subhalo/unbindcheck $PT/subhalo/unbindspec \
  $PT/subhalo/unbindcmp $OUT/ || exit 1
cd $SNAPDIR || exit 1
echo "### node $(hostname) commit $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet -- subhalo || echo ' + uncommitted changes in subhalo/')"
echo "### md5: unbind $(md5sum $OUT/unbind | cut -c1-12) unbindspec $(md5sum $OUT/unbindspec | cut -c1-12) unbindcheck $(md5sum $OUT/unbindcheck | cut -c1-12) snapshot $(md5sum $SNAP | cut -c1-12) exact $(md5sum $REF_EXACT | cut -c1-12)"

gpu() {   # unbind on one die
  ROCR_VISIBLE_DEVICES=0 timeout --signal=TERM --kill-after=15 240 \
    srun --ntasks=1 --cpus-per-task=14 --gpus-per-node=8 --unbuffered --export=ALL \
      $OUT/unbind "$@"
}
cpu() {   # a host program on the node's cores
  OMP_NUM_THREADS=$THREADS OMP_PROC_BIND=spread OMP_PLACES=cores \
    timeout --signal=TERM --kill-after=15 600 \
    srun --ntasks=1 --cpus-per-task=$THREADS --unbuffered --export=ALL "$@"
}
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
kernel() {   # tag, unbind options... : one form run to the end, dump checked
  local tag=$1; shift
  gpu $SNAP --bound-fraction 1 --dump $OUT/kernel_$tag.hdf5 "$@" > $OUT/unbind_$tag.log 2>&1
  echo "  unbind $tag ($*): exit $?"
  grep -E '^unbind: cap|^groups with a self-bound|^bound particles|^unbinding passes|^pair terms|^capped sampling|^time:' $OUT/unbind_$tag.log | sed 's/^/    /'
  dumpcheck $OUT/kernel_$tag.hdf5 | sed 's/^/    /'
}
check() {   # tag, reference, extra unbindcheck options... Sets RC.
  local tag=$1 ref=$2; shift 2
  cpu $OUT/unbindcheck $SNAP $ref $OUT/kernel_$tag.hdf5 --tau $TAU "$@" > $OUT/unbindcheck_$tag.log 2>&1
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
  cpu $OUT/unbindcheck $SNAP $ref $OUT/kernel_$tag.hdf5 --tau $TAU \
      --single-against $OUT/kernel_$dbl.hdf5 "$@" > $OUT/unbindcheck_allowance_$tag.log 2>&1
  echo "  unbindcheck $tag with the allowance against $dbl: exit $?"
  grep -E 'single-precision allowance|tolerance =|^PASS|^FAIL' $OUT/unbindcheck_allowance_$tag.log | sed 's/^/    /'
}

echo "=== A. no cap"
cpu $OUT/unbindspec $SNAP $OUT/spec_nocap.particles.bin > $OUT/unbindspec_nocap.log 2>&1
echo "  unbindspec: exit $?"; sed 's/^/    /' $OUT/unbindspec_nocap.log
$OUT/unbindcmp $REF_EXACT $OUT/spec_nocap.particles.bin --tau $TAU > $OUT/unbindcmp_spec_nocap.log 2>&1
echo "  unbindcmp, exact reference against unbindspec: exit $?"
grep -E 'bound in both|dE|^PASS|^FAIL' $OUT/unbindcmp_spec_nocap.log | sed 's/^/    /'
kernel nocap_f64
check nocap_f64 $REF_EXACT

for C in $CAPS; do
  echo "=== B. cap $C"
  cpu $OUT/unbindspec $SNAP $OUT/spec_cap$C.particles.bin --cap $C > $OUT/unbindspec_cap$C.log 2>&1
  echo "  unbindspec --cap $C: exit $?"; sed 's/^/    /' $OUT/unbindspec_cap$C.log
  REF=$OUT/spec_cap$C.particles.bin
  kernel cap${C}_f64 --cap $C;                             check cap${C}_f64 $REF --cap $C;        F64=$RC
  kernel cap${C}_f64_rsqrt --cap $C --kernel f64 --rsqrt;  check cap${C}_f64_rsqrt $REF --cap $C;  F64R=$RC
  for k in f32 f32f32; do
    kernel cap${C}_$k --cap $C --kernel $k
    check cap${C}_$k $REF --cap $C
    allowance cap${C}_$k $REF cap${C}_f64 $F64 --cap $C
    kernel cap${C}_${k}_rsqrt --cap $C --kernel $k --rsqrt
    check cap${C}_${k}_rsqrt $REF --cap $C
    allowance cap${C}_${k}_rsqrt $REF cap${C}_f64_rsqrt $F64R --cap $C
  done
done

echo "=== C. the two exact conditions"
kernel bigcap_f64 --cap $BIGCAP
$HBIN/h5diff $OUT/kernel_nocap_f64.hdf5 $OUT/kernel_bigcap_f64.hdf5 > $OUT/h5diff_bigcap.log 2>&1
echo "  cap $BIGCAP against no cap, h5diff of the two dumps: exit $? (0 = identical)"
head -5 $OUT/h5diff_bigcap.log | sed 's/^/    /'
for C in $CAPS; do
  for d in Bound Energy GroupIDs; do
    $HBIN/h5dump -d /$d -b LE -o $OUT/nocap_$d.raw $OUT/kernel_nocap_f64.hdf5 > /dev/null
    $HBIN/h5dump -d /$d -b LE -o $OUT/cap_$d.raw $OUT/kernel_cap${C}_f64.hdf5 > /dev/null
  done
  python3 - $OUT $C <<'PY'
import sys
import numpy as np
o, cap = sys.argv[1], int(sys.argv[2])
g = np.fromfile(o + "/nocap_GroupIDs.raw", dtype="<i8")
size = np.bincount(g)
small = size[g] <= cap                      # particles of groups at or below the cap
same = True
for d, t in (("Bound", np.uint8), ("Energy", "<u8")):       # energies compared bit for bit
    a = np.fromfile(o + f"/nocap_{d}.raw", dtype=t)
    b = np.fromfile(o + f"/cap_{d}.raw", dtype=t)
    diff = int((a[small] != b[small]).sum())
    same = same and diff == 0
    print(f"  cap {cap}, kernel, groups at or below the cap: {int(small.sum())} particles in "
          f"{int(((size > 0) & (size <= cap)).sum())} groups, {d} differing in {diff}")
rec = [("id", "<i8"), ("g", "<i8"), ("e", "<u8")]
a = np.fromfile(o + "/spec_nocap.particles.bin", dtype=rec)
b = np.fromfile(o + f"/spec_cap{cap}.particles.bin", dtype=rec)
a = np.sort(a[size[a["g"]] <= cap], order="id")
b = np.sort(b[size[b["g"]] <= cap], order="id")
ok = len(a) == len(b) and bool((a == b).all())
print(f"  cap {cap}, unbindspec, groups at or below the cap: {len(a)} and {len(b)} bound "
      f"records, {'identical' if ok else 'DIFFERENT'}")
print(f"  EXACT CONDITION cap {cap}: {'ok' if same and ok else 'FAIL'}")
PY
done

if [ -n "$MUTANTS" ]; then
  C=${CAPS%% *}
  echo "=== D. deliberately broken kernels at cap $C (host build, $THREADS threads): each must fail"
  for b in $MUTANTS/unbind_control $(ls $MUTANTS/unbind_* | grep -v unbind_control); do
    name=$(basename $b); name=${name#unbind_}
    cpu $b $SNAP --bound-fraction 1 --cap $C --dump $OUT/mutant_$name.hdf5 > $OUT/mutant_$name.log 2>&1
    cpu $OUT/unbindcheck $SNAP $OUT/spec_cap$C.particles.bin $OUT/mutant_$name.hdf5 --tau $TAU --cap $C \
      > $OUT/mutantcheck_$name.log 2>&1
    rc=$?
    want=1; [ $name = control ] && want=0
    echo "  $name: unbindcheck exit $rc (expected $want) $([ $rc = $want ] && echo ok || echo UNEXPECTED)"
    grep -E '^candidate:|^bound in both|^groups: |^1\. |^2\. |^3\. ' $OUT/mutantcheck_$name.log | cut -c1-200 | sed 's/^/    /'
  done
fi
sacct -j $J -X -n -o state,elapsed
