#!/usr/bin/env python3
"""relay87 -- PER-PE work distribution for the walk and cache-service EP groups.

Kale, 2026-08-22: does the PE-set split carry the walk imbalance?  Arm A turns
the split off with everything else fixed, so the leaf-128-vs-32 confound in the
GPU/CPU comparison is gone.

Reads .sumd (EP-major, per-PE, 1 ms intervals, value = microseconds spent in
that EP in that interval), which gives per-PE per-group totals directly and far
faster than parsing 896 event logs.  RLE "a+b" means a repeated b TIMES.

Reports, over the walk window, for each group: per-PE busy mean / median / p90 /
max / max-over-mean, the share of window wall, and the identity of the top 5 PEs
-- the last so the hotspot can be compared ACROSS arms (if A and B share their
hotspots the straggler is a property of the decomposition, not of the split).
"""
import sys, os, glob, re
from multiprocessing import Pool

GROUPS = {
    'walk':  r'^goDown|^startDown<|^startDual<|^resumeAfterPause',
    'cache': r'^requestNodes|^addCache|^recvTC|^dummy_pack_ep|^dummy_unpack_ep'
             r'|^request<FragData>|^requestData',
}

def ep_map(d):
    sts = [f for f in glob.glob(os.path.join(d,'*.sts')) if not f.endswith('.sum.sts')]
    sts = sts[0] if sts else glob.glob(os.path.join(d,'*.sts'))[0]
    g = {k: set() for k in GROUPS}
    names = {}
    for line in open(sts):
        m = re.match(r'ENTRY\s+\S+\s+(\d+)\s+"([^"]*)"', line)
        if not m: continue
        eid, nm = int(m.group(1)), m.group(2)
        names[eid] = nm
        for k, rx in GROUPS.items():
            if re.search(rx, nm): g[k].add(eid)
    return g, names

def rle(toks):
    for t in toks:
        if '+' in t:
            v, r = t.split('+'); yield int(v), int(r)
        else:
            yield int(t), 1

def read(args):
    path, groups = args
    pe = int(re.search(r'\.(\d+)\.sumd$', path).group(1))
    with open(path) as f:
        h = f.readline().split()
        nint = int([x for x in h if x.startswith('numIntervals')][0].split(':')[1])
        neps = int([x for x in h if x.startswith('numEPs')][0].split(':')[1])
        exe = f.readline().split()[1:]
    tot = sum(r for _, r in rle(exe))
    assert tot == nint*neps, f"{path}: RLE {tot} != {nint}*{neps}"
    ser = {k: [0.0]*nint for k in groups}
    pos = 0
    for v, r in rle(exe):
        if v:
            s, e = pos, pos+r
            while s < e:
                ep = s//nint
                if ep >= neps: break
                hi = min(e, (ep+1)*nint)
                a, b = s-ep*nint, hi-ep*nint
                for k, ids in groups.items():
                    if ep in ids:
                        t = ser[k]
                        for i in range(a, b): t[i] += v/1000.0   # ms
                s = hi
        pos += r
    return pe, ser

def q(v, f): return v[min(len(v)-1, int(f*len(v)))] if v else 0.0

def main(d, label):
    groups, names = ep_map(d)
    files = sorted(glob.glob(os.path.join(d, '*.sumd')))
    print(f"=== {label}\n    {d}\n    {len(files)} PEs")
    for k in GROUPS:
        print(f"    {k:6s} {len(groups[k]):3d} EPs: " +
              ", ".join(sorted(names[e].split('(')[0] for e in groups[k])[:8]))
    with Pool(16) as p:
        res = p.map(read, [(f, groups) for f in files], chunksize=8)
    nint = max(len(s['walk']) for _, s in res)
    agg = [0.0]*nint
    for _, s in res:
        for i, v in enumerate(s['walk']): agg[i] += v
    live = [i for i in range(nint) if agg[i] > 0]
    lo, hi = live[0], live[-1]
    tot = sum(agg); cum = 0.0; w99 = hi
    for i in range(lo, hi+1):
        cum += agg[i]
        if cum >= 0.99*tot: w99 = i; break
    # TWO WINDOWS, because the share-of-wall numbers depend entirely on which
    # one is used and Kale's local figures (walk 7.6-10.7%, cache 33-35%) come
    # out at roughly double mine on the full span.  The narrow window (to walk
    # CPU 99%) is the one that reproduces his; the full span is where the tail
    # of stragglers actually lives.  Both are reported so neither is ambiguous.
    print(f"\n    walk EPs active bins {lo}..{hi} ({hi-lo+1} ms); walk CPU 99% done by {w99}")
    for wname, wlo, whi in (('NARROW  (to walk-99%)', lo, w99), ('FULL    (all walk activity)', lo, hi)):
        print(f"\n  ================ WINDOW {wname}: bins {wlo}..{whi} ({whi-wlo+1} ms)")
        _report(res, wlo, whi)
    return

def _report(res, lo, hi):
    span_ms = hi - lo + 1
    for k in GROUPS:
        per = sorted(((sum(s[k][lo:hi+1]), pe) for pe, s in res), reverse=True)
        vals = sorted(v for v, _ in per)
        n = len(vals); mean = sum(vals)/n
        print(f"\n    ---- {k.upper()} EPs, per-PE busy over the walk window (ms)")
        print(f"      mean {mean:8.1f}   median {q(vals,.5):8.1f}   p90 {q(vals,.9):8.1f}"
              f"   max {vals[-1]:8.1f}   MAX/MEAN {vals[-1]/mean:5.2f}x")
        print(f"      min  {vals[0]:8.1f}   p10 {q(vals,.1):8.1f}"
              f"   share of window wall: mean {100*mean/span_ms:5.1f}%  max {100*vals[-1]/span_ms:5.1f}%")
        print(f"      top 5 PEs: " + ", ".join(f"PE{pe}={v:.1f}" for v, pe in per[:5]))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
