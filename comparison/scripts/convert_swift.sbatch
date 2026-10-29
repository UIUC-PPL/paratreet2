#!/bin/bash
# Convert one snapshot to SWIFT HDF5 ICs. Env: DSET, and --nodes on submit.
#
# Sizing (36 B/particle written: 12 pos + 12 vel + 4 mass + 8 id):
#   cosmo2b     1.98e9  ->   71 GB
#   cosmo25    24.46e9  ->  880 GB
#   romulus25  57.98e9  -> 2088 GB
# Reading is more than that for Tipsy, whose positions and masses live inside
# 36-48 B interleaved records, so both passes touch the whole file.
#SBATCH --account=csc710
#SBATCH --job-name=nc2swift
#SBATCH --time=02:00:00
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=7
#SBATCH --exclusive
#SBATCH --output=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.out
#SBATCH --error=/lustre/orion/csc710/scratch/rrao/compare/logs/%x.%j.err

source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
module load cray-hdf5-parallel/1.14.3.5

: ${DSET:?DSET must be cosmo2b, cosmo25 or romulus25}
case "$DSET" in
  cosmo2b)   IN=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024 ;;
  cosmo25)   IN=/lustre/orion/csc710/proj-shared/cosmo25PLK.2304g_dm.004096 ;;
  romulus25) IN=/lustre/orion/csc710/proj-shared/romulus25.3072g.ad.000352 ;;
  *) echo "FATAL unknown DSET $DSET" >&2; exit 1 ;;
esac
OUT=$CMP/data/${DSET}.hdf5

# Stripe the output WIDE before HDF5 creates it. A default-striped file (one
# OST) would serialise every rank's write onto a single object and dominate
# the conversion; the MPI-IO striping_factor hint alone does not always take,
# so the file is created here with lfs and HDF5 then opens the existing inode.
mkdir -p $CMP/data
rm -f "$OUT"
lfs setstripe -c 64 -S 16M "$OUT" 2>&1 || echo "WARN: lfs setstripe failed; default striping"

echo "### MPICH_OFI_NIC_POLICY=$MPICH_OFI_NIC_POLICY (must NOT be NUMA)"
echo "### $DSET: $IN -> $OUT"
echo "### nodes=$SLURM_JOB_NUM_NODES tasks=$SLURM_NTASKS"
df -h /lustre/orion | tail -1
date +"### start %F %T"
srun --ntasks=$SLURM_NTASKS --cpu-bind=none $CMP/bin/nc2swift -i "$IN" -o "$OUT"
rc=$?
date +"### end %F %T"
echo "### exit $rc"
ls -la "$OUT"
lfs getstripe -c "$OUT" 2>/dev/null | head -2
echo "=== CONVERT COMPLETE ==="
