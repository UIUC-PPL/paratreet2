#!/usr/bin/env python3
"""relay39: every quiescence-detection episode in a run, and what it cost.

relay37/38 showed the 102 ms gap at 8.102 s is a CkWaitQD: PE 0 posts waitQD,
the machine drains, and then nothing happens anywhere until onQD fires ~80 ms
later.  This finds ALL of them.

QD is driven from PE 0, so PE 0's log carries the whole story:
  CREATION of waitQD()      -- the application asks for quiescence
  BEGIN_PROC onQD(CkQdMsg*) -- quiescence is declared

For each episode we report the QD latency, and (with --machine) the last
moment ANY PE anywhere executed an entry method, which separates "waiting for
the application to drain" from "waiting for the QD algorithm itself".
"""
import gzip, glob, os, re, sys, collections
import multiprocessing as mp

def sts(d):
    ent = {}
    pat = re.compile(r'^ENTRY\s+\w+\s+(\d+)\s+"(.*?)"')
    for line in open(glob.glob(os.path.join(d,'*.sts'))[0]):
        m = pat.match(line)
        if m: ent[int(m.group(1))] = m.group(2)
    return ent

def pe0_episodes(d, ent):
    ids = {i for i, n in ent.items() if n.startswith('waitQD')}
    oid = {i for i, n in ent.items() if n.startswith('onQD')}
    f = [x for x in glob.glob(os.path.join(d,'*.0.log.gz'))
         if re.search(r'\.0\.log\.gz$', x)][0]
    posts, fires = [], []
    with gzip.open(f,'rt') as fh:
        fh.readline()
        for line in fh:
            sp = line.split()
            if sp[0] == '1' and int(sp[2]) in ids:
                posts.append(int(sp[3]))
            elif sp[0] == '2' and int(sp[2]) in oid:
                fires.append(int(sp[3]))
    eps, used = [], 0
    for p in posts:
        while used < len(fires) and fires[used] < p:
            used += 1
        if used < len(fires):
            eps.append((p, fires[used]))
            used += 1
    return eps

def last_busy(args):
    """For each episode, the latest END_PROCESSING on this PE inside it, and
    total busy microseconds inside it."""
    path, eps = args
    last = [-1] * len(eps)
    busy = [0] * len(eps)
    st = None
    with gzip.open(path,'rt') as fh:
        fh.readline()
        for line in fh:
            sp = line.split()
            if sp[0] == '2':
                st = int(sp[3])
            elif sp[0] == '3' and st is not None:
                e = int(sp[3])
                for i, (lo, hi) in enumerate(eps):
                    if e > lo and st < hi:
                        x, y = max(st, lo), min(e, hi)
                        if y > x: busy[i] += y - x
                        if e < hi and e > last[i]: last[i] = e
                st = None
    return last, busy

def main(d, label, jobs=48):
    ent = sts(d)
    eps = pe0_episodes(d, ent)
    files = sorted(glob.glob(os.path.join(d,'*.log.gz')))
    npe = len(files)
    LAST = [-1]*len(eps); BUSY = [0]*len(eps)
    with mp.Pool(jobs) as p:
        for last, busy in p.imap_unordered(last_busy,
                [(f, eps) for f in files], chunksize=2):
            for i in range(len(eps)):
                if last[i] > LAST[i]: LAST[i] = last[i]
                BUSY[i] += busy[i]
    print("=" * 78)
    print("### %s -- %d quiescence-detection episodes, %d PEs" % (label, len(eps), npe))
    print()
    print("  post_s   fire_s   QD_ms   drain_ms  settle_ms  busy_ms  busy%_of_capacity")
    tot = tset = 0
    for i, (lo, hi) in enumerate(eps):
        qd = (hi - lo) / 1000.0
        drain = (LAST[i] - lo) / 1000.0 if LAST[i] > 0 else 0.0
        settle = qd - drain
        cap = (hi - lo) * npe
        print("  %8.3f %8.3f %7.2f   %8.2f  %9.2f  %7.2f   %7.4f%%"
              % (lo/1e6, hi/1e6, qd, drain, settle, BUSY[i]/1000.0,
                 100.0*BUSY[i]/cap if cap else 0))
        tot += qd; tset += settle
    print()
    print("  TOTAL QD latency          %8.1f ms" % tot)
    print("  TOTAL settle (machine already quiet, waiting on QD itself)  %8.1f ms" % tset)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
