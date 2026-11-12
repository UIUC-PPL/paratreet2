#!/bin/bash
# One HBT-HERONS result on the Mac against the written definition
# (unbindspec), for "HBT-HERONS on the Mac" in design/subhalo-checks.md.
#
#   hbt_mac_compare.sh <input name> <tag of the HBT-HERONS run> [wide]
#
# The definition is the reference and HBT-HERONS the candidate, so that the
# amended rule replays the definition. The definition is run in these forms:
#
#   spec                 as written: every frame's nearest-image offsets from
#                        the lowest particle id still bound, every pass afresh
#   spec-carried         passes carried as in HBT-HERONS's source (--carried)
# and, with `wide` (the run was made with the hbt_firstparticle.patch build),
#   spec-origin-<tag>    as written, first frame from HBT-HERONS's first particle
#   spec-carried-<tag>   that particle and --carried: what HBT-HERONS computes
#   spec-kernel-<tag>    that particle and --carried-kernel: the kernel's rule
#
# A narrow input is judged by unbindcheck (both rules). unbindcheck knows
# neither --first-origin nor --carried, so a wide input is judged by
# unbindcmp (the rule as first written) and listed group by group.
#
#   R        where the inputs live (default ~/hbt-test)
#   PY       a python with numpy (default python3); H5DUMP as in first_origins.py
#   REUSE=1  keep an unbindspec output that already exists
S=$(cd "$(dirname "$0")" && pwd)
R=${R:-$HOME/hbt-test}
PY=${PY:-python3}
TAU=${TAU:-0.00141}
NAME=$1; TAG=$2; WIDE=$3
D=$R/$NAME
[ -n "$TAG" ] || { echo "usage: hbt_mac_compare.sh <input name> <tag> [wide]"; exit 2; }
cd $D || exit 2
SNAP=snap/snap_0000.hdf5; HBT=$TAG.particles.bin
[ -s $HBT ] || { echo "hbt_mac_compare: no $D/$HBT"; exit 2; }
spec() {   # output tag, unbindspec options...
  local tag=$1; shift
  echo "--- $tag ($*)"
  if [ "$REUSE" = 1 ] && [ -s $tag.particles.bin ]; then
    echo "    kept the existing $tag.particles.bin: $(( $(stat -f %z $tag.particles.bin) / 24 )) bound particles"
    return
  fi
  $S/unbindspec $SNAP $tag.particles.bin "$@" |
    grep -E 'first-frame|carried|unbindspec:   group|with a sub-halo|passes per group' | sed 's/^/    /'
}
echo "=== $NAME, HBT-HERONS run $TAG: $(( $(grep -c . $TAG.groups.txt) - 1 )) groups with a sub-halo, $(( $(stat -f %z $HBT) / 24 )) bound particles, $(grep '^real' hbt_$TAG.log) s"
spec spec
if [ "$WIDE" = wide ]; then
  $PY $S/first_origins.py $SNAP hbt_$TAG.log origins-$TAG.txt || exit 2
  spec spec-origin-$TAG --first-origin origins-$TAG.txt
  spec spec-carried-$TAG --first-origin origins-$TAG.txt --carried
  spec spec-kernel-$TAG --first-origin origins-$TAG.txt --carried-kernel
  LIST="spec spec-origin-$TAG spec-carried-$TAG spec-kernel-$TAG"
else
  spec spec-carried --carried
  LIST="spec spec-carried"
fi
for c in $LIST; do
  echo "================ $NAME: HBT-HERONS ($TAG) against $c"
  if [ "$WIDE" = wide ]; then
    $S/unbindcmp $c.particles.bin $HBT --tau $TAU | tail -11
    echo "groups where anything differs (first 12):"
    $PY $S/group_diff.py $c.particles.bin $HBT $TAU | head -13
  else
    $S/unbindcheck $SNAP $c.particles.bin $HBT --tau $TAU --list 20 | sed -n '/^reference:/,$p'
  fi
done
echo "=== $NAME $TAG compared"
