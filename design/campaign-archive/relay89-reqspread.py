#!/usr/bin/env python3
"""relay89 -- WHERE DOES EACH PE SEND ITS requestNodes TRAFFIC?

The htram/aggregation question: buffering only pays if requests CONCENTRATE on
few destinations.  At ~10 requests/ms per PE spread over 127 peer processes a
4-item buffer takes ~50 ms to fill and flush-on-idle degrades every send to a
singleton; if the walk's spatial locality piles requests onto a handful of
peers instead, the conclusion flips.

DIRECTION, and it is the trap Kale flagged: requestNodes EXECUTES ON THE
SERVER.  On BEGIN_PROCESSING, p[5] is the SOURCE pe (the requester) and the
executing pe is the FILE.  So inverting the record gives, per requester, the
spread of servers it talked to.
"""
import sys, os, glob, gzip, re, collections
from multiprocessing import Pool

def ep_ids(d, rx):
    sts=[f for f in glob.glob(os.path.join(d,'*.sts')) if not f.endswith('.sum.sts')]
    sts=sts[0] if sts else glob.glob(os.path.join(d,'*.sts'))[0]
    return {int(m.group(1)) for m in
            (re.match(r'ENTRY\s+\S+\s+(\d+)\s+"([^"]*)"',l) for l in open(sts))
            if m and re.search(rx, m.group(2))}

def scan(a):
    path, ids, lo, hi = a
    server = int(re.search(r'\.(\d+)\.log\.gz$', path).group(1))
    out = collections.Counter()
    try:
        with gzip.open(path,'rt') as f:
            for line in f:
                if line[0]!='2' or line[1]!=' ': continue
                p=line.split()
                if len(p)<6: continue
                try: e=int(p[2]); t=int(p[3]); src=int(p[5])
                except ValueError: continue
                if e in ids and lo<=t<hi: out[src]+=1
    except Exception: return None
    return server, out

def q(v,f): return v[min(len(v)-1,int(f*len(v)))] if v else 0

def main(d,label,lo_ms,hi_ms,ppn=7):
    ids=ep_ids(d, r'^requestNodes')
    files=sorted(glob.glob(os.path.join(d,'*.log.gz')))
    span=hi_ms-lo_ms
    print(f"=== {label}\n    {d}\n    {len(files)} PEs, window {lo_ms}..{hi_ms} ms ({span} ms), ppn={ppn}")
    with Pool(16) as p:
        res=[r for r in p.map(scan,[(f,ids,lo_ms*1000,hi_ms*1000) for f in files],chunksize=8) if r]
    # requester -> Counter(server process)
    byreq=collections.defaultdict(collections.Counter)
    pairproc=collections.Counter()
    total=0
    for server,cnt in res:
        sproc=server//ppn
        for req,n in cnt.items():
            byreq[req][sproc]+=n
            pairproc[(req//ppn,sproc)]+=n
            total+=n
    print(f"    total requestNodes executions in window: {total:,}")
    nproc=max(max(c) for c in byreq.values())+1 if byreq else 0
    dist=sorted(len(c) for c in byreq.values())
    print(f"\n    DISTINCT DESTINATION PROCESSES per requesting PE (of {nproc})")
    print(f"      min {dist[0]}  p10 {q(dist,.1)}  median {q(dist,.5)}  p90 {q(dist,.9)}  max {dist[-1]}"
          f"   requesting PEs {len(dist)}")
    # concentration: share of a requester's traffic on its top-k destinations
    for k in (1,4,8,16):
        sh=sorted(100.0*sum(sorted(c.values(),reverse=True)[:k])/max(1,sum(c.values()))
                  for c in byreq.values())
        print(f"      share of a PE's requests on its top-{k:2d} destinations:"
              f"  median {q(sh,.5):5.1f}%   p10 {q(sh,.1):5.1f}%   min {sh[0]:5.1f}%")
    # arrival rate on the busiest (source PE, destination process) pair
    perpe=sorted(sum(c.values()) for c in byreq.values())
    print(f"\n    REQUESTS PER REQUESTING PE over the window: median {q(perpe,.5):,}"
          f"  max {perpe[-1]:,}   -> median rate {q(perpe,.5)/span:.2f} /ms")
    top=sorted(((n,pr) for pr,n in pairproc.items()), reverse=True)[:5]
    print(f"    busiest (source PROCESS -> dest PROCESS) pairs, and fill time for a 4-item buffer:")
    for n,(a,b) in top:
        rate=n/span
        print(f"      P{a} -> P{b}: {n:,} reqs, {rate:8.2f} /ms  -> 4-item buffer fills in {4/rate:7.2f} ms")
    # per (requesting PE -> dest process) which is what a per-PE buffer would see
    pairpe=collections.Counter()
    for req,c in byreq.items():
        for sp,n in c.items(): pairpe[(req,sp)]+=n
    ppv=sorted(pairproc.values())
    print(f"    per (source PROCESS -> dest PROCESS) channel -- the granularity a NODEGROUP")
    print(f"      aggregator would see, since the cache is per process:")
    print(f"      channels {len(pairproc):,}   median {q(ppv,.5):,} reqs -> {q(ppv,.5)/span:6.2f} /ms"
          f"  (4-item buffer fills in {4/max(1e-9,q(ppv,.5)/span):5.2f} ms)")
    print(f"      p10 {q(ppv,.1):,} -> {q(ppv,.1)/span:6.2f} /ms   p90 {q(ppv,.9):,} -> {q(ppv,.9)/span:6.2f} /ms")
    pv=sorted(pairpe.values())
    print(f"    per (requesting PE -> dest PROCESS) channel: median {q(pv,.5)} reqs over {span} ms"
          f"  -> {q(pv,.5)/span:.3f} /ms, 4-item buffer fills in {4/max(1e-9,q(pv,.5)/span):.0f} ms")
    print(f"      channels: {len(pairpe):,}   busiest channel {pv[-1]:,} reqs ({pv[-1]/span:.2f} /ms)")

if __name__=='__main__':
    main(sys.argv[1],sys.argv[2],int(sys.argv[3]),int(sys.argv[4]))
