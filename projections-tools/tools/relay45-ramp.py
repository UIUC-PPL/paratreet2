#!/usr/bin/env python3
"""relay45: the delivery ramp of one broadcast, and per-PE lateness inside it.

A Charm broadcast to an array/group shows up in the trace twice:
  - ONE CREATION_MULTICAST on PE 0 with the full destination count
  - ONE CREATION_MULTICAST per PROCESS, with nDest = ppn, emitted by the PE in
    that process that received the message and re-multicasts it locally
The second set is the clean measurement of WHEN EACH PROCESS RECEIVED IT, and
the spread of those times is the ramp.  Per-PE lateness is then measured
against that PE's OWN process receipt, which separates a late process from a
late PE inside an on-time process.

relay44 tried to match executions by (source PE, event id) and that collides;
this uses the per-process multicast record instead, which cannot collide.
"""
import gzip, glob, os, re, sys, collections
import multiprocessing as mp
from relay18_state import pe_of

MS = 1000

def load_sts(d):
    ent = {}
    pat = re.compile(r'^ENTRY\s+\w+\s+(\d+)\s+"(.*?)"')
    for line in open(glob.glob(os.path.join(d,'*.sts'))[0]):
        m = pat.match(line)
        if m: ent[int(m.group(1))] = m.group(2)
    return ent

def scan(args):
    path, ids, lo, hi = args
    pe = pe_of(path)
    mc, ex = [], []
    with gzip.open(path,'rt') as f:
        f.readline()
        for line in f:
            sp = line.split()
            if sp[0] == '20' and int(sp[2]) in ids:
                t = int(sp[3])
                if lo <= t < hi:
                    mc.append((t, pe, int(sp[8]) if len(sp) > 8 else -1))
            elif sp[0] == '2' and int(sp[2]) in ids:
                t = int(sp[3])
                if lo <= t < hi:
                    ex.append((t, pe))
    return mc, ex

def one(d, ent, name, lo_ms, hi_ms, files, ppn, perproc, jobs):
    ids = {i for i, n in ent.items() if n.startswith(name)}
    if not ids:
        print("   no entry method starts with %r" % name); return
    lo, hi = int(lo_ms*MS), int(hi_ms*MS)
    MC, EX = [], []
    with mp.Pool(jobs) as p:
        for m, e in p.imap_unordered(scan, [(f, ids, lo, hi) for f in files],
                                     chunksize=2):
            MC.extend(m); EX.extend(e)
    MC.sort(); EX.sort()
    root = [x for x in MC if x[2] > ppn]
    local = [x for x in MC if 0 < x[2] <= ppn]
    print("=" * 78)
    print("### %s   %.3f - %.3f s" % (name, lo_ms/1000.0, hi_ms/1000.0))
    if root:
        t0, rpe, rnd = root[0]
        print("   root multicast: %.6f s from PE %d, %d destinations"
              % (t0/1e6, rpe, rnd))
    else:
        t0 = local[0][0] if local else lo
        print("   no root multicast in the window; timing from the first local one")
    print("   per-process re-multicasts: %d" % len(local))
    if len(local) > 3:
        d = [(t - t0)/1000.0 for t, _, _ in local]
        print("   THE RAMP: first process at %+.3f ms, last at %+.3f ms,"
              " spread %.0f us over %d processes = %.2f us per process"
              % (d[0], d[-1], (d[-1]-d[0])*1000, len(d),
                 (d[-1]-d[0])*1000/max(1, len(d)-1)))
        step = max(1, len(d)//10)
        print("   decile of process receipt (ms after the root send):")
        print("      " + "  ".join("%.3f" % d[i] for i in range(0, len(d), step)))
        # linearity: compare the observed spread to a straight line
        n = len(d)
        pred = [d[0] + (d[-1]-d[0])*i/(n-1) for i in range(n)]
        err = max(abs(d[i]-pred[i]) for i in range(n))
        print("   worst deviation from a straight line: %.0f us"
              " (a tree would bend, serial injection is straight)" % (err*1000))
    proc_recv = {}
    for t, pe, nd in local:
        proc_recv[pe//ppn] = min(proc_recv.get(pe//ppn, 1<<62), t)
    lags = []
    for t, pe in EX:
        base = proc_recv.get(pe//ppn)
        if base is not None:
            lags.append(((t - base)/1000.0, pe))
    lags.sort()
    if lags:
        print("   %d executions.  Lag AFTER the PE's own process received it:"
              % len(lags))
        q = [lags[int(len(lags)*f)][0] for f in (0, .5, .9, .99)] + [lags[-1][0]]
        print("      p0 %.3f   p50 %.3f   p90 %.3f   p99 %.3f   max %.3f ms" % tuple(q))
        n3 = [x for x in lags if x[0] > 3.0]
        print("      PEs more than 3 ms behind their own process: %d of %d"
              % (len(n3), len(lags)))
        for lg, pe in n3[-10:]:
            print("         PE %4d  proc %3d  rank %d  node %2d   %+.3f ms"
                  % (pe, pe//ppn, pe % ppn, pe//perproc, lg))
    return {pe for lg, pe in lags if lg > 3.0}

def main(d, label, specs, ppn=7, ppnode=8, jobs=48):
    ent = load_sts(d)
    files = sorted(glob.glob(os.path.join(d,'*.log.gz')))
    perproc = ppn*ppnode
    print("### %s -- broadcast ramps, %d PEs, %d per process, %d per node"
          % (label, len(files), ppn, perproc))
    sets = []
    for name, a, b in specs:
        s = one(d, ent, name, a, b, files, ppn, perproc, jobs)
        if s: sets.append((name, s))
    if len(sets) > 1:
        print("=" * 78)
        print("### ARE THE LATE PEs THE SAME ONES EVERY TIME?")
        for i in range(len(sets)):
            for j in range(i+1, len(sets)):
                a, b = sets[i], sets[j]
                inter = a[1] & b[1]
                print("   %-22s (%3d late) vs %-22s (%3d late): %d in common"
                      % (a[0][:22], len(a[1]), b[0][:22], len(b[1]), len(inter)))
        allpe = set().union(*[s for _, s in sets])
        print("   %d distinct PEs late at least once, out of %d"
              % (len(allpe), len(files)))
        cnt = collections.Counter()
        for _, s in sets:
            for pe in s: cnt[pe] += 1
        print("   late in how many of the %d broadcasts: %s"
              % (len(sets), dict(sorted(collections.Counter(cnt.values()).items()))))
        byrank = collections.Counter()
        for pe, n in cnt.items(): byrank[pe % ppn] += n
        print("   late events by rank within the process: %s"
              % dict(sorted(byrank.items())))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], [
        ("verifyEncodedTips", 8290, 8340),
        ("resetPhase3",       8375, 8425),
        ("initUF2",           8435, 8475),
        ("applyTipEncoding",  7840, 7900),
        ("histogramShard",   10720, 10790),
    ])
