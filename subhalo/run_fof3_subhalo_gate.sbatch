#!/bin/bash
# Step 2, parts 2c (16 processes) and 2d (design/subhalo-checks.md, "Step
# 2: unbinding inside FoF3"), and the 2-node lambb gate. Two nodes.
#
#   make -C subhalo device all compare
#   make -C fof GPU=1 ...; make -C examples/fof3 GPU=1 ...; make -C examples/fofhost GPU=1 ...
#   CHECK_ONLY=1 bash subhalo/run_fof3_subhalo_gate.sbatch
#   sbatch subhalo/run_fof3_subhalo_gate.sbatch
#
# Parts (PARTS, default "0 1 2 3"):
#   0. The sample fetch by key threshold (unbind_spread.h): unbind --spread
#      on the 8m-lambb snapshot, one die, H = 1, H = 4 and H = 8 slabs, and
#      FoF3 with gather=10000 on 8 processes: the threshold changes what is
#      sent, not the sample, so every digest must equal job 5917906's.
#   1. 8m-lambb, FoF3 with -L (cap 10,000, refine 1,000, bf=1, the
#      snapshot's G): gathered on 8 and 16 processes, gather=10000 on 16; the
#      defaults with and without gather=10000 on 16 (time). Checks: label and
#      catalogue digests; digest_subhaloes of every capped run equals job
#      5917906's 60b25ca96fb0c3a0; the sub-halo label digest is the same on 8
#      and 16 processes and with gather=10000; the catalogue rows carry every
#      sub-halo (FOF3STAT unbind_rows); fof3ubd.py sub: each .sub file agrees
#      with the .ubd bound flags and each .lbl label with the .ubd's.
#   2. The plug-in (examples/fofhost) on 8m-lambb, 16 processes, the same
#      settings, the host's own placement and its host cells (-c 6): its
#      label, catalogue, sub-halo label and row digests against FoF3's.
#   3. The lambb gate: lambb.00500 (80,621,568 particles, periodic box of
#      side 1, a = 1, softening 1e-4), FoF3 -K -m 20 -P 1 -U at the
#      production defaults (gather 1,000,000: the 1,519,203-particle group
#      stays spread) with -L, on 8 and 16 processes, and gather=0 on 16; the
#      plug-in on 16. Checks: label_digest e88625a3f5164c05 (periodic gold);
#      the gathered groups' digests the same on 8 and 16 processes; the
#      catalogue rows carry every sub-halo; fof3ubd.py sub; the spread and
#      gathered runs and the plug-in agree on the sub-halo labels (decision
#      3 asks only that the spread group pass the pass rule across layouts;
#      equal sub-halo labels are more than that, and are reported either way).

#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:40:00
#SBATCH --job-name=fof3subgate
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/fof3subgate.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11/fof3subgate.%j.err

PT=${PT:-/ccs/home/rrao/paratreet2}
S=${S:-/lustre/orion/csc710/scratch/rrao/unbind-2026-11-11}
T8=$S/8m/8m-lambb.tipsy
LAMBB=/lustre/orion/csc710/proj-shared/lambb.00500
WRAP=${WRAP:-/lustre/orion/csc710/scratch/rrao/lazycanopy-2026-11-02/v7/gcd_wrap.sh}
EPS8=9.999999776482582e-05      # 8m-lambb: the snapshot's softening, as in jobs 5916950 .. 5917906
EPSL=9.999999747378752e-05      # lambb.00500: the Tipsy file's eps
GEFF=0.9999517944622952
LABEL_DIGEST8=c4f41054eacf794c
CATALOG_DIGEST8=57c1e840e74a8aae
SUBHALO_DIGEST8=60b25ca96fb0c3a0
LABEL_DIGEST_LAMBB_P1=e88625a3f5164c05
PARTS=${PARTS:-"0 1 2 3"}
SNAP=$S/8m/snap/snap_0000.hdf5
declare -A OLD_SPREAD=([s1]=ab9798e1895ce025 [s4]=4a9cd521adcacb57 [s8slab]=cd125e4fd37ec9d6)
OLD_SPR_NP8="digest_subhaloes 60b25ca96fb0c3a0 digest_particles 1e18d903818f6e41"
part() { [[ " $PARTS " == *" $1 "* ]]; }
CAPG="eps=$EPS8,cap=10000,refine=1000,bf=1,G=$GEFF"

