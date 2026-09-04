#!/bin/bash
#SBATCH --account=csc710
#SBATCH --qos=debug
#SBATCH --time=02:00:00
#SBATCH --job-name=fof3_2B
#SBATCH --nodes=16
#SBATCH --ntasks=128
#SBATCH --ntasks-per-node=8
#SBATCH --cpus-per-task=14
#SBATCH --threads-per-core=2
#SBATCH --exclusive
#SBATCH --output=fof3_2B.%j.out
#SBATCH --error=fof3_2B.%j.err

module load craype-x86-trento libfabric/2.3.1 craype-network-ofi perftools-base/24.11.0 \
  xpmem/1.0.1-1.5_1_gfb6998056825 cray-pmi/6.1.15 Core/25.03 tmux/3.4 hsi/default \
  lfs-wrapper/0.0.1 DefApps cmake/3.30.5 hwloc/2.11.1 gcc-native/13.2 craype/2.7.33 \
  cray-dsmml/0.3.0 cray-mpich/8.1.31 cray-libsci/24.11.0 PrgEnv-gnu/8.6.0 \
  darshan-runtime/3.4.6-mpi

export LD_LIBRARY_PATH=/ccs/home/rrao/charm_reconverse/lib:/sw/frontier/spack-envs/cpe24.11-cpu/opt/gcc-13.2/darshan-runtime-3.4.6-evo63a663bvwadj3dgv5vwqgwupxnh43/lib:/sw/frontier/spack-envs/core-25.03/opt/gcc-13.2/hwloc-2.11.1-daogniiyv5buyimip5lfgv6i6zawodef/lib:/opt/cray/pe/papi/7.1.0.4/lib64:/opt/cray/libfabric/2.3.1/lib64

# CXI multi-process-per-node fixes worked out on this run (see conversation history):
#  - job_vni: shares one VNI across the whole multi-node job (single_node_vni is 1-node only)
#  - cray_shasta PMI plugin: required alongside job_vni for correct interconnect setup
#  - LCI_ATTR_BACKEND=ofi / FI_CXI_RX_MATCH_MODE=hybrid: needed for LCI's CXI backend to
#    initialize correctly with multiple ranks sharing a node's NIC. At 8 procs/node the
#    CXI hardware match-table (LE) resources are exhausted without this -- libfabric
#    fails with "LE resources not recovered during flow control" and names this exact
#    var as the fix. (Previously mistyped as FI_CXI_RX_MATH_MODE, which libfabric
#    silently ignores -- worked at 4 procs/node only because hardware-only matching
#    had enough headroom there.)
#  - PMI_MAX_KVS_ENTRIES: default (30) is far too small once +lci_ndevices publishes
#    per-device addresses for every rank; bumped generously for 128 ranks x 14 devices
#  - +backend_poll_thread 2: only every 2nd thread on a process polls the network
#    (default is every thread); needed at 8 procs/node to avoid a "Network completion
#    failed for peer rank N" abort seen once init succeeded.
#  - src/Paratreet.h paratreet::setConfiguration was fixed (added a mutex): it's called
#    by every PE via TreeSpec::receiveConfiguration (a group broadcast), all writing the
#    same per-process Csv main_->config_ shared_ptr concurrently -- an unsynchronized
#    race that intermittently double-freed the outgoing Configuration and segfaulted
#    during tree validation. Rebuild src/ and this example after pulling that fix.
export LCI_ATTR_BACKEND=ofi
export FI_CXI_RX_MATCH_MODE=hybrid
export PMI_MAX_KVS_ENTRIES=32768

INPUT=/lustre/orion/csc710/proj-shared/cosmo25cmb.768g2_dm.001024
PPN=14
# 8 procs/node x lci_ndevices contexts share one NIC. At ndevices=14 (=112 total
# contexts/node) LCI's memory registration hits ENOSPC during init. Halved to 7
# (=56 total contexts/node, matching the working 4-proc/node x 14 configuration)
# per CXI resource-limit troubleshooting.
NDEVICES=7

cd /ccs/home/rrao/paratreet2/examples/fof3
mkdir -p fof3_projections

srun --mpi=cray_shasta --network=job_vni --unbuffered \
  --cpu-bind=none --distribution=block:block \
  ./FoF3 -f "$INPUT" -d oct -u dist -b 0.2 +ppn $PPN \
  +pemap 1-7,65-71,9-15,73-79,17-23,81-87,25-31,89-95,33-39,97-103,41-47,105-111,49-55,113-119,57-63,121-127 \
  +lci_ndevices $NDEVICES \
  +backend_poll_thread 2 \
  +traceroot fof3_projections
