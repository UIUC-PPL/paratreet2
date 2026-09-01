#!/usr/bin/env python3
"""relay40: the SHAPE of a quiescence-detection settle, from untraced wakeups.

Inside a QD settle there are zero traced events -- QdMsg handlers are converse
handlers and Projections never sees them.  But every QD message that lands on a
PE makes that PE leave the idle loop for a microsecond, and END_IDLE(15) /
BEGIN_IDLE(14) ARE recorded.  So the QD rounds show up as waves of near
simultaneous idle-exits across the machine.

This bins idle-exits over a window and reports, per bin: how many wakeups, how
many distinct PEs, and how many distinct PROCESSES.  A round that touches every
PE is a broadcast; a handful of PEs is the upward report chain.

Method taken from charm-notes/reconverse-qd-latency.md (Anvil, 2026-07-30),
where 25.5 ms spacing between waves identified one QD round per wave.
"""
import gzip, glob, os, re, sys, collections
import multiprocessing as mp
from relay18_state import pe_of

def scan(args):
    path, lo, hi, binw = args
    pe = pe_of(path)
    out = collections.Counter()
    with gzip.open(path,'rt') as f:
        f.readline()
        for line in f:
            sp = line.split()
            if sp[0] == '15':
                t = int(sp[1])
                if lo <= t < hi:
                    out[(t - lo) // binw] += 1
    return pe, out

def main(d, label, lo_ms, hi_ms, binw_ms=0.5, ppn=7, jobs=48):
    lo, hi = int(lo_ms*1000), int(hi_ms*1000)
    binw = int(binw_ms*1000)
    files = sorted(glob.glob(os.path.join(d,'*.log.gz')))
    nb = (hi - lo) // binw + 1
    tot = [0]*nb
    pes = [set() for _ in range(nb)]
    procs = [set() for _ in range(nb)]
    with mp.Pool(jobs) as p:
        for pe, c in p.imap_unordered(scan,
                [(f, lo, hi, binw) for f in files], chunksize=2):
            for b, n in c.items():
                if b < nb:
                    tot[b] += n; pes[b].add(pe); procs[b].add(pe // ppn)
    npe = len(files)
    print("### %s  idle-exit waves, %.3f - %.3f s, %.1f ms bins, %d PEs / %d procs"
          % (label, lo/1e6, hi/1e6, binw_ms, npe, npe//ppn))
    print("   ms_into   wakeups   PEs   procs   bar")
    prev = None
    for b in range(nb):
        if tot[b] == 0: continue
        t = b*binw_ms
        gap = "" if prev is None else "   (+%.1f ms)" % (t - prev)
        prev = t
        print("  %8.1f   %7d  %4d   %5d   %s%s"
              % (t, tot[b], len(pes[b]), len(procs[b]),
                 '#' * min(60, 1 + tot[b]*60//max(1,max(tot))), gap))
    print("  TOTAL %d wakeups in %d non-empty bins of %d"
          % (sum(tot), sum(1 for v in tot if v), nb))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]),
         float(sys.argv[5]) if len(sys.argv)>5 else 0.5)
