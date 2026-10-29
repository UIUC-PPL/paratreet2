#!/bin/bash
# SWIFT stand-alone FoF tool (`fof_mpi`) for the comparison campaign.
#
#  --enable-stand-alone-fof  builds swift_fof.c into `fof` / `fof_mpi`:
#      read a snapshot, run the MPI+threads union-find FoF, write the
#      group catalogue. That is the tool the SWIFT FoF paper describes.
#  --with-parmetis           SWIFT's domain decomposition. WITHOUT it SWIFT
#      falls back to a regular grid split of the top-level cells, which is
#      badly count-imbalanced on clustered cosmology data; leaving it out
#      would penalise SWIFT for a missing dependency rather than measure it.
#  --with-hydro=none         these snapshots are treated as one uniform
#      particle species (see the converter), so no hydro scheme is needed.
#  --disable-hand-vec/--enable-optimization defaults are kept.
set -e
source /lustre/orion/csc710/scratch/rrao/compare/env/env.sh
module load cray-hdf5-parallel/1.14.3.5 parmetis/4.0.3-mpi metis/5.1.0 gsl/2.8 cray-fftw/3.3.10.11 >/dev/null 2>&1
# h5cc shim first: see shim/h5cc for why Cray's h5pcc cannot be used here.
export PATH=$CMP/shim:$PATH
cd $CMP/swift
export CC=cc

[ -f configure ] || ./autogen.sh
make distclean >/dev/null 2>&1 || true
./configure \
  --enable-mpi \
  --enable-stand-alone-fof \
  --with-hydro=none \
  --with-gravity=with-multi-softening \
  --with-hdf5=yes \
  --with-parmetis="$OLCF_PARMETIS_ROOT" \
  --with-metis="$OLCF_METIS_ROOT" \
  --with-fftw="$FFTW_ROOT" \
  --with-gsl="$OLCF_GSL_ROOT" \
  --disable-doxygen-doc \
  --enable-compiler-warnings=yes \
  --with-lustreapi=no \
  --disable-debugging-checks
make -j32
echo "SWIFT_BUILD_RC=$?"
ls -la fof_mpi fof 2>/dev/null
