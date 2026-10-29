#!/bin/bash
# Sharper localisation of the ArborX hang. Ladder from "does Kokkos/HIP work"
# up to "does the distributed DBSCAN work", with stdbuf so a hang cannot hide
# behind block-buffered stdout (that is what made the first diagnostic
# unreadable: the stock benchmark printf()s without fflush, so at 1 rank its
# parameter block was still in the buffer when SIGKILL arrived).
#SBATCH --account=csc710
#SBATCH --job-name=axdiag2
#SBATCH --time=00:30:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
B=$CMP/arborx-build/benchmarks
WRAP=$CMP/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"
R() { echo "=== $1"; shift; timeout 150 srun --network=single_node_vni --gpus-per-node=8 \
        --cpu-bind=none "$@" 2>&1 | tail -25; echo "### rc=$? (124=timeout)"; }

# 1. Kokkos/HIP + ArborX core, no MPI at all: does a BVH build+query run?
R "1: BVH benchmark (no MPI, no DBSCAN)" --ntasks=1 stdbuf -oL -eL "$WRAP" \
   $B/bvh_driver/ArborX_Benchmark_BoundingVolumeHierarchy.exe \
   --benchmark_filter="HIP.*/1000/.*" --benchmark_min_time=1x

# 2. ArborX core + DBSCAN, single rank, no MPI: isolates FDBSCAN itself.
R "2: serial DBSCAN benchmark, generator" --ntasks=1 stdbuf -oL -eL "$WRAP" \
   $B/cluster/ArborX_Benchmark_DBSCAN.exe --n 20000 --dimension 3 --eps 2 \
   --core-min-size 2

# 3. Distributed DBSCAN, 1 and 2 ranks, generator.
for NP in 1 2; do
  R "3: distributed DBSCAN generator ranks=$NP" --ntasks=$NP stdbuf -oL -eL "$WRAP" \
     $B/cluster/ArborX_Benchmark_DistributedDBSCAN.exe --n 5000 --dimension 3 \
     --num-seq 4 --spacing 4 --core-min-size 2 --eps 2
done

# 4. Distributed TREE benchmark: exercises the same Distributor/MPI layer but
#    not DBSCAN, so it separates "distributed comms broken" from "DBSCAN broken".
R "4: distributed tree driver ranks=2" --ntasks=2 stdbuf -oL -eL "$WRAP" \
   $B/distributed_tree_driver/ArborX_Benchmark_DistributedTree.exe \
   --values 10000 --queries 10000

echo "=== DIAG2 COMPLETE ==="
