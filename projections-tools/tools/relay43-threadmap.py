#!/usr/bin/env python3
"""relay43: read the thread map and find who shares a core with a pinned PE.

Inputs: the file written by scripts/monitor-threads.py.
Columns: t_s pid tid comm psr sum_exec_ns run_delay_ns pcount cpus_allowed

The verdict columns are the CUMULATIVE kernel counters, differenced between
the first and last sample of each thread:
  sum_exec_ns   how much CPU the thread actually got
  run_delay_ns  how long it sat ON THE RUNQUEUE waiting for a CPU  <-- this is
                the quantity relay39 inferred from the trace
KNOWN LIMIT: monitor-threads.py caches cpus_allowed at first sight, so a
thread first seen BEFORE Charm applied +pemap carries its pre-pinning mask.
psr (sampled every time) is therefore the ground truth for placement.
"""
import sys, collections

def main(path, ppn=7):
    first, last, psrs, meta = {}, {}, collections.defaultdict(set), {}
    for line in open(path):
        if line.startswith('#'): continue
        f = line.split()
        if len(f) < 9: continue
        pid, tid = int(f[1]), int(f[2])
        k = (pid, tid)
        vals = (int(f[5]), int(f[6]), int(f[7]))
        if k not in first: first[k] = vals; meta[k] = (f[3], f[8])
        last[k] = vals
        if f[4] != '-1': psrs[k].add(int(f[4]))
    bypid = collections.defaultdict(list)
    for k in first:
        d_exec = last[k][0] - first[k][0]
        d_wait = last[k][1] - first[k][1]
        d_cnt  = last[k][2] - first[k][2]
        bypid[k[0]].append((k[1], meta[k][0], meta[k][1], d_exec, d_wait,
                            d_cnt, sorted(psrs[k])))
    print("### %s" % path)
    print("### %d processes on this node" % len(bypid))
    # core occupancy from psr
    occ = collections.defaultdict(set)
    for pid, rows in bypid.items():
        for tid, comm, mask, de, dw, dc, ps in rows:
            for p in ps: occ[p].add((pid, tid))
    shared = {c: v for c, v in occ.items() if len(v) > 1}
    print("### logical CPUs that hosted MORE THAN ONE thread: %d" % len(shared))
    for c in sorted(shared):
        print("      CPU %3d : %s" % (c, " ".join("%d/%d" % t for t in sorted(shared[c]))))
    for pid in sorted(bypid):
        rows = sorted(bypid[pid], key=lambda r: -r[4])
        print()
        print("PROCESS pid %d : %d threads" % (pid, len(rows)))
        print("   %-8s %-14s %10s %12s %8s  %-18s %s" %
              ("tid", "comm", "cpu_ms", "runqwait_ms", "switches", "psr(s) seen", "mask"))
        for tid, comm, mask, de, dw, dc, ps in rows:
            m = mask if len(mask) < 20 else "UNPINNED(all)"
            print("   %-8d %-14s %10.1f %12.1f %8d  %-18s %s" %
                  (tid, comm[:14], de/1e6, dw/1e6, dc,
                   ",".join(str(x) for x in ps[:5]) + ("..." if len(ps) > 5 else ""),
                   m))

if __name__ == '__main__':
    main(sys.argv[1])
