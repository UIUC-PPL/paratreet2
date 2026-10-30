#!/bin/bash
# NChilada reader check: run the same 100k snapshot through FoF3 three ways --
# as Tipsy, as an NChilada directory with float32 fields, and as an NChilada
# directory with float64 positions/velocities -- and compare the FOF3STAT
# components lines. They must be byte-identical: the converted snapshots hold
# the same particles in the same global order, so FoF has to find the same
# components.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:20:00
#SBATCH --job-name=fof3_nchilada_check
#SBATCH --nodes=1
#SBATCH --ntasks=4
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --exclusive
#SBATCH --output=fof3_nchilada_check.%j.out
#SBATCH --error=fof3_nchilada_check.%j.err

# Modules, CHARM_HOME, LD_LIBRARY_PATH and the binary preflight: one shared file.
source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh

export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export PMI_MAX_KVS_ENTRIES=4194304

DATA=/lustre/orion/csc710/scratch/rrao/nctest
PPN=7

cd /ccs/home/rrao/paratreet2/examples/fof3

run_one() {
  local tag=$1 input=$2
  echo "=== $tag: $input ==="
  # --network=single_node_vni: required for a run confined to one physical node.
  srun --mpi=cray_shasta --network=single_node_vni --unbuffered \
    --cpu-bind=none --distribution=block:block \
    ./FoF3 -f "$input" -d oct -u serial -b 0.2 -c stats +noisomalloc +ppn $PPN \
    2>&1 | tee "/tmp/fof3_nc_$tag.out" | grep -E "FOF3STAT components|Universal bounding box|components \(|Loading input data|Input file"
  echo
}

run_one tipsy      "$DATA/100k.tipsy"
run_one nchilada   "$DATA/100k.nchilada"
run_one nchilada64 "$DATA/100k-f64.nchilada"

echo "=== comparison ==="
for t in tipsy nchilada nchilada64; do
  printf '%-12s %s\n' "$t" "$(grep -m1 'FOF3STAT components:' /tmp/fof3_nc_$t.out)"
done
a=$(grep -m1 'FOF3STAT components:' /tmp/fof3_nc_tipsy.out)
b=$(grep -m1 'FOF3STAT components:' /tmp/fof3_nc_nchilada.out)
c=$(grep -m1 'FOF3STAT components:' /tmp/fof3_nc_nchilada64.out)
if [ -n "$a" ] && [ "$a" = "$b" ] && [ "$a" = "$c" ]; then
  echo "RESULT: PASS -- all three inputs produce identical components"
else
  echo "RESULT: FAIL -- component lines differ"
fi
