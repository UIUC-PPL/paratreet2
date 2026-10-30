#!/bin/bash
# paratreet2 issue #3 gate (jobs 5794262, 5794291): the helper-thread affinity scope must act or SAY why
# not in every launch shape. One node, lambb.00500, GPU Replace arm; every
# run must also reproduce 23707197 components.
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=00:15:00
#SBATCH --job-name=fof3_issue3
#SBATCH --nodes=1
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --threads-per-core=2
#SBATCH --output=fof3_affinity_shapes.%j.out
#SBATCH --error=fof3_affinity_shapes.%j.err

source /ccs/home/rrao/paratreet2/examples/fof3/scripts/frontier_env.sh
export LCI_ATTR_BACKEND=ofi FI_CXI_RX_MATCH_MODE=hybrid \
       FI_MR_CACHE_MONITOR=userfaultfd PMI_MAX_KVS_ENTRIES=4194304
export PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1

G=${OUTDIR:-$PWD}
BIN=/ccs/home/rrao/paratreet2/examples/fof3/FoF3
IN=/lustre/orion/csc710/proj-shared/lambb.00500
WRAP=$G/gcd_wrap.sh   # ROCR_VISIBLE_DEVICES by local rank, NUMA-local
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1)
export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
exec "$@"
EOW
chmod +x "$WRAP"
PEMAP=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63

shape() {  # tag "srun shape opts" "charm opts" [extra env]
  local tag=$1 sopts=$2 copts=$3; shift 3
  echo "=== $tag   srun: $sopts   charm: $copts   env: $*"
  # Evidence first: the mask each task actually gets in this shape.
  srun -n8 --network=single_node_vni $sopts bash -c \
    'echo "  rank $SLURM_LOCALID: $(grep Cpus_allowed_list /proc/self/status | cut -f2)"' | sort -k2 -n | head -3
  env "$@" timeout --signal=TERM --kill-after=30 150 \
    srun -n8 --mpi=cray_shasta --network=single_node_vni --unbuffered \
      --distribution=block:block --export=ALL $sopts \
      "$WRAP" "$BIN" -f "$IN" -d oct -u dist -b 0.2 -c stats -l 128 -i 2 \
      $copts +lci_ndevices 7 +backend_poll_thread 1 > "$G/issue3_$tag.log" 2>&1
  echo "  exit: $?"
  grep -m1 'FOF3STAT components:' "$G/issue3_$tag.log" | cut -c1-60
  grep '\[fofgpu\]' "$G/issue3_$tag.log" | sort | uniq -c | sed 's/^/  /'
  grep -E '^Iteration [01] time' "$G/issue3_$tag.log" | sed 's/^/  /'
}

shape A_pemap    "-c14 --threads-per-core=2 --cpu-bind=none"   "+noisomalloc +ppn 7 +pemap $PEMAP"
shape A_pemapoff "-c14 --threads-per-core=2 --cpu-bind=none"   "+noisomalloc +ppn 7 +pemap $PEMAP" FOF_NO_AFFINITY_FIX=1
shape B_cores7   "-c7 --threads-per-core=1 --cpu-bind=cores"   "+noisomalloc +ppn 7"
shape C_thr7     "-c7 --threads-per-core=1 --cpu-bind=threads" "+noisomalloc +ppn 7"
shape D_thr7off  "-c7 --threads-per-core=1 --cpu-bind=threads" "+noisomalloc +ppn 7" FOF_NO_AFFINITY_FIX=1
shape E_cores14  "-c7 --threads-per-core=1 --cpu-bind=cores"   "+noisomalloc +ppn 14"
echo "=== expected: 23707197 components in every shape"
