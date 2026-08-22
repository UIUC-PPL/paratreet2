#!/usr/bin/env python3
"""relay78 -- WHAT IS ACTUALLY HAPPENING during the 313 ms union-find drain.

Kale's question, 2026-08-22: are the findBoss entry methods leading to more
calls to them on other processors (chain propagation)?  Leading to parent
pointer changes (unions)?  Or being discarded because both find chains reached
the same root?  The remedy depends on the answer.

WHAT THE PROTOCOL DOES (unionFindLib.C, AGGREGATION off in our build, so every
remote hop is a real insertDataFindBoss message):

  union_request(v,w)  -> find_boss1 on v carrying partner w
  find_boss1  parent != -1 : climb (locally in a while loop, remotely by ONE
                             insertDataFindBoss message)          -> PROPAGATES
              parent == -1 : root of v found; send find_boss2 to w -> PROPAGATES
  find_boss2  parent != -1 : climb                                 -> PROPAGATES
              parent == -1 and boss1ID >  self : FLIP, re-issues
                             union_request                         -> PROPAGATES
              parent == -1 and boss1ID <  self : UNION. sets parent,
                             calls add_size                        -> TERMINATES
              parent == -1 and boss1ID == self : SAME ROOT, does
                             nothing at all                        -> TERMINATES

RETRACTED, 2026-08-22: I first wrote that executions minus creations counts
terminations.  IT DOES NOT.  Over a whole run sends equal receives, so that
difference measures only flow across the window boundary.  The branch taken
inside find_boss2 is invisible to Projections and must be COUNTED in the
library -- see the [UFSTAT] census (patches/relay79-unionfind.diff + relay79-paratreet2.diff (cumulative; see patches/APPLY.md)).
What this script is good for is VOLUME per window, which is what it now prints.

One volume relation IS sound and is used in relay78: every union starts
exactly one add_size chain that terminates once at a root, and the census
measured only 7% of add_size calls taking an extra forwarding hop, so
add_size executions in a window are a good proxy for UNIONS in that window.
"""
import sys, os, glob, gzip, re
from multiprocessing import Pool

WANT = r'^insertDataFindBoss|^union_request|^union_requests|^add_size|^need_boss|^set_component|^find_components|^wave_'

def ep_ids(d):
    sts = [f for f in glob.glob(os.path.join(d,'*.sts')) if not f.endswith('.sum.sts')]
    sts = sts[0] if sts else glob.glob(os.path.join(d,'*.sts'))[0]
    out = {}
    for line in open(sts):
        m = re.match(r'ENTRY\s+\S+\s+(\d+)\s+"([^"]*)"', line)
        if m and re.search(WANT, m.group(2)):
            out[int(m.group(1))] = m.group(2).split('(')[0]
    return out

def scan(args):
    path, ids, wins = args
    # counts[w][kind][entry]
    counts = [{1: {}, 2: {}} for _ in wins]
    try:
        with gzip.open(path,'rt') as f:
            for line in f:
                k = line[0]
                if k != '1' and k != '2': continue
                if line[1] != ' ': continue
                p = line.split()
                if len(p) < 5: continue
                try:
                    entry = int(p[2]); t = int(p[3])
                except ValueError: continue
                if entry not in ids: continue
                kind = int(k)
                for wi,(lo,hi) in enumerate(wins):
                    if lo <= t <= hi:
                        d = counts[wi][kind]
                        d[entry] = d.get(entry,0)+1
    except Exception as e:
        return None
    return counts

def main(d, label, drain_lo_ms, drain_hi_ms, walk_lo_ms, walk_hi_ms):
    ids = ep_ids(d)
    files = sorted(glob.glob(os.path.join(d,'*.log.gz')))
    wins = [(walk_lo_ms*1000, walk_hi_ms*1000), (drain_lo_ms*1000, drain_hi_ms*1000)]
    names = [f"WALK  {walk_lo_ms}-{walk_hi_ms} ms", f"DRAIN {drain_lo_ms}-{drain_hi_ms} ms"]
    print(f"=== {label}\n    {d}\n    {len(files)} PE logs; EPs: " +
          ", ".join(f"{v}" for k,v in sorted(ids.items())))
    with Pool(16) as p:
        res = p.map(scan, [(f, set(ids), wins) for f in files], chunksize=4)
    res = [r for r in res if r]
    print(f"    {len(res)} logs read\n")
    for wi, nm in enumerate(names):
        tot = {1: {}, 2: {}}
        for r in res:
            for kind in (1,2):
                for e,c in r[wi][kind].items():
                    tot[kind][e] = tot[kind].get(e,0)+c
        print(f"    ---- {nm}")
        print(f"      {'entry method':28s} {'executions':>12s} {'creations':>12s}")
        for e in sorted(set(tot[1])|set(tot[2])):
            print(f"      {ids[e]:28s} {tot[2].get(e,0):12,d} {tot[1].get(e,0):12,d}")
        asz = [e for e in ids if ids[e]=='add_size']
        if asz:
            U = tot[2].get(asz[0],0)
            print(f"\n      add_size executions here = {U:,}  -> ~UNIONS in this window (see header)")
        print()

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]),
         int(sys.argv[5]), int(sys.argv[6]))
