#!/usr/bin/env python3
"""relay84 -- MESSAGE-DELIVERY GAP for union-find entry methods, split by window.

Kale, 2026-08-22: the drain PEs are 97% idle, so tail concurrency cannot be the
payoff for a nodegroup/sharded design. The hypothesis is QUEUING BEHIND WALK
EXECUTIONS during the walk-concurrent cascade -- the same mechanism the
add_size removal exposed. So: measure creation-timestamp -> begin-processing
for the union-find EPs, on the 128 element home PEs, walk window vs drain.

  large in the walk window (tens of us at the median, worse at p99)
      -> a nodegroup targets real latency
  small everywhere
      -> chain concurrency joins chain length as a closed line, and the next
         lever is protocol ROUND-TRIP COUNT (fold the two-phase find, kill flips)

THE PROJLOG TRAP (Kale's warning, and it is real):
  BEGIN_PROCESSING  2 mtype entry time event pe msglen ...
      field 6 (p[5]) is the SOURCE pe. The EXECUTING pe is the file the record
      came from. Reading p[5] as the executor inverts the whole measurement.
  CREATION          1 mtype entry time event pe msglen
      p[5] is the creating pe, i.e. the file's own pe.
  Match key is (source_pe, event); gap = begin_time - creation_time, in us.

Only the 128 home PEs are read: UnionFindLib is one element per process placed
on the process's first PE, so every union-find message is home-PE -> home-PE.
Verified empirically -- PEs that are not multiples of ppn execute zero
insertDataFindBoss. union_request(s) come from the application on arbitrary
PEs, so those are reported with their match rate and excluded if unmatched.
"""
import sys, os, glob, gzip, re
from multiprocessing import Pool

GROUPS = {
    'find':     r'^insertDataFindBoss',
    'add_size': r'^add_size',
    'submit':   r'^union_request',
    'label':    r'^need_boss|^set_component',
}

def ep_map(d):
    sts = [f for f in glob.glob(os.path.join(d,'*.sts')) if not f.endswith('.sum.sts')]
    sts = sts[0] if sts else glob.glob(os.path.join(d,'*.sts'))[0]
    g = {k: set() for k in GROUPS}
    for line in open(sts):
        m = re.match(r'ENTRY\s+\S+\s+(\d+)\s+"([^"]*)"', line)
        if not m: continue
        eid, nm = int(m.group(1)), m.group(2)
        for k, rx in GROUPS.items():
            if re.search(rx, nm): g[k].add(eid)
    return g

def scan(args):
    path, ids = args
    pe = int(re.search(r'\.(\d+)\.log\.gz$', path).group(1))
    creations = {}   # event -> time      (this pe is the creator)
    begins = []      # (entry, begin_time, src_pe, event)
    try:
        with gzip.open(path, 'rt') as f:
            for line in f:
                c = line[0]
                if c != '1' and c != '2': continue
                if line[1] != ' ': continue
                p = line.split()
                if len(p) < 6: continue
                try:
                    entry = int(p[2]); t = int(p[3]); ev = int(p[4]); src = int(p[5])
                except ValueError: continue
                if entry not in ids: continue
                if c == '1':
                    creations[ev] = t
                else:
                    begins.append((entry, t, src, ev))
    except Exception:
        return None
    return pe, creations, begins

def q(v, f):
    if not v: return 0
    return v[min(len(v)-1, int(f*len(v)))]

