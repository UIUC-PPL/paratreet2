#!/usr/bin/env python3
"""relay88 -- CACHE TRAFFIC during the walk: per-PE counts and BYTES.

relay87 showed the walk phase is fetch-bound (cache EPs 28-34% of window wall
against walk EPs 8-10%), so this characterises the traffic itself.

BYTES come from the BEGIN_PROCESSING record's msglen field:
    2 mtype entry time event pe msglen ...      -> p[6] is msglen
and the executing PE is the FILE, not p[5] (p[5] is the source).

WHAT THIS CAN AND CANNOT SEE.  The trace carries message counts and sizes but
NOT the keys being fetched, so per-key duplicate and refetch fractions are not
derivable here -- they need cache-manager instrumentation.  Distinct fetched
content IS already reported by the application's own FOF3STAT cache line
(CacheManager::cacheStats), at PROCESS granularity, which is the correct
granularity because CacheManager is a NODEGROUP: the cache is shared by all
PEs of a process, so a key fetched by one PE is already deduped for the rest.
"""
import sys, os, glob, gzip, re
from multiprocessing import Pool

GROUPS = {'requestNodes': r'^requestNodes', 'addCache': r'^addCache',
          'recvTC': r'^recvTC', 'requestData': r'^requestData|^request<FragData>',
          'pack/unpack': r'^dummy_pack_ep|^dummy_unpack_ep'}

def ep_map(d):
    sts = [f for f in glob.glob(os.path.join(d,'*.sts')) if not f.endswith('.sum.sts')]
    sts = sts[0] if sts else glob.glob(os.path.join(d,'*.sts'))[0]
    g = {k: set() for k in GROUPS}
    for line in open(sts):
        m = re.match(r'ENTRY\s+\S+\s+(\d+)\s+"([^"]*)"', line)
        if not m: continue
        for k, rx in GROUPS.items():
            if re.search(rx, m.group(2)): g[k].add(int(m.group(1)))
    return g

def scan(args):
    path, ids, lo, hi = args
    pe = int(re.search(r'\.(\d+)\.log\.gz$', path).group(1))
    cnt = {}; byt = {}
    try:
        with gzip.open(path,'rt') as f:
            for line in f:
                if line[0] != '2' or line[1] != ' ': continue
                p = line.split()
                if len(p) < 7: continue
                try: e=int(p[2]); t=int(p[3]); ml=int(p[6])
                except ValueError: continue
                if e not in ids or not (lo <= t < hi): continue
                cnt[e] = cnt.get(e,0)+1
                byt[e] = byt.get(e,0)+ml
    except Exception: return None
    return pe, cnt, byt

def q(v,f): return v[min(len(v)-1,int(f*len(v)))] if v else 0

def main(d, label, lo_ms, hi_ms):
    g = ep_map(d); ids = set().union(*g.values())
    rev = {e:k for k,s in g.items() for e in s}
    files = sorted(glob.glob(os.path.join(d,'*.log.gz')))
    print(f"=== {label}\n    {d}\n    {len(files)} PEs, window {lo_ms}..{hi_ms} ms ({hi_ms-lo_ms} ms)")
    with Pool(16) as p:
        res = [r for r in p.map(scan,[(f,ids,lo_ms*1000,hi_ms*1000) for f in files],chunksize=8) if r]
    print(f"\n    {'group':12s} {'total msgs':>13s} {'total MB':>10s} {'per-PE msgs':>26s} {'per-PE MB':>22s}")
    print(f"    {'':12s} {'':>13s} {'':>10s} {'mean':>8s}{'p90':>9s}{'max':>9s} {'mean':>7s}{'p90':>7s}{'max':>8s}")
    grand_msgs = grand_bytes = 0
    for k in GROUPS:
        eids = g[k]
        if not eids: continue
        per_c = sorted(sum(c.get(e,0) for e in eids) for _,c,_ in res)
        per_b = sorted(sum(b.get(e,0) for e in eids) for _,_,b in res)
        tc, tb = sum(per_c), sum(per_b)
        grand_msgs += tc; grand_bytes += tb
        print(f"    {k:12s} {tc:13,d} {tb/1e6:10.1f} {sum(per_c)/len(per_c):8.0f}{q(per_c,.9):9,d}{per_c[-1]:9,d}"
              f" {sum(per_b)/len(per_b)/1e6:7.2f}{q(per_b,.9)/1e6:7.2f}{per_b[-1]/1e6:8.2f}")
        if per_c and sum(per_c):
            print(f"    {'':12s} max/mean msgs {per_c[-1]/(sum(per_c)/len(per_c)):5.2f}x   "
                  f"max/mean bytes {per_b[-1]/max(1e-9,sum(per_b)/len(per_b)):5.2f}x   "
                  f"mean msg size {tb/max(1,tc):8.0f} B")
    print(f"\n    ALL LISTED: {grand_msgs:,} msgs, {grand_bytes/1e6:.1f} MB, "
          f"{grand_bytes/1e9/ (len(res)/7):.2f} GB per process")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
