#!/bin/bash
# One HBT-HERONS run on the Mac with the exact-reference settings
# (hbt_8m_lambb_exact.conf: double-precision build, no sampling,
# BoundMassPrecision 1, energies saved), then hbtref. Used for "HBT-HERONS on
# the Mac" in design/subhalo-checks.md.
#
#   hbt_mac_run.sh <input name> <tag> <cap, 0 = none> [HBT binary]
#
#   ANGLE=0.1   tree opening angle instead of 0: 4.4 times faster on the 8M
#               input, but the result is then not an exact reference
#   R           where the inputs live (default ~/hbt-test)
#
# Reads  $R/<input>/snap/snap_0000.hdf5
# Writes $R/<input>/hbt_<tag>.conf, hbt_<tag>.log, out-<tag>/,
#        <tag>.particles.bin, <tag>.fof3.particles.bin, <tag>.groups.txt
#
# The fourth argument is for the build with hbt_firstparticle.patch, which
# the inputs with a group at least half the box wide need.
S=$(cd "$(dirname "$0")" && pwd)
R=${R:-$HOME/hbt-test}
NAME=$1; TAG=$2; CAP=$3
HBT=${4:-$HOME/HBT-HERONS/build-real8/HBT}
D=$R/$NAME
[ -n "$CAP" ] || { echo "usage: hbt_mac_run.sh <input name> <tag> <cap, 0 = none> [HBT binary]"; exit 2; }
[ -s $D/snap/snap_0000.hdf5 ] || { echo "hbt_mac_run: no snapshot in $D/snap"; exit 2; }
[ -e $D/out-$TAG ] && { echo "hbt_mac_run: $D/out-$TAG exists, not overwriting"; exit 2; }
sed -e "s#^SnapshotPath .*#SnapshotPath      $D/snap#" -e "s#^HaloPath .*#HaloPath          $D/snap#" \
    -e "s#^SubhaloPath .*#SubhaloPath       $D/out-$TAG#" \
    -e "s#^MaxSampleSizeOfPotentialEstimate .*#MaxSampleSizeOfPotentialEstimate  $CAP#" \
    -e "s#^TreeNodeOpenAngle .*#TreeNodeOpenAngle                 ${ANGLE:-0}#" \
    $S/hbt_8m_lambb_exact.conf > $D/hbt_$TAG.conf
grep -q "^SubhaloPath  *$D/out-$TAG\$" $D/hbt_$TAG.conf || { echo "hbt_mac_run: paths not set"; exit 2; }
grep -q "^TreeNodeOpenAngle  *${ANGLE:-0}\$" $D/hbt_$TAG.conf || { echo "hbt_mac_run: angle not set"; exit 2; }
echo "=== $NAME $TAG (cap $CAP, opening angle ${ANGLE:-0}) $(date '+%H:%M:%S')"
( OMP_NUM_THREADS=4 /usr/bin/time -p mpirun -np 2 $HBT $D/hbt_$TAG.conf ) > $D/hbt_$TAG.log 2>&1
rc=$?
grep -E 'Unbinding\.\.\.|newly identified|without any self-bound|^real' $D/hbt_$TAG.log | cut -c1-160
[ $rc = 0 ] || { echo "hbt_mac_run: HBT exit $rc"; tail -5 $D/hbt_$TAG.log; exit 1; }
$S/hbtref $D/out-$TAG/000 $D/$TAG $D/snap/snap_0000.hdf5 | tail -4
echo "=== $NAME $TAG done $(date '+%H:%M:%S')"