def main(d, label, uf_lo_bin, walk99_bin, uf_hi_bin, ppn=7):
    g = ep_map(d)
    ids = set().union(*g.values())
    rev = {}
    for k, s in g.items():
        for e in s: rev[e] = k
    files = sorted(glob.glob(os.path.join(d, '*.log.gz')))
    home = []
    for f in files:
        pe = int(re.search(r'\.(\d+)\.log\.gz$', f).group(1))
        if pe % ppn == 0: home.append(f)
    print(f"=== {label}\n    {d}")
    print(f"    {len(home)} home PEs of {len(files)} (every {ppn}th)")
    print(f"    windows: WALK bins {uf_lo_bin}..{walk99_bin}   DRAIN bins {walk99_bin+1}..{uf_hi_bin}")
    with Pool(16) as p:
        res = [r for r in p.map(scan, [(f, ids) for f in home], chunksize=2) if r]
    cre = {}
    for pe, c, _ in res:
        for ev, t in c.items(): cre[(pe, ev)] = t
    W = (uf_lo_bin*1000, (walk99_bin+1)*1000)
    D = ((walk99_bin+1)*1000, (uf_hi_bin+1)*1000)
    # TWO CLASSIFICATIONS, because they answer different questions and a
    # message created late in the walk but delivered in the drain would
    # otherwise charge its whole walk-accrued wait to the drain:
    #   by SENT   -- when the message was created (attributes the wait to the
    #                window that caused it).  This is the one to read.
    #   by RECVD  -- when it began executing (what a naive split gives).
    gaps  = {k: {'walk': [], 'drain': []} for k in GROUPS}   # by SENT
    gapsr = {k: {'walk': [], 'drain': []} for k in GROUPS}   # by RECVD
    cross = {k: 0 for k in GROUPS}      # created in walk, delivered in drain
    seen = {k: [0, 0] for k in GROUPS}   # [total begins, matched]
    def win_of(t):
        if W[0] <= t < W[1]: return 'walk'
        if D[0] <= t < D[1]: return 'drain'
        return None
    for _, _, begins in res:
        for entry, t, src, ev in begins:
            k = rev.get(entry)
            if k is None: continue
            wr = win_of(t)
            if wr is None: continue
            seen[k][0] += 1
            ct = cre.get((src, ev))
            if ct is None: continue
            seen[k][1] += 1
            ws = win_of(ct)
            gapsr[k][wr].append(t - ct)
            if ws is not None: gaps[k][ws].append(t - ct)
            if ws == 'walk' and wr == 'drain': cross[k] += 1
    for tag, G in (('BY SENT  (window the message was CREATED in)', gaps),
                   ('BY RECVD (window it began executing in)', gapsr)):
        print(f"\n    ---- {tag}")
        print(f"    {'group':10s} {'window':6s} {'n':>9s} {'p50':>8s} {'p90':>8s} {'p99':>9s} {'max':>9s} {'mean':>8s}   (us)")
        for k in GROUPS:
            for win in ('walk', 'drain'):
                v = sorted(G[k][win])
                if not v:
                    print(f"    {k:10s} {win:6s} {0:>9d}        -"); continue
                print(f"    {k:10s} {win:6s} {len(v):9,d} {q(v,.50):8,d} {q(v,.90):8,d} {q(v,.99):9,d} {v[-1]:9,d} {sum(v)//len(v):8,d}")
    print()
    for k in GROUPS:
        tot, mat = seen[k]
        if tot:
            note = "   (union_request comes from app PEs outside the home set)" if k=='submit' and mat<tot*0.95 else ""
            print(f"    {k:10s} matched {mat:,}/{tot:,} = {100*mat/tot:.1f}%   created-in-walk delivered-in-drain: {cross[k]:,}{note}")
    # shape: where does the mass sit?
    print(f"\n    SHAPE of the find gap, BY SENT (fraction at or under each bound)")
    print(f"    {'window':6s} {'<=10us':>8s} {'<=100us':>8s} {'<=1ms':>8s} {'<=10ms':>8s} {'<=50ms':>8s}")
    for win in ('walk', 'drain'):
        v = sorted(gaps['find'][win])
        if not v: continue
        import bisect
        n = len(v)
        f = lambda b: 100.0*bisect.bisect_right(v, b)/n
        print(f"    {win:6s} {f(10):7.1f}% {f(100):7.1f}% {f(1000):7.1f}% {f(10000):7.1f}% {f(50000):7.1f}%")
    print()

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]),
         int(sys.argv[6]) if len(sys.argv) > 6 else 7)
