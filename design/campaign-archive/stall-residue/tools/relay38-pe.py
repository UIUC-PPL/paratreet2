#!/usr/bin/env python3
"""relay38: raw, fully-decoded dump of ONE PE's log over a time window.

relay37 showed the 102 ms window ends when PE 0 creates a 128-way multicast.
The question this answers is what PE 0 (or any named PE) is doing for the 102
ms BEFORE that: idle, in overhead, or inside something untraced.  Nothing is
filtered -- idle records and user events are included, which is the point.
"""
import gzip, glob, os, re, sys

NAMES = {'0':'?','1':'CREATION','2':'BEGIN_PROC','3':'END_PROC','4':'ENQUEUE',
 '5':'DEQUEUE','6':'BEGIN_COMP','7':'END_COMP','8':'BEGIN_INTERRUPT',
 '9':'END_INTERRUPT','10':'MSG_RECV','11':'BEGIN_TRACE','12':'END_TRACE',
 '13':'USER_EVENT','14':'BEGIN_IDLE','15':'END_IDLE','16':'BEGIN_PACK',
 '17':'END_PACK','18':'BEGIN_UNPACK','19':'END_UNPACK','20':'CREATION_MULTICAST',
 '21':'USER_SUPPLIED','22':'MEMORY_USAGE','23':'USER_SUPPLIED_NOTE',
 '24':'USER_SUPPLIED_BRACKETED_NOTE','25':'?','26':'USER_STAT',
 '27':'?','28':'BEGIN_USER_EVENT_PAIR','29':'END_USER_EVENT_PAIR'}
TIMED = ('1','2','3','20')

def sts(d):
    ent, usr = {}, {}
    pe = re.compile(r'^ENTRY\s+\w+\s+(\d+)\s+"(.*?)"')
    pu = re.compile(r'^EVENT\s+(\d+)\s+(.*)$')
    for line in open(glob.glob(os.path.join(d,'*.sts'))[0]):
        m = pe.match(line)
        if m: ent[int(m.group(1))] = m.group(2)
        m = pu.match(line)
        if m: usr[int(m.group(1))] = m.group(2).strip()
    return ent, usr

def main(d, pes, lo_ms, hi_ms):
    ent, usr = sts(d)
    lo, hi = int(lo_ms*1000), int(hi_ms*1000)
    for pe in pes:
        f = glob.glob(os.path.join(d, '*.%d.log.gz' % pe))
        f = [x for x in f if re.search(r'\.%d\.log\.gz$' % pe, x)][0]
        print('=== PE %d   %.3f - %.3f s ===' % (pe, lo/1e6, hi/1e6))
        n = 0
        with gzip.open(f,'rt') as fh:
            fh.readline()
            for line in fh:
                sp = line.split()
                t = sp[0]
                try: v = int(sp[3]) if t in TIMED else int(sp[1])
                except (IndexError, ValueError): continue
                if not (lo <= v <= hi): continue
                nm = NAMES.get(t, t)
                if t in ('2','3'):
                    tag = ent.get(int(sp[2]), sp[2])[:46]
                    extra = ' src=%s ev=%s len=%s' % (sp[5] if len(sp)>5 else '-', sp[4], sp[6] if len(sp)>6 else '-') if t=='2' else ''
                    print('  %+9.3f ms  %-12s %s%s' % ((v-lo)/1000.0, nm, tag, extra))
                elif t in ('1','20'):
                    tag = ent.get(int(sp[2]), sp[2])[:40]
                    print('  %+9.3f ms  %-12s %s  ev=%s len=%s %s' %
                          ((v-lo)/1000.0, nm, tag, sp[4], sp[6] if len(sp)>6 else '-',
                           ('nDest=%s' % sp[8]) if t=='20' and len(sp)>8 else ''))
                elif t == '13':
                    print('  %+9.3f ms  %-12s %s' % ((v-lo)/1000.0, nm,
                          usr.get(int(sp[2]), sp[2]) if len(sp)>2 else ''))
                else:
                    print('  %+9.3f ms  %-12s %s' % ((v-lo)/1000.0, nm, ' '.join(sp[2:6])))
                n += 1
        print('  (%d records)' % n)

if __name__ == '__main__':
    main(sys.argv[1], [int(x) for x in sys.argv[2].split(',')],
         float(sys.argv[3]), float(sys.argv[4]))
