#!/usr/bin/env python3
"""relay75 -- how DEEP is the union-find drain chain, and what does one hop cost?

The drain is 334 ms of wall carrying 1054 PE-ms of union-find work at 0.93%
machine utilisation -- a ratio of 108x.  That is a dependence chain, not a
load problem.  A compression wave shortens chains, so the quantity that
decides its best-case payoff is: how many SEQUENTIAL hops, and how long is
one hop?

Reads Projections .log.gz directly (format verified in scripts/projlog_tool.py
against VERSION 11.0 traces):
  BEGIN_PROCESSING: 2 mtype entry time event pe msglen recvTime [...]
  END_PROCESSING:   3 mtype entry time event pe msglen time2
  times in microseconds.
"""
import sys, os, glob, gzip, re
from multiprocessing import Pool

def ep_ids(d, rx):
    sts = [f for f in glob.glob(os.path.join(d,'*.sts')) if not f.endswith('.sum.sts')]
    sts = sts[0] if sts else glob.glob(os.path.join(d,'*.sts'))[0]
    out = {}
    for line in open(sts):
        m = re.match(r'ENTRY\s+\S+\s+(\d+)\s+"([^"]*)"', line)
        if m and re.search(rx, m.group(2)): out[int(m.group(1))] = m.group(2)
    return out

def scan(args):
    path, ids, lo_us, hi_us = args
    ev = []
    try:
        with gzip.open(path,'rt') as f:
            for line in f:
                if not line.startswith('2 '): continue
                p = line.split()
                if len(p) < 5: continue
                try:
                    entry = int(p[2]); t = int(p[3])
                except ValueError: continue
                if entry in ids and lo_us <= t <= hi_us:
                    ev.append((t, entry))
    except Exception:
        return None
    return ev

def main(d, lo_ms, hi_ms, npes=48):
    ids = ep_ids(d, r'^insertDataFindBoss|^union_request|^need_boss|^add_size|^set_component')
    print(f"=== chain probe  {d}")
    print(f"    window {lo_ms}-{hi_ms} ms; union-find EPs: " +
          ", ".join(f"{k}:{v.split('(')[0]}" for k,v in sorted(ids.items())))
    files = sorted(glob.glob(os.path.join(d,'*.log.gz')))[:npes]
    print(f"    reading {len(files)} PE logs")
    with Pool(16) as p:
        res = p.map(scan, [(f, set(ids), lo_ms*1000, hi_ms*1000) for f in files], chunksize=2)
    res = [r for r in res if r]
    allev = sorted(e for r in res for e in r)
    print(f"    union-find executions in the window, over these PEs: {len(allev)}")
    if len(allev) < 2: return
    # per-PE hop spacing: consecutive union-find executions on the SAME PE
    gaps = []
    for r in res:
        r = sorted(r)
        for a,b in zip(r, r[1:]):
            g = b[0]-a[0]
            if 0 < g < 50000: gaps.append(g)
    gaps.sort()
    if gaps:
        q = lambda f: gaps[min(len(gaps)-1,int(f*len(gaps)))]
        print(f"    per-PE gap between consecutive union-find executions (us):")
        print(f"      n={len(gaps)}  p10 {q(.1)}  p50 {q(.5)}  p90 {q(.9)}  p99 {q(.99)}  mean {sum(gaps)/len(gaps):.0f}")
    # machine-wide: how many DISTINCT time slots are occupied?  a perfectly
    # serial chain occupies ~ (window / hop) slots
    span = (allev[-1][0]-allev[0][0])/1000.0
    print(f"    executions span {span:.1f} ms of the window")
    if gaps:
        hop = sum(gaps)/len(gaps)
        print(f"    IF the drain is one serial chain of hops of the median gap ({q(.5)} us),")
        print(f"      {hi_ms-lo_ms} ms of drain is about {(hi_ms-lo_ms)*1000/max(1,q(.5)):.0f} sequential hops")
    print()

if __name__ == '__main__':
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]),
         int(sys.argv[4]) if len(sys.argv)>4 else 48)
