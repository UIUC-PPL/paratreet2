#!/usr/bin/env python3
"""relay84 v2 -- message-delivery gap with PER-PAIR CLOCK-SKEW CORRECTION.

Kale, 2026-08-22: "There may be skew among clocks across processes. If it looks
similar delay, ignore the tachyons -- messages going back in time -- possibly by
adding fixed offsets. The question is whether receiving PEs are keeping up with
the incoming message rate."

He is right and v1 was measuring skew. Two facts killed the queuing reading:
  - home PEs are only 53.1% busy in the walk window, with execution durations
    of p50 2 us / p90 14 us / p99 74 us. A 53%-busy PE serving 2 us items
    cannot produce a 16.7 ms median queueing delay.
  - the label group showed NEGATIVE gaps: messages arriving before they were
    sent. Tachyons. Projections timestamps are per-PE clocks.

CORRECTION. For each ordered pair (src -> dst) the MINIMUM observed gap over
the whole run is (clock offset + the true floor latency for that pair). It is a
constant per pair, so subtracting it removes the offset and leaves a delay
measured from that pair's own floor. Corrected gap >= 0 by construction, and
the pair minimum itself is reported so the skew magnitude is visible rather
than hidden.

What the corrected number answers: are receiving PEs keeping up? If corrected
delays are small (microseconds) the answer is yes -- the messages are not
waiting, and neither chain concurrency nor delivery latency is the lever.
"""
import sys, os, glob, gzip, re, bisect
from multiprocessing import Pool

GROUPS = {'find': r'^insertDataFindBoss', 'add_size': r'^add_size',
          'submit': r'^union_request', 'label': r'^need_boss|^set_component'}

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
    path, ids = args
    pe = int(re.search(r'\.(\d+)\.log\.gz$', path).group(1))
    cre = {}; beg = []
    try:
        with gzip.open(path, 'rt') as f:
            for line in f:
                c = line[0]
                if (c != '1' and c != '2') or line[1] != ' ': continue
                p = line.split()
                if len(p) < 6: continue
                try: e=int(p[2]); t=int(p[3]); ev=int(p[4]); src=int(p[5])
                except ValueError: continue
                if e not in ids: continue
                if c == '1': cre[ev] = t
                else: beg.append((e, t, src, ev))
    except Exception: return None
    return pe, cre, beg

def q(v, f): return v[min(len(v)-1, int(f*len(v)))] if v else 0

