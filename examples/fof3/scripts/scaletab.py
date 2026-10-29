#!/usr/bin/env python3
"""Per-node-count scaling table from the scale2b logs. Iteration 1 is the
reported number (steady state); iteration 0 additionally carries one-time
Kokkos/HIP init on the device arms, exactly as the 2026-08-14 sweep."""
import re, sys, glob, statistics
from collections import defaultdict
GOLD = "424897832 max_size 185317566"
arm_re = re.compile(r'^=== ARM (\S+) rep (\S+) nodes (\d+)')
pat = {
 'it0': re.compile(r'^Iteration 0 time: ([\d.]+) ms'),
 'it1': re.compile(r'^Iteration 1 time: ([\d.]+) ms'),
 'tb':  re.compile(r'^Tree build and sending leaves: ([\d.]+) ms'),
 'trav':re.compile(r'^Tree traversal: ([\d.]+) ms'),
 'dev': re.compile(r'^FOF3STAT time_s: phase1_device wall ([\d.]+)'),
 'p1':  re.compile(r'^FOF3STAT time_s: phase1 ([\d.]+) .* upwardPass ([\d.]+) loadCache ([\d.]+)'),
 'p3':  re.compile(r'^FOF3STAT time_s: uf2_setup ([\d.]+) phase3_walk ([\d.]+) edge_gather ([\d.]+) uf2 ([\d.]+) relabel ([\d.]+)'),
}
rows, cur, ok = [], None, True
for p in sorted(sys.argv[1:]):
    for line in open(p, errors='replace'):
        m = arm_re.match(line)
        if m:
            if cur: rows.append(cur)
            cur = dict(arm=m.group(1), rep=m.group(2), n=int(m.group(3)),
                       vals=defaultdict(list), gate=False)
            continue
        if cur is None: continue
        if GOLD in line: cur['gate'] = True
        for k, rx in pat.items():
            m = rx.match(line)
            if m:
                cur['vals'][k].append(tuple(float(g) for g in m.groups()))
                break
if cur: rows.append(cur)

# steady state = the LAST occurrence of each per-iteration stat (iteration 1)
agg = defaultdict(lambda: defaultdict(list))
for r in rows:
    if r['rep'] == 'warmup' or not r['gate']: continue
    v = r['vals']
    def last(k, i=0):
        return v[k][-1][i] if v.get(k) else float('nan')
    agg[(r['n'], r['arm'])]['it1'].append(last('it1'))
    agg[(r['n'], r['arm'])]['it0'].append(last('it0'))
    agg[(r['n'], r['arm'])]['dev'].append(last('dev'))
    agg[(r['n'], r['arm'])]['up'].append(last('p1', 1))
    agg[(r['n'], r['arm'])]['lc'].append(last('p1', 2))
    agg[(r['n'], r['arm'])]['walk'].append(last('p3', 1))
    agg[(r['n'], r['arm'])]['uf2'].append(last('p3', 3))
    agg[(r['n'], r['arm'])]['tb'].append(last('tb'))

def mean(k, f): 
    xs=[x for x in agg[k][f] if x==x]
    return statistics.mean(xs) if xs else float('nan')

print("ITERATION 1 (steady state), mean of 3 reps, ms / s")
print(f"{'nodes':>5} {'arm':<11} {'iter1_ms':>9} {'spread':>7} {'iter0_ms':>9} "
      f"{'ph1_dev_s':>10} {'upward':>7} {'ldCache':>8} {'walk':>7} {'uf2':>7} {'tbuild_ms':>10}")
for n in sorted({k[0] for k in agg}):
    for a in ('device','device_l12','cpu'):
        k=(n,a)
        if k not in agg: continue
        xs=[x for x in agg[k]['it1'] if x==x]
        print(f"{n:>5} {a:<11} {mean(k,'it1'):>9.1f} {(max(xs)-min(xs)):>7.1f} "
              f"{mean(k,'it0'):>9.1f} {mean(k,'dev'):>10.3f} {mean(k,'up'):>7.3f} "
              f"{mean(k,'lc'):>8.3f} {mean(k,'walk'):>7.3f} {mean(k,'uf2'):>7.3f} "
              f"{mean(k,'tb'):>10.1f}")
    print()
print("SPEEDUP and SCALING (iteration 1)")
print(f"{'nodes':>5} {'device_s':>9} {'cpu_s':>8} {'gpu/cpu':>8} {'dev speedup vs 8n':>18} {'ideal':>6}")
d8 = mean((8,'device'),'it1')
for n in sorted({k[0] for k in agg}):
    d, c = mean((n,'device'),'it1'), mean((n,'cpu'),'it1')
    print(f"{n:>5} {d/1000:>9.3f} {c/1000:>8.3f} {c/d:>7.2f}x {d8/d:>17.2f}x {n/8:>5.0f}x")
