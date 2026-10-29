#!/bin/bash
# Extract each dataset's EXACT linking length from the FoF3 logs and write
# scripts/linklen.env. The ArborX and SWIFT arms source this, so no dataset's
# b is ever typed twice or re-derived.
#
# Fails loudly if two FoF3 runs of the same dataset disagree: b depends only
# on the snapshot, so a disagreement means one of the runs read different data
# (or a different padding rule), and every downstream comparison built on it
# would be invalid.
CMP=/lustre/orion/csc710/scratch/rrao/compare
out=$CMP/scripts/linklen.env
: > "$out.tmp"
rc=0
for dset in cosmo2b cosmo25 romulus25; do
  # Only OK points, and only non-empty values. A run that died before FoF3
  # printed its linking length leaves "b_exact=" with nothing after it, and
  # grep -o then yields an EMPTY value that sorted to the front and made the
  # generated file
  #     LINKLEN_romulus25=
  #     5.167330988573081e-05
  # i.e. a bare number on its own line, which the consumers then tried to
  # EXECUTE ("5.167...e-05: command not found").
  vals=$(grep -h "^VERDICT dset=$dset " $CMP/logs/*.out 2>/dev/null | \
         grep "status=OK" | \
         grep -o "b_exact=[0-9][0-9.eE+-]*" | sed 's/b_exact=//' | \
         grep -v '^NA$' | grep -v '^$' | sort -u)
  n=$(echo "$vals" | grep -c . )
  if [ "$n" -eq 0 ]; then
    echo "# $dset: no FoF3 run has printed a linking length yet" >> "$out.tmp"
  elif [ "$n" -eq 1 ]; then
    echo "LINKLEN_$dset=$vals" >> "$out.tmp"
    echo "$dset: $vals"
  else
    echo "ERROR: $dset has $n different linking lengths across FoF3 runs:" >&2
    echo "$vals" | sed 's/^/  /' >&2
    echo "# $dset: AMBIGUOUS -- $n distinct values, refusing to guess" >> "$out.tmp"
    rc=1
  fi
done
mv "$out.tmp" "$out"
echo "wrote $out"
exit $rc
