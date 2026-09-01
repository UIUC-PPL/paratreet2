#!/usr/bin/env python3
"""relay32: human-readable dump of a Projections time window.

Names every event type and every entry method from the .sts, sorted by time
across all PEs.  BEGIN_IDLE/END_IDLE are omitted by default -- they are 78% of
the records in this window and carry no information the state changes do not.

Record layouts, verified against src/ck-perf/trace-projections.C:
  1  CREATION           mIdx eIdx time event pe msglen recvtime
  2  BEGIN_PROCESSING   mIdx eIdx time event SRCpe msglen recvtime id0..3 cputime
  3  END_PROCESSING     mIdx eIdx time event pe ...
  20 CREATION_MULTICAST mIdx eIdx time event pe msglen nPEs ...
  14/15 BEGIN/END_IDLE, 16..19 BEGIN/END_PACK/UNPACK: type time pe
"""
import gzip, glob, os, sys, re
import multiprocessing as mp

NAMES = {'1':'CREATION','2':'BEGIN_PROCESSING','3':'END_PROCESSING','4':'ENQUEUE',
 '5':'DEQUEUE','6':'BEGIN_COMPUTATION','7':'END_COMPUTATION','13':'USER_EVENT',
 '14':'BEGIN_IDLE','15':'END_IDLE','16':'BEGIN_PACK','17':'END_PACK',
 '18':'BEGIN_UNPACK','19':'END_UNPACK','20':'CREATION_MULTICAST'}
TIMED = ('1','2','3','20')
SKIP = ('14','15')


def load_sts(d):
    names = {}; pat = re.compile(r'^ENTRY\s+\w+\s+(\d+)\s+"(.*?)"')
    for line in open(glob.glob(os.path.join(d,'*.sts'))[0]):
        m = pat.match(line)
        if m: names[int(m.group(1))] = m.group(2)
    return names


def grab(args):
    path, lo, hi, skip = args
    pe = int(re.search(r'\.(\d+)\.log\.gz$', path).group(1))
    out = []
    with gzip.open(path,'rt') as f:
        f.readline()
        for line in f:
            sp = line.split()
            t = sp[0]
            if t in skip: continue
            try:
                v = int(sp[3]) if t in TIMED else int(sp[1])
            except (IndexError, ValueError):
                continue
            if lo <= v <= hi:
                out.append((v, pe, t, sp))
    return out


def main(d, lo_s, hi_s, outpath, keep_idle=False):
    lo, hi = int(lo_s*1e6), int(hi_s*1e6)
    names = load_sts(d)
    files = sorted(glob.glob(os.path.join(d,'*.log.gz')))
    skip = () if keep_idle else SKIP
    ev = []
    with mp.Pool(16) as p:
        for r in p.map(grab, [(f, lo, hi, skip) for f in files], chunksize=4):
            ev.extend(r)
    ev.sort(key=lambda x: (x[0], x[1]))
    with open(outpath,'w') as o:
        o.write("# Projections events, %s\n" % os.path.basename(d.rstrip('/')))
        o.write("# window %.4f - %.4f s (%.1f ms), %d PEs, %d events\n"
                % (lo_s, hi_s, (hi-lo)/1000.0, len(files), len(ev)))
        o.write("# BEGIN_IDLE/END_IDLE %s\n" % ("kept" if keep_idle else "OMITTED (78% of records here)"))
        o.write("# ppn 7: PE p is rank p%%7 of process p/7; pollers are ranks 0,2,4,6\n")
        o.write("#\n")
        o.write("# %-12s %5s  %-18s %s\n" % ("time_ms","PE","event","entry method / detail"))
        for v, pe, t, sp in ev:
            nm = NAMES.get(t, 'type'+t)
            det = ""
            if t == '2':
                det = "%-52s  from PE %-5s ev %-8s %s B" % (
                    names.get(int(sp[2]), '?')[:52], sp[5], sp[4], sp[6])
            elif t == '3':
                det = names.get(int(sp[2]), '?')[:52]
            elif t == '1':
                det = "%-52s  ev %-8s %s B" % (names.get(int(sp[2]), '?')[:52], sp[4], sp[6])
            elif t == '20':
                det = "%-52s  ev %-8s %s B  to %s PEs" % (
                    names.get(int(sp[2]), '?')[:52], sp[4], sp[6], sp[7] if len(sp)>7 else '?')
            o.write("%14.4f %5d  %-18s %s\n" % (v/1000.0, pe, nm, det))
    print("wrote %s: %d events, %.1f KB" % (outpath, len(ev), os.path.getsize(outpath)/1024.0))


if __name__ == '__main__':
    main(sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4],
         keep_idle=(len(sys.argv) > 5 and sys.argv[5] == 'keepidle'))
