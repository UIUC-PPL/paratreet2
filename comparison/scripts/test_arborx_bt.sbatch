#!/bin/bash
# Attach rocgdb to the hung ArborX distributed DBSCAN and dump every thread's
# backtrace. Speculating about where a hang is has cost two jobs already;
# this asks the process.
#SBATCH --account=csc710
#SBATCH --job-name=axbt
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
ulimit -c 0

cat > $CMP/scripts/bt_wrap.sh <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
stdbuf -oL -eL "$@" &
pid=$!
sleep 75
if kill -0 $pid 2>/dev/null; then
  echo "=== rank $SLURM_PROCID STILL RUNNING after 75 s; backtrace: ==="
  gdb -p $pid -batch -ex "set pagination off" -ex "thread apply all bt" 2>&1 \
    | grep -vE "^\[New LWP|^warning:|^Reading symbols" | head -70
  kill -9 $pid
else
  echo "=== rank $SLURM_PROCID finished on its own ==="
  wait $pid
fi
EOW
chmod +x $CMP/scripts/bt_wrap.sh

for NP in 2 8; do
  echo "############### ranks=$NP"
  srun --network=single_node_vni --ntasks=$NP --gpus-per-node=8 --cpu-bind=none \
    $CMP/scripts/bt_wrap.sh \
    $B/cluster/ArborX_Benchmark_DistributedDBSCAN.exe --n 5000 --dimension 3 \
    --num-seq 4 --spacing 4 --core-min-size 2 --eps 2 2>&1 | head -160
  echo "### rc=$?"
done
echo "=== BT COMPLETE ==="
