#!/usr/bin/env python3
"""relay46: read the new instruments out of the relay39 trace.

Record layouts confirmed against this trace:
  13  USER_EVENT       : 13 <eventid> <time_us> <seq> <pe>
  100 USER_EVENT_PAIR  : 100 <eventid> <time_us> <seq> ...   TWO records per
      bracket, matched by (eventid, seq); the pair's span is the duration.

Events (patches/0018):
  9100 QD wait for STILL_IDLE      bracketed, length = the per-hop idle wait
  9101/9102/9103  QD phase 0/1/2   point
  9104 QD round restart (PE 0)     point
  9105 QD detected (PE 0)          point
  9106 QD started (PE 0)           point
  9110 sched gap, thread OFF CORE  bracketed, length = the gap
  9111 sched gap, thread RUNNING   bracketed, length = the gap
  9112 involuntary ctx switch      point
"""
import gzip, glob, os, re, sys, collections
import multiprocessing as mp
from relay18_state import pe_of

BR = {9100, 9110, 9111}
PT = {9101, 9102, 9103, 9104, 9105, 9106, 9112}

def scan(path):
    pe = pe_of(path)
    open_ = {}
    br = collections.defaultdict(list)   # event -> [(begin, end)]
    pt = collections.Counter()
    with gzip.open(path, 'rt') as f:
        f.readline()
        for line in f:
            if line[0] not in '19':      # cheap reject: only 13 and 100 start 1/9? no
                pass
            sp = line.split()
            t = sp[0]
            if t == '100':
                e, tm, seq = int(sp[1]), int(sp[2]), sp[3]
                k = (e, seq)
                if k in open_:
                    br[e].append((open_.pop(k), tm))
                else:
                    open_[k] = tm
            elif t == '13':
                pt[int(sp[1])] += 1
    return pe, {e: v for e, v in br.items()}, pt

def main(d, label, ppn=7, ppnode=8, jobs=48):
    files = sorted(glob.glob(os.path.join(d, '*.log.gz')))
    npe = len(files)
    perproc = ppn * ppnode
    BRA = collections.defaultdict(list)          # event -> [(pe, begin, end)]
    PTA = collections.Counter()
    perpe = collections.defaultdict(lambda: collections.Counter())
    perpe_us = collections.defaultdict(lambda: collections.Counter())
    with mp.Pool(jobs) as p:
        for pe, br, pt in p.imap_unordered(scan, files, chunksize=2):
            for e, v in br.items():
                for a, b in v:
                    BRA[e].append((pe, a, b))
                    perpe[e][pe] += 1
                    perpe_us[e][pe] += b - a
            PTA.update(pt)
            for e, n in pt.items():
                perpe[e][pe] += n
    print("=" * 78)
    print("### %s -- the relay39 instruments, %d PEs" % (label, npe))
    print()
    print("1. POINT EVENTS, machine-wide")
    names = {9101: "QD phase 0 (ask children)", 9102: "QD phase 1 (report counts)",
             9103: "QD phase 2 (dirty check)", 9104: "QD round restart (PE 0)",
             9105: "QD detected (PE 0)", 9106: "QD started (PE 0)",
             9112: "involuntary ctx switch"}
    for e in sorted(PTA):
        print("   %-32s %8d" % (names.get(e, str(e)), PTA[e]))

    print()
    print("2. BRACKETED EVENTS")
    print("   %-28s %8s %12s %10s %10s %10s" %
          ("event", "count", "total_ms", "mean_ms", "p99_ms", "max_ms"))
    bn = {9100: "QD wait for STILL_IDLE", 9110: "sched gap: OFF CORE",
          9111: "sched gap: RUNNING (long call)"}
    for e in sorted(BRA):
        d_ = sorted((b - a) for _, a, b in BRA[e])
        tot = sum(d_) / 1000.0
        print("   %-28s %8d %12.1f %10.3f %10.3f %10.3f"
              % (bn.get(e, str(e)), len(d_), tot, tot / len(d_),
                 d_[int(len(d_) * .99)] / 1000.0, d_[-1] / 1000.0))

    for e in (9110, 9111):
        if e not in BRA: continue
        d_ = sorted((b - a) for _, a, b in BRA[e])
        print()
        print("3.%d DURATION HISTOGRAM -- %s" % (e - 9109, bn[e]))
        edges = [1, 2, 4, 8, 12, 16, 24, 32, 64, 1 << 30]
        prev = 0
        for x in edges:
            n = sum(1 for v in d_ if prev * 1000 <= v < x * 1000)
            if n:
                print("      %5d - %-6s ms : %7d  %5.1f%%"
                      % (prev, ("%d" % x) if x < (1 << 30) else "inf",
                         n, 100.0 * n / len(d_)))
            prev = x
        pes = perpe[e]
        print("      PEs affected: %d of %d.  Busiest 8: %s"
              % (len(pes), npe, " ".join("PE%d:%d" % (k, v)
                                         for k, v in pes.most_common(8))))
        byrank = collections.Counter()
        for k, v in pes.items(): byrank[k % ppn] += v
        print("      by rank within the process: %s" % dict(sorted(byrank.items())))
        tot_us = sum(perpe_us[e].values())
        print("      total %.1f ms over %d PEs = %.2f ms per PE; machine wall"
              " share %.3f%%" % (tot_us / 1000.0, npe, tot_us / 1000.0 / npe,
                                 0.0))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
