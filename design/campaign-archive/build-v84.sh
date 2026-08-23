#!/bin/bash
# relay89 -- cache-touch census build binaries from CLEAN trees at the landed refs.
#   paratreet2 main   a65da5a
#   unionfind  master cd2d9c8   (relay79 patch pair as landed, plus
#                                local_union size gating + prune_components
#                                abort under FOF_UF_SIZES=0)
# Working trees are CLEAN -- the local patch was discarded before pulling, so
# these binaries reproduce the landed code and nothing else.
#
# FULL SCRUB and the regenerated-header gate: unionFindLib.ci carries ufstat_*
# and wave_arm, so a stale unionFindLib.decl.h is still the live trap.
set -eu
source /opt/cray/pe/lmod/lmod/init/bash
module load craype-x86-trento libfabric/2.3.1 craype-network-ofi \
  perftools-base/24.11.0 xpmem/1.0.1-1.5_1_gfb6998056825 cray-pmi/6.1.15 \
  Core/25.03 hwloc/2.11.1 gcc-native/13.2 craype/2.7.33 cray-dsmml/0.3.0 \
  cray-mpich/8.1.31 cray-libsci/24.11.0 PrgEnv-gnu/8.6.0 rocm/6.2.4 \
  cmake/3.30.5 2>&1 | tail -1
M=$HOME/software/merged
export ROCM_PATH=/opt/rocm-6.2.4; export PATH=/opt/rocm-6.2.4/bin:$PATH
export KOKKOS_DIR=$HOME/kokkos
BASE=$(echo "${LD_LIBRARY_PATH:-}" | tr ':' '\n' | grep -v "^$HOME/software/" | grep -v '^$' | paste -sd:)
P=$M/paratreet2; U=$M/unionfind

echo "############ REFS"
echo "  paratreet2 $(git -C $P rev-parse --short HEAD)  dirty=$(git -C $P status --porcelain -uno | grep -v ' utility$' | wc -l)"
echo "  unionfind  $(git -C $U rev-parse --short HEAD)  dirty=$(git -C $U status --porcelain -uno | wc -l)"
[ "$(git -C $P rev-parse --short HEAD)" = "a65da5a" ] || { echo "  !!! paratreet2 not a65da5a"; exit 2; }
[ "$(git -C $U rev-parse --short HEAD)" = "cd2d9c8" ] || { echo "  !!! unionfind not cd2d9c8"; exit 2; }
echo "  (trees intentionally dirty: relay87 census + relay89 cache-touch patches)"
echo "  (paratreet2 dirty by design: relay89 patch)"

