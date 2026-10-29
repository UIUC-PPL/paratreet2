#!/bin/bash
# A/B gate for the routed census (FOF_HIST_ROUTED). Runs the SAME binary
# twice in one allocation -- routed on, routed off -- and asserts the two
# component lines are byte-identical. A census rewrite that changes any
# answer is a bug, so this is the go/no-go before spending nodes on timing.
#SBATCH --account=csc710
#SBATCH --job-name=censusab
#SBATCH --time=00:25:00
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --gpus-per-node=8
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err
module load craype-x86-trento libfabric/2.3.1 craype-network-ofi \
  perftools-base/24.11.0 xpmem/1.0.1-1.5_1_gfb6998056825 cray-pmi/6.1.15 \
  Core/25.03 gcc-native/13.2 craype/2.7.33 cray-dsmml/0.3.0 \
  cray-mpich/8.1.31 cray-libsci/24.11.0 PrgEnv-gnu/8.6.0 rocm/6.2.4
# hwloc/2.11.1, cmake/3.30.5 and python/3.13.0 were RETIRED from Frontier
# after the 2026-09 campaign. Lmod aborts the whole compound load when any
# one member is missing, so leaving them here silently prevents rocm from
# loading and every rank dies at "libamdhip64.so.6: cannot open shared
# object file" (jobs 5512027-34). hwloc is not needed: the binary's RPATH
# still resolves libhwloc.so.15 from the spack tree on disk.
[ -n "${ROCM_PATH:-}" ] || { echo "### ABORT: rocm module did not load"; exit 3; }
echo "### rocm=$ROCM_PATH"

B=/lustre/orion/csc710/scratch/rrao/compare
export CHARM_HOME=/ccs/home/rrao/charm_reconverse/reconverse-linux-x86_64-amd-cmp2
export LD_LIBRARY_PATH=$CHARM_HOME/lib:$LD_LIBRARY_PATH
export LCI_ATTR_BACKEND=ofi FI_CXI_RX_MATCH_MODE=hybrid FI_MR_CACHE_MONITOR=userfaultfd
: ${BIN:=/ccs/home/rrao/paratreet2/examples/fof3/FoF3}
: ${DSET:?}
case "$DSET" in
  smoke)    INPUT=/lustre/orion/csc710/scratch/rrao/nctest/100k.nchilada ;;
  cosmo2b)  INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024 ;;
  romulus25) INPUT=/lustre/orion/csc710/proj-shared/romulus25.3072g.ad.000352 ;;
  *) echo "FATAL: unknown DSET"; exit 1 ;;
esac
N=$SLURM_JOB_NUM_NODES; NT=$((N*8))
KVS=$(( NT*NT*5/4 )); [ "$KVS" -lt 4194304 ] && KVS=4194304
export PMI_MAX_KVS_ENTRIES=$KVS
PEMAP=1-7,9-15,17-23,25-31,33-39,41-47,49-55,57-63
ulimit -c 0
WRAP=$B/scripts/gcd_wrap.$SLURM_JOB_ID.sh
cat > "$WRAP" <<'EOW'
#!/bin/bash
GCDS=(4 5 2 3 6 7 0 1); export ROCR_VISIBLE_DEVICES=${GCDS[$SLURM_LOCALID]}
export RANK=$SLURM_PROCID WORLD_SIZE=$SLURM_NTASKS
exec "$@"
EOW
chmod +x "$WRAP"
cd /ccs/home/rrao/paratreet2/examples/fof3 || exit 1
echo "### binary=$BIN md5=$(md5sum $BIN|cut -c1-12) dset=$DSET nodes=$N tasks=$NT"

run_arm() {   # $1 = FOF_HIST_ROUTED value, $2 = label
  local log=$B/logs/censusab_${DSET}_${N}n.$SLURM_JOB_ID.$2.log
  echo "=== arm $2 (FOF_HIST_ROUTED=$1) ==="
  env PARATREET_DEVICE_TREE=1 FOF_GPU_PHASE1=1 FOF_HIST_ROUTED=$1 \
    srun --mpi=cray_shasta --network=job_vni --unbuffered --cpu-bind=none \
      --distribution=block:block --ntasks=$NT --gpus-per-node=8 --export=ALL \
      "$WRAP" "$BIN" -f "$INPUT" -d oct -u dist -c stats -l 128 -i 1 \
      +ppn 7 +pemap $PEMAP +backend lci +lci_ndevices 7 \
      +backend_poll_thread 1 > "$log" 2>&1
  echo "rc=$?"
  grep -m1 "FOF3STAT components:" "$log"
  grep -m1 "touched) components:" "$log"
  grep -m1 "component_histogram" "$log"
  grep -m1 "^Tree traversal:" "$log"
  grep -m1 "^Iteration 0 time:" "$log"
}
run_arm 1 routed
run_arm 0 concat

RA=$(grep -m1 -o "FOF3STAT components:.*" $B/logs/censusab_${DSET}_${N}n.$SLURM_JOB_ID.routed.log)
RB=$(grep -m1 -o "FOF3STAT components:.*" $B/logs/censusab_${DSET}_${N}n.$SLURM_JOB_ID.concat.log)
echo "routed: $RA"
echo "concat: $RB"
if [ -z "$RA" ] || [ -z "$RB" ]; then
  echo "CENSUS_AB FAIL dset=$DSET nodes=$N -- an arm produced no components line"
elif [ "$RA" = "$RB" ]; then
  echo "CENSUS_AB PASS dset=$DSET nodes=$N -- identical"
else
  echo "CENSUS_AB FAIL dset=$DSET nodes=$N -- ARMS DISAGREE"
fi