missing=0
for f in $T8 $LAMBB $SNAP $PT/subhalo/unbind $WRAP $PT/examples/fof3/FoF3 $PT/examples/fofhost/fofhost \
         $PT/subhalo/fof3ubd.py $PT/subhalo/capdistance.py; do
  [ -s $f ] || { echo "### MISSING: $f"; missing=1; }
done
strings $PT/examples/fof3/FoF3 | grep -q 'unbind_rows' ||
  { echo "### $PT/examples/fof3/FoF3 has no part 2d: rebuild with GPU=1"; missing=1; }
strings $PT/examples/fofhost/fofhost | grep -q 'FOFHOST subhalo' ||
  { echo "### $PT/examples/fofhost/fofhost has no -U: rebuild with GPU=1"; missing=1; }
[ $missing = 0 ] || { echo "### ABORT: inputs missing"; exit 2; }
[ "${CHECK_ONLY:-0}" = 1 ] && { echo "### inputs present; CHECK_ONLY set, nothing run"; exit 0; }

source $PT/examples/fof3/scripts/frontier_env.sh
J=${SLURM_JOB_ID:-nojob}
OUT=$S/fof3subgate.$J
B=$OUT/bin
mkdir -p $B && cp $PT/examples/fof3/FoF3 $PT/examples/fofhost/fofhost $PT/subhalo/fof3ubd.py $PT/subhalo/unbind \
  $PT/subhalo/capdistance.py $B/ || exit 1
echo "### parts $PARTS; nodes $(scontrol show hostnames $SLURM_JOB_NODELIST | tr '\n' ' ') commit $(git -C $PT rev-parse --short HEAD)$(git -C $PT diff --quiet || echo ' + uncommitted changes')"
echo "### md5: FoF3 $(md5sum $B/FoF3 | cut -c1-12) fofhost $(md5sum $B/fofhost | cut -c1-12)"

# name, processes, program and its arguments: one run in $OUT/name.
launch() {
  local name=$1 np=$2 d=$OUT/$1 map net nodes=1; shift 2
  mkdir -p $d
  case $np in
    1) map=1-7 ;;
    *) map=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63 ;;
  esac
  net="--network=single_node_vni"
  [ $np -gt 8 ] && { nodes=$(( (np + 7) / 8 )); net=""; }
  ( cd $PT/examples/fof3 && PARATREET_DEVICE_TREE=0 timeout --signal=TERM --kill-after=15 600 \
    srun --mpi=cray_shasta $net --unbuffered --cpu-bind=none --distribution=block:block \
      --nodes=$nodes --ntasks=$np --gpus-per-node=8 --export=ALL \
      $WRAP "$@" +ppn 7 +pemap $map +lci_ndevices 7 +backend_poll_thread 1 > $d/run.log 2>&1 )
  RC=$?
}
show() {   # name: the lines that matter
  local d=$OUT/$1
  grep -E 'label_digest|FOF3STAT catalog|FOF3STAT unbind|FOF3STAT subhalo|FOF3STAT label_files|linking_length_exact|FOFHOST|FOFPLUGIN (catalog|unbind)' $d/run.log \
    | grep -v unbind_settings | cut -c1-400 | sed 's/^/    /'
  [ $RC = 0 ] || grep -m8 -E 'Fatal|Abort|abort|rror|subhalodev|unbind::spread' $d/run.log | sed 's/^/    /'
}
# name, tipsy, processes, -U settings, extra FoF3 options
fof3() {
  local name=$1 in=$2 np=$3 set=$4; shift 4
  launch $name $np $B/FoF3 -f $in -d oct -u dist -b 0.2 -c stats -l 12 -i 1 -K -m 20 -P 1 \
    -U "$set,out=$OUT/$name/u" -L $OUT/$name/l "$@"
  echo "  FoF3 $name ($np processes; $set): exit $RC"
  show $name
}
field() { grep -o "$2 [0-9a-f]*" $OUT/$1/run.log 2>/dev/null | head -1 | awk '{print $2}'; }
line() { grep -m1 "$2" $OUT/$1/run.log 2>/dev/null; }
subdigest() { grep -o 'subhalo_label_files: records [0-9]* in_subhalo [0-9]* digest [0-9a-f]*' $OUT/$1/run.log | awk '{print $NF}'; }
gathdig() { grep -o 'unbind_gathered: digest_subhaloes [0-9a-f]* digest_particles [0-9a-f]*' $OUT/$1/run.log; }
subcheck() {
  python3 $B/fof3ubd.py sub $OUT/$1/u $OUT/$1/l 2>&1 | sed 's/^/    /'
  echo "    fof3ubd.py sub $1: exit ${PIPESTATUS[0]} (0 = pass)"
}
yesno() { [ -n "$1" ] && [ "$1" = "$2" ] && echo yes || echo NO; }

