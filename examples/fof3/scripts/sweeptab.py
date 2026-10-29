#!/usr/bin/env python3
"""One row per arm of the 16-node parameter sweep: iteration-1 mean over
reps, spread, and the phase columns that explain any difference.
Warm-up and any arm that does not print the gold component pair are
excluded from the means and listed separately."""
import re, sys, statistics
from collections import defaultdict
GOLD = "424897832 max_size 185317566"
arm_re = re.compile(r'^=== ARM (\S+) rep (\S+) \((.*)\) ===')
pat = {
 'it0': re.compile(r'^Iteration 0 time: ([\d.]+) ms'),
 'it1': re.compile(r'^Iteration 1 time: ([\d.]+) ms'),
 'tb':  re.compile(r'^Tree build and sending leaves: ([\d.]+) ms'),
 'dev': re.compile(r'^FOF3STAT time_s: phase1_device wall ([\d.]+)'),
 'p1':  re.compile(r'^FOF3STAT time_s: phase1 ([\d.]+) .* upwardPass ([\d.]+) loadCache ([\d.]+)'),
 'p3':  re.compile(r'^FOF3STAT time_s: uf2_setup ([\d.]+) phase3_walk ([\d.]+) edge_gather ([\d.]+) uf2 ([\d.]+) relabel ([\d.]+)'),
 'pa':  re.compile(r'^FOF3STAT time_s: phase1_stages reset [\d.]+ register [\d.]+ phaseA ([\d.]+) phaseB ([\d.]+)'),
 'exit':re.compile(r'^exit: (\d+)'),
}
rows, cur = [], None
for p in sys.argv[1:]:
    for line in open(p, errors='replace'):
        m = arm_re.match(line)
        if m:
            if cur: rows.append(cur)
            cur = dict(arm=m.group(1), rep=m.group(2), cfg=m.group(3),
                       v=defaultdict(list), gate=False)
            continue
        if cur is None: continue
        if GOLD in line: cur['gate'] = True
        for k, rx in pat.items():
            m = rx.match(line)
            if m:
                cur['v'][k].append(tuple(float(g) for g in m.groups()))
                break
if cur: rows.append(cur)

bad = [r for r in rows if r['rep'] != 'warmup' and not r['gate']]
agg = defaultdict(lambda: defaultdict(list))
for r in rows:
    if r['rep'] == 'warmup' or not r['gate']: continue
    def last(k, i=0):
        return r['v'][k][-1][i] if r['v'].get(k) else float('nan')
    for f, k, i in (('it1','it1',0),('it0','it0',0),('dev','dev',0),
                    ('up','p1',1),('lc','p1',2),('walk','p3',1),
                    ('uf2','p3',3),('tb','tb',0),('pA','pa',0),('pB','pa',1)):
        agg[r['arm']][f].append(last(k,i))

def m(a,f):
    xs=[x for x in agg[a][f] if x==x]
    return statistics.mean(xs) if xs else float('nan')
base = m('base','it1')
print(f"{'arm':<12} {'n':>2} {'iter1_ms':>9} {'spread':>7} {'vs base':>8} {'iter0_ms':>9} "
      f"{'ph1dev_s':>9} {'phaseA':>7} {'walk_s':>7} {'upward':>7} {'tbuild':>8}")
def key(a):
    x = m(a,'it1')
    return x if x==x else float('inf')
for a in sorted(agg, key=key):
    xs=[x for x in agg[a]['it1'] if x==x]
    if not xs:            # arm ran but never reached iteration 1 (a hang the
        continue          # 150 s timeout cut short); it is listed under EXCLUDED
    print(f"{a:<12} {len(xs):>2} {m(a,'it1'):>9.1f} {(max(xs)-min(xs)):>7.1f} "
          f"{100*(m(a,'it1')-base)/base:>+7.1f}% {m(a,'it0'):>9.1f} {m(a,'dev'):>9.3f} "
          f"{m(a,'pA'):>7.3f} {m(a,'walk'):>7.3f} {m(a,'up'):>7.3f} {m(a,'tb'):>8.1f}")
if bad:
    print("\nEXCLUDED (no gold component pair -- timed out, aborted or wrong answer):")
    for r in bad:
        print(f"  {r['arm']} rep {r['rep']}  exit={r['v'].get('exit',[('?',)])[-1][0]}")