scrub () {
  rm -f $P/src/*.o $P/src/*.a $P/src/*.decl.h $P/src/*.def.h
  rm -f $P/fof/*.o $P/fof/*.a $P/fof/*.decl.h $P/fof/*.def.h
  rm -f $P/fof/gpu/*.o $P/fof/gpu/*.a
  rm -f $P/examples/fof3/*.o $P/examples/fof3/*.decl.h $P/examples/fof3/*.def.h $P/examples/fof3/FoF3
  rm -f $U/*.o $U/*.a $U/*.decl.h $U/*.def.h $U/prefixLib/*.o $U/prefixLib/*.a
  rm -f $M/htram/*.o $M/htram/*.a $M/htram/*.decl.h $M/htram/*.def.h
  local n=$(find $P $U $M/htram -name '*.o' -o -name '*.a' 2>/dev/null | grep -v '/\.git/' | grep -v '/utility/' | wc -l)
  echo "  scrub: $n remain (want 0)"; [ "$n" = 0 ] || exit 9
}

hdr_gate () {
  local D=$U/unionFindLib.decl.h
  [ -f "$D" ] || { echo "  !!! decl.h never regenerated"; exit 3; }
  echo "  REGENERATED-HEADER GATE  mtime $(date -r $D '+%H:%M:%S')"
  for sym in wave_arm ufstat_mark compression_wave short_circuit_parent; do
    local n=$(grep -c "$sym" $D || true); echo "     $sym $n (>0)"
    [ "$n" -gt 0 ] || { echo "     !!! stale decl.h"; exit 3; }
  done
  local lg=$(grep -c "set_component(const uint64_t &arrIdx, long compNum" $D || true)
  local ig=$(grep -c "set_component(const uint64_t &arrIdx, int compNum"  $D || true)
  echo "     long compNum $lg (>0)   int compNum $ig (0)"
  [ "$lg" -gt 0 ] && [ "$ig" -eq 0 ] || { echo "     !!! decl header truncating"; exit 3; }
}

build_one () {   # build_one <out> <gpu 0|1> <charmdir> <projections 0|1> <make_opts>
  local MO="${5:-}"
  local out=$1 gpu=$2 CH=$3 proj=$4 tag=$(basename $1)
  echo "############ $tag  gpu=$gpu  proj=$proj  charm=$(basename $(dirname $CH))"
  scrub
  export CHARM_HOME=$CH
  export LD_LIBRARY_PATH=$M/lci-install/lib64:$CH/lib:$BASE
  step () { local d=$1; shift; echo "  -- $(basename $d)"; cd $d; "$@" > $M/log-$tag-$(basename $d).txt 2>&1 || { echo "!!! FAILED in $d"; tail -25 $M/log-$tag-$(basename $d).txt; exit 1; }; }
  step $M/htram      make unionfind_smp CHARMC_SMP=$CHARM_HOME/bin/charmc
  step $U/prefixLib  make CHARM_DIR=$CHARM_HOME PARENT_DIR=$M PROFILE= AGGREGATION=
  step $U            make CHARM_DIR=$CHARM_HOME PARENT_DIR=$M PROFILE= AGGREGATION=
  hdr_gate
  step $P/src        make MAKE_OPTS="$MO" -j8
  local PJ=""; [ "$proj" = 1 ] && PJ="PROJECTIONS=1 SUMMARY=1"
  if [ "$gpu" = 1 ]; then
    cd $P/fof/gpu && make KOKKOS_DIR=$KOKKOS_DIR > $M/log-$tag-fofgpu.txt 2>&1 || { echo '!!! fof/gpu FAILED'; tail -25 $M/log-$tag-fofgpu.txt; exit 1; }
    cd $P/fof && make GPU=1 KOKKOS_DIR=$KOKKOS_DIR -j8 > $M/log-$tag-fof.txt 2>&1 || { echo "!!! fof FAILED"; tail -25 $M/log-$tag-fof.txt; exit 1; }
    cd $P/examples/fof3 && make GPU=1 $PJ KOKKOS_DIR=$KOKKOS_DIR -j8 > $M/log-$tag-fof3.txt 2>&1 || { echo "!!! fof3 FAILED"; tail -30 $M/log-$tag-fof3.txt; exit 1; }
  else
    cd $P/fof && make -j8 > $M/log-$tag-fof.txt 2>&1 || { echo "!!! fof FAILED"; tail -25 $M/log-$tag-fof.txt; exit 1; }
    cd $P/examples/fof3 && make MAKE_OPTS="$MO" $PJ -j8 > $M/log-$tag-fof3.txt 2>&1 || { echo "!!! fof3 FAILED"; tail -30 $M/log-$tag-fof3.txt; exit 1; }
  fi
  cp -f $P/examples/fof3/FoF3 $out
  local sz=$(strings $out | grep -c FOF_UF_SIZES || true)
  local sk=$(strings $out | grep -c addsize_SKIPPED || true)
  local pr=$(strings $out | grep -c 'requires FOF_UF_SIZES=1' || true)
  local bp=$(strings $out | grep -c boss_count_prefix || true)
  local tp=$(nm -C $out | grep -ci TraceProjections || true)
  local fg=$(nm -C $out | grep -c 'fofgpu::' || true)
  echo "  ARTIFACT GATE  md5 $(md5sum $out | cut -d' ' -f1)"
  local lc=$(strings $out | grep -c FOF_UF_LOCALCOMP || true)
  local hh=$(strings $out | grep -c climb_local_hops_log2 || true)
  echo "     FOF_UF_SIZES $sz (>0)  addsize_SKIPPED $sk (>0)  prune-abort $pr (>0)  boss_count_prefix $bp (0)"
  echo "     FOF_UF_LOCALCOMP $lc (>0)   climb_local_hops_log2 $hh (>0)"
  local tc=$(strings $out | grep -c 'FOF3STAT cache_touch:' || true)
  local td=$(strings $out | grep -c 'cache_touch_depth' || true)
  echo "     cache_touch strings $tc / $td   MAKE_OPTS='$MO'"
  [ "$tc" -gt 0 ] && [ "$td" -gt 0 ] || { echo "     !!! touch report not linked"; exit 4; }
  local ce=$(strings $out | grep -c edge_INTER_NODE || true)
  local pp=$(strings $out | grep -c FOF_PROCS_PER_PNODE || true)
  echo "     edge_INTER_NODE $ce (>0)   FOF_PROCS_PER_PNODE $pp (>0)"
  [ "$ce" -gt 0 ] && [ "$pp" -gt 0 ] || { echo "     !!! cross-node census not linked"; exit 4; }
  local gs=$(strings $out | grep -c 'FOF3STAT gather:' || true)
  local gm=$(nm -C $out | grep -c runFoFPhase3Staged || true)
  local gp=$(strings $out | grep -c 'dist, serial' || true)
  echo "     FOF3STAT-gather $gs (>0)   runFoFPhase3Staged $gm (>0)   -u help $gp"
  [ "$gs" -gt 0 ] && [ "$gm" -gt 0 ] || { echo "     !!! gather mode not linked"; exit 4; }
  [ "$lc" -gt 0 ] && [ "$hh" -gt 0 ] || { echo "     !!! new sharding measurement not linked"; exit 4; }
  echo "     TraceProjections $tp (want $([ $proj = 1 ] && echo '>0' || echo 0))   fofgpu $fg (want $([ $gpu = 1 ] && echo '>0' || echo 0))"
  [ "$sz" -gt 0 ] && [ "$sk" -gt 0 ] && [ "$pr" -gt 0 ] || { echo "     !!! sizes knob / prune abort not linked"; exit 4; }
  [ "$bp" -eq 0 ] || { echo "     !!! boss_count_prefix present"; exit 4; }
  if [ "$proj" = 1 ]; then [ "$tp" -gt 0 ] || { echo "     !!! tracing NOT linked"; exit 4; }
                     else [ "$tp" -eq 0 ] || { echo "     !!! tracing linked into a timing binary"; exit 4; }; fi
  if [ "$gpu" = 1 ]; then [ "$fg" -gt 0 ] || { echo "     !!! fofgpu missing"; exit 4; }
                     else [ "$fg" -eq 0 ] || { echo "     !!! fofgpu in a CPU binary"; exit 4; }; fi
}

PROD=$M/charm-prod/reconverse-linux-x86_64-amd
PTR=$M/charm-prodtr/reconverse-linux-x86_64-amd
build_one $M/FoF3.cpu-prod-v84   0 $PROD 0 ""
build_one $M/FoF3.cpu-touch-v84 0 $PROD 0 "-DFOF_CACHE_TOUCH=1"
echo "############ DONE"