if part 0; then
echo "=== 0. the sample fetch by key threshold against job 5917906"
for spec in "s1 --spread 1" "s4 --spread 4" "s8slab --spread 8 --spread-slabs"; do
  set -- $spec; name=$1; shift
  mkdir -p $OUT/ub
  ROCR_VISIBLE_DEVICES=0 timeout --signal=TERM --kill-after=15 300 \
    srun --nodes=1 --ntasks=1 --cpus-per-task=14 --gpus-per-node=8 --unbuffered --export=ALL \
    $B/unbind $SNAP --bound-fraction 1 --cap 10000 --refine 1000 "$@" > $OUT/ub/$name.log 2>&1
  rc=$?
  d=$(grep -o 'spread: digest [0-9a-f]*' $OUT/ub/$name.log | awk '{print $3}')
  echo "  unbind $name ($*): exit $rc; digest $d, job 5917906 ${OLD_SPREAD[$name]}: equal $(yesno "$d" ${OLD_SPREAD[$name]})"
  grep -E '^spread: [0-9]+ groups|against the gathered' $OUT/ub/$name.log | sed 's/^/    /'
done
fof3 spr8_np8 $T8 8 "$CAPG,gather=10000"
echo "  spr8_np8: $(grep -o 'FOF3STAT unbind: .*' $OUT/spr8_np8/run.log | grep -o 'digest_subhaloes [0-9a-f]* digest_particles [0-9a-f]*'); job 5917906 spr_np8: $OLD_SPR_NP8; equal: $(yesno "$(grep -o 'FOF3STAT unbind: .*' $OUT/spr8_np8/run.log | grep -o 'digest_subhaloes [0-9a-f]* digest_particles [0-9a-f]*')" "$OLD_SPR_NP8")"
subcheck spr8_np8
fi

if part 1; then
echo "=== 1. 8m-lambb: FoF3 with -L"
fof3 gath8_np8 $T8 8 "$CAPG,gather=0"
fof3 gath8_np16 $T8 16 "$CAPG,gather=0"
fof3 spr8_np16 $T8 16 "$CAPG,gather=10000"
fof3 def8_np16 $T8 16 "eps=$EPS8"
fof3 defspr8_np16 $T8 16 "eps=$EPS8,gather=10000"
for r in gath8_np8 gath8_np16 spr8_np16 def8_np16 defspr8_np16; do
  ld=$(grep -o 'label_digest: n [0-9]* digest [0-9a-f]*' $OUT/$r/run.log | awk '{print $NF}')
  cg=$(grep -o 'FOF3STAT catalog: .* digest [0-9a-f]*' $OUT/$r/run.log | awk '{print $NF}')
  echo "  $r: label_digest unchanged: $(yesno "$ld" $LABEL_DIGEST8), catalogue unchanged: $(yesno "$cg" $CATALOG_DIGEST8); $(line $r 'FOF3STAT unbind_rows' | cut -c1-120)"
done
for r in gath8_np8 gath8_np16 spr8_np16; do
  echo "  $r: digest_subhaloes $(field $r digest_subhaloes) (job 5917906: $SUBHALO_DIGEST8; equal: $(yesno "$(field $r digest_subhaloes)" $SUBHALO_DIGEST8)); sub-halo label digest $(subdigest $r)"
