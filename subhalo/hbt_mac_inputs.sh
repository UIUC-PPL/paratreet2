#!/bin/bash
# The six inputs of design/subhalo-checks.md, "A scale factor other than 1,
# and groups at least half the box wide", rebuilt on the Mac with the
# commands of run_unbind_scale_wide.sbatch (step 1). FoF3 runs in one process
# with 8 threads here. Needs inputs/8m-lambb.tipsy, FoF3 built by
# examples/fof3/scripts/build_mac.sh, `make convert`, and the 8m-lambb label
# files and snapshot in $R/8m-lambb (Check 2).
#
#   R    where the inputs go (default ~/hbt-test)
#   PY   a python with numpy (default python3)
S=$(cd "$(dirname "$0")" && pwd)
PT=$(cd "$S/.." && pwd)
R=${R:-$HOME/hbt-test}
PY=${PY:-python3}
T8=$PT/inputs/8m-lambb.tipsy
T806=$R/806k-lambb.tipsy
LINK8M=0.000997425          # 8m-lambb's linking length, 0.2 (V/N)^(1/3)

snapshot() {   # name, tipsy, label prefix, tipsy2hbt options...
  local name=$1 tipsy=$2 lbl=$3; shift 3
  mkdir -p $R/$name/snap
  $S/tipsy2hbt -i $tipsy -l $lbl -o $R/$name/snap/snap_0000.hdf5 "$@" > $R/$name/tipsy2hbt.log 2>&1
  echo "  tipsy2hbt $name ($*): exit $?"; sed 's/^/    /' $R/$name/tipsy2hbt.log
}
fof3() {   # name, tipsy, b, period
  local name=$1 tipsy=$2 b=$3 period=$4
  mkdir -p $R/$name/labels
  rm -f $R/$name/labels/l.*.lbl
  ( cd $PT/examples/fof3 && DYLD_LIBRARY_PATH=$HOME/charm_reconverse/lib ./FoF3 -f $tipsy -d oct -u dist \
      -b $b -c stats -l 12 -i 1 -K -m 20 -P $period -L $R/$name/labels/l +p8 > $R/$name/fof3.log 2>&1 )
  local rc=$?
  echo "  FoF3 $name (b $b, period $period): exit $rc"
  grep -E 'linking_length_exact|label_digest|FOF3STAT catalog|label_files' $R/$name/fof3.log | cut -c1-200 | sed 's/^/    /'
  grep -q 'FOF3STAT label_files' $R/$name/fof3.log && [ $rc = 0 ]
}

echo "=== 0. the converter rebuilds the a = 1 snapshot"
mkdir -p $R/regress/snap
$S/tipsy2hbt -i $T8 -l $R/8m-lambb/labels/8m -o $R/regress/snap/snap_0000.hdf5 --box-mpc 100 --h 0.7 > /dev/null 2>&1
h5diff $R/8m-lambb/snap/snap_0000.hdf5 $R/regress/snap/snap_0000.hdf5 | head -5
echo "  h5diff against the existing 8m-lambb snapshot: exit ${PIPESTATUS[0]} (0 = identical)"
rm $R/regress/snap/snap_0000.hdf5 && rmdir $R/regress/snap $R/regress

echo "=== 1. inputs"
snapshot a0.5 $T8 $R/8m-lambb/labels/8m --box-mpc 100 --h 0.7 --scale-factor 0.5
snapshot a1.25 $T8 $R/8m-lambb/labels/8m --box-mpc 100 --h 0.7 --scale-factor 1.25
$PY $S/subsample_tipsy.py $T8 $T806 | sed 's/^/  /'
fof3 w565 $T806 0.565 1 && snapshot w565 $T806 $R/w565/labels/l --box-mpc 100 --h 0.7
fof3 w580 $T806 0.58 1 && snapshot w580 $T806 $R/w580/labels/l --box-mpc 100 --h 0.7
for side in 0.045 0.04; do
  c=cube$($PY -c "print(round($side * 1000))")
  mkdir -p $R/$c
  $PY $S/subcube_tipsy.py $T8 $R/$c/cube.tipsy -0.25730261 0.17688775 0.35168427 $side | sed 's/^/  /'
  # b that gives 8m-lambb's linking length in this cube: FoF3 takes b (V/N)^(1/3)
  bcube=$($PY -c "
import struct
n = struct.unpack('>i', open('$R/$c/cube.tipsy', 'rb').read(12)[8:12])[0]
print(f'{$LINK8M / ($side ** 3 / n) ** (1 / 3):.6f}')")
  fof3 $c $R/$c/cube.tipsy $bcube $side &&
    snapshot $c $R/$c/cube.tipsy $R/$c/labels/l --box-mpc $($PY -c "print($side * 100)") --h 0.7 --period $side
done
echo "=== FoF3 label digests on Frontier (job 5904186): w565 c6db3522b8591d53, w580 904d16defaeb8f0a,"
echo "    cube45 317c06d80c306124, cube40 3e2c10066f6666ec. The snapshots' md5 cannot equal"
echo "    Frontier's: tipsy2hbt's files are not byte-reproducible (the same input built twice"
echo "    has two md5 values and h5diff finds no difference). The digests, h5diff and"
echo "    unbindspec's counts are the check."
echo "=== inputs done"
