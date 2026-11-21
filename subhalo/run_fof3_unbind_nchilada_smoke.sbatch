#!/bin/bash
# Step 2 on cosmo25 (design/subhalo-checks.md, "cosmo25"): the two-node
# smoke test before the 64-node run. cosmo25 is an NChilada snapshot, and
# FoF3 -U has only read Tipsy so far. The 100k test snapshot exists in both
# formats with the same float32 values (nctest, 2026-08-21), so every digest
# of FoF3 -U must be the same on the two files.
#
#   CHECK_ONLY=1 bash subhalo/run_fof3_unbind_nchilada_smoke.sbatch
#   sbatch subhalo/run_fof3_unbind_nchilada_smoke.sbatch
#
# Runs, 16 processes on two nodes (the multi-node launch of the big runs),
# FoF3 -K -m 20 -U, open boundaries (the 100k input is an isolated sphere),
# softening 0.05 (the files' value), every run until a pass removes nothing:
#   def    the defaults: every group gathered (the largest has 26,042)
#   spr    cap 500, refinement 100, gather 1,000: the two largest groups
#          sampled and spread over processes
# Each on 100k.tipsy and 100k.nchilada. Pass: label digest, catalogue
# digest, both unbinding digests, the spread digests and the rows digest the
# same on the two files, and the rows digest equal to digest_subhaloes.
# Last, 8m-lambb with 60 groups spread (job 5923267's spr8_np16): the same
# digests, since the binary gained only prints (unbind_spread_largest;
# unbind_by_size and unbind_by_family, whose sums must equal the totals).

#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:15:00
#SBATCH --job-name=fof3ncsmoke
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/fof3ncsmoke.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/fof3ncsmoke.%j.err

PT=${PT:-/ccs/home/rrao/paratreet2}
S=${S:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11}
NC=/lustre/orion/csc710/scratch/rrao/nctest
WRAP=${WRAP:-/lustre/orion/csc710/scratch/rrao/lazycanopy-2026-11-02/v7/gcd_wrap.sh}
EPS=0.05000000074505806
declare -A SET=(
  [def]="eps=$EPS,bf=1"
  [spr]="eps=$EPS,bf=1,cap=500,refine=100,gather=1000"
)

missing=0
for f in $S/8m/8m-lambb.tipsy $NC/100k.tipsy $NC/100k.nchilada/dark/vel $NC/100k.nchilada/dark/soft $WRAP $PT/examples/fof3/FoF3; do
  [ -s $f ] || { echo "### MISSING: $f"; missing=1; }
done
strings $PT/examples/fof3/FoF3 | grep -q 'unbind_rows' || { echo "### FoF3 has no part 2d"; missing=1; }
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
J=${SLURM_JOB_ID:-nojob}
OUT=$S/fof3ncsmoke.$J
B=$OUT/bin
mkdir -p $B && cp $PT/examples/fof3/FoF3 $B/ || exit 1
echo "### commit $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet || echo ' + uncommitted changes'); md5 FoF3 $(md5sum $B/FoF3 | cut -c1-12)"

# The digests a run prints, as one line: label, catalogue, unbinding, spread, rows.
digests() {
  local l=$OUT/$1/fof3.log
  echo "label $(grep -o 'label_digest: n [0-9]* digest [0-9a-f]*' $l | awk '{print $3, $5}')" \
       "catalog $(grep -o 'FOF3STAT catalog: .* digest [0-9a-f]*' $l | awk '{print $NF}')" \
       "unbind $(grep -o 'FOF3STAT unbind: .*' $l | grep -o 'subhaloes [0-9]* bound [0-9]*\|digest_subhaloes [0-9a-f]* digest_particles [0-9a-f]*' | tr '\n' ' ')" \
       "spread $(grep -o 'FOF3STAT unbind_spread: groups [0-9]*' $l | awk '{print $NF}') $(grep -o 'FOF3STAT unbind_spread: .*' $l | grep -o 'digest_subhaloes [0-9a-f]* digest_particles [0-9a-f]*')" \
       "rows $(grep -o 'unbind_rows: subhaloes [0-9]* digest [0-9a-f]* ([a-z ]*digest_subhaloes)' $l)"
}