done
echo "  sub-halo label digest the same on 8 and 16 processes: $(yesno "$(subdigest gath8_np8)" "$(subdigest gath8_np16)"); with gather=10000: $(yesno "$(subdigest gath8_np16)" "$(subdigest spr8_np16)")"
echo "  gathered digests on 8 and 16 processes the same: $(yesno "$(gathdig gath8_np8)" "$(gathdig gath8_np16)")"
for r in gath8_np16 spr8_np16; do subcheck $r; done
fi

if part 2; then
echo "=== 2. the plug-in on 8m-lambb"
L8=$(grep -o 'linking_length_exact: [^ ]*' $OUT/gath8_np16/run.log 2>/dev/null | head -1 | awk '{print $2}')
for mode in copyin cells; do
  extra=""; [ $mode = cells ] && extra="-c 6"
  launch host8_$mode 16 $B/fofhost -f $T8 ${L8:+-L $L8} -P 1 -l 12 -m 20 -K -U "$CAPG,gather=0" $extra
  echo "  fofhost host8_$mode (16 processes, linking length ${L8:-from -b 0.2}): exit $RC"
  show host8_$mode
  hd=$(grep -o 'FOFHOST subhalo: in_subhalo [0-9]* label_digest [0-9a-f]*' $OUT/host8_$mode/run.log | awk '{print $NF}')
  hr=$(grep -o 'rows_digest [0-9a-f]*' $OUT/host8_$mode/run.log | awk '{print $2}')
  echo "  host8_$mode: sub-halo label digest $hd, FoF3's $(subdigest gath8_np16): $(yesno "$hd" "$(subdigest gath8_np16)"); rows digest $hr, FoF3's digest_subhaloes $SUBHALO_DIGEST8: $(yesno "$hr" $SUBHALO_DIGEST8)"
done
fi

if part 3; then
echo "=== 3. the lambb gate"
fof3 lambb_np16 $LAMBB 16 "eps=$EPSL"
fof3 lambb_np8 $LAMBB 8 "eps=$EPSL"
fof3 lambbg_np16 $LAMBB 16 "eps=$EPSL,gather=0"
for r in lambb_np16 lambb_np8 lambbg_np16; do
  ld=$(grep -o 'label_digest: n [0-9]* digest [0-9a-f]*' $OUT/$r/run.log | awk '{print $NF}')
  echo "  $r: label_digest $ld (gold $LABEL_DIGEST_LAMBB_P1: $(yesno "$ld" $LABEL_DIGEST_LAMBB_P1)); $(line $r 'FOF3STAT catalog' | cut -c1-110); sub-halo label digest $(subdigest $r)"
done
echo "  gathered groups' digests on 8 and 16 processes the same: $(yesno "$(gathdig lambb_np8)" "$(gathdig lambb_np16)")"
echo "  sub-halo labels: 16 spread vs 8 spread: $(yesno "$(subdigest lambb_np16)" "$(subdigest lambb_np8)"); 16 spread vs 16 gathered: $(yesno "$(subdigest lambb_np16)" "$(subdigest lambbg_np16)")"
for r in lambb_np16 lambbg_np16; do subcheck $r; done
LL=$(grep -o 'linking_length_exact: [^ ]*' $OUT/lambb_np16/run.log 2>/dev/null | head -1 | awk '{print $2}')
launch hostl_copyin 16 $B/fofhost -f $LAMBB ${LL:+-L $LL} -P 1 -l 12 -m 20 -K -U "eps=$EPSL"
echo "  fofhost hostl_copyin (16 processes, linking length ${LL:-from -b 0.2}): exit $RC"
show hostl_copyin
hd=$(grep -o 'FOFHOST subhalo: in_subhalo [0-9]* label_digest [0-9a-f]*' $OUT/hostl_copyin/run.log | awk '{print $NF}')
hr=$(grep -o 'rows_digest [0-9a-f]*' $OUT/hostl_copyin/run.log | awk '{print $2}')
echo "  plug-in on lambb: sub-halo label digest $hd, FoF3 on 16: $(subdigest lambb_np16) ($(yesno "$hd" "$(subdigest lambb_np16)")); rows digest $hr, FoF3 digest_subhaloes $(field lambb_np16 digest_subhaloes) ($(yesno "$hr" "$(field lambb_np16 digest_subhaloes)"))"
fi
echo "=== done $(date +%T)"
