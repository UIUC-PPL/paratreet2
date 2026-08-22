#!/bin/bash
# relay74 -- THREE-POINT BUILD LADDER for the histogram attribution, plus the
# compression wave.  CPU-only binaries; every arm of the job is CPU best.
#
#   OLD  the existing FoF3.cpu-prod          (NOT rebuilt)
#        paratreet2 7c28411 + unionfind c00032f
#   MID  paratreet2 277ec6d + unionfind 2ad91a8
#        prefix removal + sign fix, histogram pair path STILL the
#        unordered_map (pre-sort)
#   NEW  paratreet2 e2a877e + unionfind 2ad91a8
#        sort-scan pair path + the wave available
#
# unionfind 2ad91a8 for BOTH built points, as instructed: the .ci fix and the
# wave are additive, and MID must not be built against anything older than
# 9cf9964 or it under-merges.  MID and NEW therefore differ ONLY in
# paratreet2, which is what makes C-B a clean read of the sort path.
#
# THE WAVE IS FIRED FROM THE PARATREET2 SIDE (e2a877e, at the fireUF2Edges
# barrier), so MID cannot fire it even though its unionfind carries the knob.
# MID is a clean no-wave point.
set -eu
source /opt/cray/pe/lmod/lmod/init/bash
module load craype-x86-trento libfabric/2.3.1 craype-network-ofi \
  perftools-base/24.11.0 xpmem/1.0.1-1.5_1_gfb6998056825 cray-pmi/6.1.15 \
  Core/25.03 hwloc/2.11.1 gcc-native/13.2 craype/2.7.33 cray-dsmml/0.3.0 \
  cray-mpich/8.1.31 cray-libsci/24.11.0 PrgEnv-gnu/8.6.0 rocm/6.2.4 \
  cmake/3.30.5 2>&1 | tail -1
M=$HOME/software/merged; P=$M/paratreet2
export ROCM_PATH=/opt/rocm-6.2.4; export PATH=/opt/rocm-6.2.4/bin:$PATH
BASE=$(echo "${LD_LIBRARY_PATH:-}" | tr ':' '\n' | grep -v "^$HOME/software/" | grep -v '^$' | paste -sd:)
CH=$M/charm-prod/reconverse-linux-x86_64-amd

scrub () {
  rm -f $P/src/*.o $P/src/*.a $P/src/*.decl.h $P/src/*.def.h
  rm -f $P/fof/*.o $P/fof/*.a $P/fof/*.decl.h $P/fof/*.def.h
  rm -f $P/fof/gpu/*.o $P/fof/gpu/*.a
  rm -f $P/examples/fof3/*.o $P/examples/fof3/*.decl.h $P/examples/fof3/*.def.h $P/examples/fof3/FoF3
  rm -f $M/unionfind/*.o $M/unionfind/*.a $M/unionfind/*.decl.h $M/unionfind/*.def.h
  rm -f $M/unionfind/prefixLib/*.o $M/unionfind/prefixLib/*.a
  rm -f $M/htram/*.o $M/htram/*.a $M/htram/*.decl.h $M/htram/*.def.h
  local n=$(find $P $M/unionfind $M/htram -name '*.o' -o -name '*.a' 2>/dev/null | grep -v '/\.git/' | grep -v '/utility/' | wc -l)
  echo "  scrub: $n .o/.a remain outside utility/ (want 0)"
  [ "$n" = 0 ] || exit 9
}

build_cpu () {   # build_cpu <paratreet2-ref> <outname>
  local ref=$1 out=$2 tag=$(basename $2)
  echo "############ $tag   paratreet2 $ref + unionfind $(git -C $M/unionfind rev-parse --short HEAD)"
  git -C $P checkout --quiet $ref
  echo "  paratreet2 HEAD $(git -C $P rev-parse --short HEAD)  dirty=$(git -C $P status --porcelain -uno | wc -l)"
  scrub
  export CHARM_HOME=$CH
  export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:$BASE
  step () { local d=$1; shift; echo "  -- $(basename $d)"; cd $d; "$@" > $M/log-$tag-$(basename $d).txt 2>&1 || { echo "!!! FAILED in $d"; tail -25 $M/log-$tag-$(basename $d).txt; exit 1; }; }
  step $M/htram               make unionfind_smp CHARMC_SMP=$CHARM_HOME/bin/charmc
  step $M/unionfind/prefixLib make CHARM_DIR=$CHARM_HOME PARENT_DIR=$M PROFILE= AGGREGATION=
  step $M/unionfind           make CHARM_DIR=$CHARM_HOME PARENT_DIR=$M PROFILE= AGGREGATION=
  step $P/src                 make -j8
  cd $P/fof && make -j8 > $M/log-$tag-fof.txt 2>&1 || { echo "!!! fof FAILED"; tail -25 $M/log-$tag-fof.txt; exit 1; }
  cd $P/examples/fof3 && make -j8 > $M/log-$tag-fof3.txt 2>&1 || { echo "!!! fof3 FAILED"; tail -30 $M/log-$tag-fof3.txt; exit 1; }
  cp -f $P/examples/fof3/FoF3 $out

  # ---- GATES
  local cc=$(strings $out | grep -c component_count_done || true)
  local bp=$(strings $out | grep -c boss_count_prefix || true)
  local wv=$(strings $out | grep -c FOF_WAVE || true)
  local lf=$(grep -m1 'conf.max_particles_per_leaf' $P/examples/fof3/Main.C | grep -o '[0-9]*')
  echo "  md5 $(md5sum $out | cut -d' ' -f1)  fofgpu $(nm -C $out | grep -c 'fofgpu::')  TraceProjections $(nm -C $out | grep -ci TraceProjections)"
  echo "  strings: component_count_done $cc (>0)   boss_count_prefix $bp (0)   FOF_WAVE $wv"
  echo "  source default leaf: $lf"
  # THE .ci GATE: a stale generated header reproduces the int truncation with
  # no other symptom, so gate on the REGENERATED artefact, not on the build.
  local lg=$(grep -c "set_component(const uint64_t &arrIdx, long compNum" $M/unionfind/unionFindLib.decl.h || true)
  local ig=$(grep -c "set_component(const uint64_t &arrIdx, int compNum" $M/unionfind/unionFindLib.decl.h || true)
  echo "  decl header: long compNum $lg (>0)   int compNum $ig (must be 0)"
  [ "$cc" -gt 0 ] || { echo "  !!! GATE component_count_done"; exit 3; }
  [ "$bp" -eq 0 ] || { echo "  !!! GATE boss_count_prefix"; exit 3; }
  [ "$lg" -gt 0 ] && [ "$ig" -eq 0 ] || { echo "  !!! GATE decl header still int"; exit 3; }
  [ "$lf" = "32" ] || { echo "  !!! GATE default leaf is $lf not 32"; exit 3; }
}

build_cpu 277ec6d $M/FoF3.cpu-MID
build_cpu e2a877e $M/FoF3.cpu-NEW
git -C $P checkout --quiet main
echo "############ paratreet2 restored to $(git -C $P rev-parse --abbrev-ref HEAD) $(git -C $P rev-parse --short HEAD)"
echo "############ OLD, not rebuilt: $(md5sum $M/FoF3.cpu-prod | cut -d' ' -f1)  (want b558cc059f71e43a2cf4afd791b6fef3)"
echo "############ DONE"