def main(d, label, uf_lo, walk99, uf_hi, ppn=7):
    g = ep_map(d); ids = set().union(*g.values())
    rev = {e: k for k, s in g.items() for e in s}
    files = [f for f in sorted(glob.glob(os.path.join(d,'*.log.gz')))
             if int(re.search(r'\.(\d+)\.log\.gz$', f).group(1)) % ppn == 0]
    print(f"=== {label}   {len(files)} home PEs")
    print(f"    WALK bins {uf_lo}..{walk99}   DRAIN bins {walk99+1}..{uf_hi}")
    with Pool(16) as p:
        res = [r for r in p.map(scan, [(f, ids) for f in files], chunksize=2) if r]
    cre = {}
    for pe, c, _ in res:
        for ev, t in c.items(): cre[(pe, ev)] = t
    # pass 1: raw gaps, keyed by pair, over the WHOLE run
    recs = []          # (group, sent_time, raw_gap, src, dst)
    pairmin = {}
    for dst, _, beg in res:
        for e, t, src, ev in beg:
            k = rev.get(e); ct = cre.get((src, ev))
            if k is None or ct is None: continue
            raw = t - ct
            recs.append((k, ct, raw, src, dst))
            pk = (src, dst)
            if pk not in pairmin or raw < pairmin[pk]: pairmin[pk] = raw
    mins = sorted(pairmin.values())
    print(f"\n    PER-PAIR MINIMUM GAP over {len(pairmin):,} ordered pairs -- this IS the skew")
    print(f"      p01 {q(mins,.01):,}   p50 {q(mins,.50):,}   p99 {q(mins,.99):,}   min {mins[0]:,}   max {mins[-1]:,}  (us)")
    same = [v for (s,dd),v in pairmin.items() if s==dd]
    if same:
        same.sort()
        print(f"      SAME-PE pairs (no cross-clock, so this is a true floor): n={len(same)}"
              f"  p50 {q(same,.5)}  max {same[-1]}")
    W = (uf_lo*1000, (walk99+1)*1000); D = ((walk99+1)*1000, (uf_hi+1)*1000)
    # THE SKEW-FREE MEASUREMENT: src == dst. One clock, no network, so this is
    # purely "did the receiving PE get to it". If these are small the PE is
    # keeping up and any large cross-PE gap lives in the transport, not the
    # scheduler.
    loc = {'walk': [], 'drain': []}
    for k, ct, raw, src, dst in recs:
        if src != dst or k != 'find': continue
        win = 'walk' if W[0] <= ct < W[1] else ('drain' if D[0] <= ct < D[1] else None)
        if win: loc[win].append(raw)
    print(f"\n    SAME-PE find messages -- ONE CLOCK, NO NETWORK (us)")
    for win in ('walk','drain'):
        v = sorted(loc[win])
        if not v: continue
        n=len(v); f=lambda b: 100.0*bisect.bisect_right(v,b)/n
        print(f"      {win:6s} n={n:8,d}  p50 {q(v,.5):5,d}  p90 {q(v,.9):6,d}  p99 {q(v,.99):7,d}  max {v[-1]:8,d}"
              f"   <=10us {f(10):5.1f}%  <=100us {f(100):5.1f}%  <=1ms {f(1000):5.1f}%")
    # and the cross-PE half for contrast
    crossg = {'walk': [], 'drain': []}
    for k, ct, raw, src, dst in recs:
        if src == dst or k != 'find': continue
        win = 'walk' if W[0] <= ct < W[1] else ('drain' if D[0] <= ct < D[1] else None)
        if win: crossg[win].append(raw - pairmin[(src,dst)])
    print(f"    CROSS-PE find messages, pair-corrected (us)")
    for win in ('walk','drain'):
        v = sorted(crossg[win])
        if not v: continue
        n=len(v); f=lambda b: 100.0*bisect.bisect_right(v,b)/n
        print(f"      {win:6s} n={n:8,d}  p50 {q(v,.5):5,d}  p90 {q(v,.9):6,d}  p99 {q(v,.99):7,d}  max {v[-1]:8,d}"
              f"   <=10us {f(10):5.1f}%  <=100us {f(100):5.1f}%  <=1ms {f(1000):5.1f}%")
    out = {k: {'walk': [], 'drain': []} for k in GROUPS}
    for k, ct, raw, src, dst in recs:
        win = 'walk' if W[0] <= ct < W[1] else ('drain' if D[0] <= ct < D[1] else None)
        if win is None: continue
        out[k][win].append(raw - pairmin[(src, dst)])
    print(f"\n    CORRECTED DELAY, by window the message was SENT in (us above that pair's own floor)")
    print(f"    {'group':9s} {'window':6s} {'n':>9s} {'p50':>6s} {'p90':>7s} {'p99':>8s} {'max':>9s} {'mean':>7s}   {'<=10us':>7s} {'<=100us':>8s} {'<=1ms':>7s}")
    for k in GROUPS:
        for win in ('walk','drain'):
            v = sorted(out[k][win])
            if not v: print(f"    {k:9s} {win:6s} {0:>9d}      -"); continue
            n=len(v); f=lambda b: 100.0*bisect.bisect_right(v,b)/n
            print(f"    {k:9s} {win:6s} {n:9,d} {q(v,.5):6,d} {q(v,.9):7,d} {q(v,.99):8,d} {v[-1]:9,d} {sum(v)//n:7,d}"
                  f"   {f(10):6.1f}% {f(100):7.1f}% {f(1000):6.1f}%")
    print()

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]))