fail=0
for run in def spr; do
  for fmt in tipsy nchilada; do
    name=${run}_$fmt
    d=$OUT/$name
    mkdir -p $d
    ( cd $PT/examples/fof3 && PARATREET_DEVICE_TREE=0 timeout --signal=TERM --kill-after=15 300 \
      srun --mpi=cray_shasta --unbuffered --cpu-bind=none --distribution=block:block \
        --nodes=2 --ntasks=16 --gpus-per-node=8 --export=ALL \
        $WRAP $B/FoF3 -f $NC/100k.$fmt -d oct -u dist -b 0.2 -c stats -l 12 -i 1 -K -m 20 \
        -U "${SET[$run]}" +ppn 7 +pemap 1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63 \
        +lci_ndevices 7 +backend_poll_thread 1 > $d/fof3.log 2>&1 )
    rc=$?
    echo "=== $name (-U ${SET[$run]}): exit $rc"
    grep -E 'label_digest|FOF3STAT catalog|FOF3STAT unbind' $d/fof3.log | grep -v unbind_settings | cut -c1-400 | sed 's/^/  /'
    [ $rc = 0 ] || { fail=1; grep -m8 -E 'Fatal|Abort|abort|rror|subhalodev|unbind::spread' $d/fof3.log | sed 's/^/  /'; }
  done
  a=$(digests ${run}_tipsy); b=$(digests ${run}_nchilada)
  echo "  $run tipsy:    $a"
  echo "  $run nchilada: $b"
  if [ "$a" = "$b" ] && [[ "$a" == *"(equals digest_subhaloes)"* ]] && [[ "$a" == *"digest_particles"* ]]; then
    echo "  $run: PASS (every digest the same on the two formats, rows equal digest_subhaloes)"
  else
    echo "  $run: FAIL"; fail=1
  fi
done
# The binary gained only a print (FOF3STAT unbind_spread_largest): 8m-lambb
# with 60 spread groups must give job 5923267's spr8_np16 digests exactly.
T8=$S/8m/8m-lambb.tipsy
OLD8="unbind digest_subhaloes 60b25ca96fb0c3a0 digest_particles dbf68c1f739a5748 spread digest_subhaloes f0df4f01b805bdc1 digest_particles 27f553027f64da50"
d=$OUT/spr8_np16
mkdir -p $d
( cd $PT/examples/fof3 && PARATREET_DEVICE_TREE=0 timeout --signal=TERM --kill-after=15 300 \
  srun --mpi=cray_shasta --unbuffered --cpu-bind=none --distribution=block:block \
    --nodes=2 --ntasks=16 --gpus-per-node=8 --export=ALL \
    $WRAP $B/FoF3 -f $T8 -d oct -u dist -b 0.2 -c stats -l 12 -i 1 -K -m 20 -P 1 \
    -U "eps=9.999999776482582e-05,cap=10000,refine=1000,bf=1,G=0.9999517944622952,gather=10000" \
    +ppn 7 +pemap 1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63 \
    +lci_ndevices 7 +backend_poll_thread 1 > $d/fof3.log 2>&1 )
rc=$?
echo "=== spr8_np16 (8m-lambb, 60 groups spread): exit $rc"
grep -E 'label_digest|FOF3STAT catalog|FOF3STAT unbind' $d/fof3.log | grep -v unbind_settings | cut -c1-400 | sed 's/^/  /'
new8="unbind $(grep -o 'FOF3STAT unbind: .*' $d/fof3.log | grep -o 'digest_subhaloes [0-9a-f]* digest_particles [0-9a-f]*') spread $(grep -o 'FOF3STAT unbind_spread: .*' $d/fof3.log | grep -o 'digest_subhaloes [0-9a-f]* digest_particles [0-9a-f]*')"
if [ $rc = 0 ] && [ "$new8" = "$OLD8" ]; then
  echo "  spr8_np16: PASS (job 5923267's digests)"
else
  echo "  spr8_np16: FAIL: $new8; job 5923267: $OLD8"; fail=1
fi
# FOF3STAT unbind_by_size and unbind_by_family check their sums against the
# totals (added 2026-11-21); none may differ.
if grep -l 'DIFFER FROM' $OUT/*/fof3.log; then echo "  a sum check DIFFERS (files above)"; fail=1
else echo "  unbind_by_size / unbind_by_family / unbind_rows: every sum equals its total"; fi
grep -h 'unbind_by_family' $OUT/*/fof3.log | sed 's/^/  /'
echo "=== smoke: $([ $fail = 0 ] && echo PASS || echo FAIL) ($(date +%T))"
sacct -j $J -X -n -o elapsed,nnodes,state
