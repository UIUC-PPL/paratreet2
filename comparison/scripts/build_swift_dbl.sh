#!/bin/bash
# Rebuild ONLY src/fof.c (already patched) and relink, then save the binary as
# fof_mpi.dbl and restore the as-published fof_mpi.
#
# Why: SWIFT's max_size disagrees with the FoF3 gold by ~4e-6 (cosmo2b
# 185316849 vs 185317566) and ~2e-5 (romulus25 125859155 vs 125856955). The
# cause is not -ffast-math/-ffp-contract, as first guessed: all seven pairwise
# distance tests in src/fof.c read
#     float dx[3], r2 = 0.0f;
#     dx[k] = pix - pjx;            /* double subtraction, rounded to float */
#     for (k) r2 += dx[k]*dx[k];    /* accumulated in float */
#     if (r2 < l_x2)                /* compared against a DOUBLE l_x2 */
# so SWIFT's effective linking length carries float32's ~6e-8 relative error
# -- the same order as ArborX's float BVH, and the same order as the
# component-count gaps measured in the eps-sensitivity test. This build
# changes those seven declarations to double and nothing else, to confirm the
# offset closes. The published binary (fof_mpi.pub / fof_mpi) is what the
# paper's SWIFT numbers come from; fof_mpi.dbl exists only to attribute the
# discrepancy.
set -e
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
module load cray-hdf5-parallel/1.14.3.5 parmetis/4.0.3-mpi metis/5.1.0 gsl/2.8 cray-fftw/3.3.10.11 >/dev/null 2>&1
export PATH=$CMP/shim:$PATH
cd $CMP/swift
grep -q "CAMPAIGN: published SWIFT uses float here" src/fof.c || {
  echo "FATAL: src/fof.c is not the patched version" >&2; exit 1; }
touch src/fof.c
make -j32
cp -a fof_mpi fof_mpi.dbl
echo "SWIFT_DBL_BUILT md5=$(md5sum fof_mpi.dbl | cut -c1-12)"
# restore the as-published source and binary
cp -a $CMP/swift/src/fof.c.pub src/fof.c
cp -a fof_mpi.pub fof_mpi
echo "RESTORED published fof_mpi md5=$(md5sum fof_mpi | cut -c1-12)"
grep -c "float dx\[3\], r2 = 0.0f;" src/fof.c
