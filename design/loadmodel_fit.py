#!/usr/bin/env python3
"""Regression on the FOF3STAT load_model: lines (design/piece-load-model.md).

Reproduces, on Frontier data, the table Anvil job 19932506 produced:
Pearson r of several compressions of the `pair` term against actual
per-process phaseB, and of `self` against actual phaseA, over the 128
processes. Usage: loadmodel_fit.py <file-with-the-128-lines>
"""
import sys, math, re

def pearson(xs, ys):
    n = len(xs)
    if n < 2: return float('nan')
    mx, my = sum(xs)/n, sum(ys)/n
    sxy = sum((x-mx)*(y-my) for x, y in zip(xs, ys))
    sxx = sum((x-mx)**2 for x in xs)
    syy = sum((y-my)**2 for y in ys)
    if sxx <= 0 or syy <= 0: return float('nan')
    return sxy / math.sqrt(sxx*syy)

def spread(vs):
    pos = [v for v in vs if v > 0]
    if not pos: return float('nan')
    return max(pos)/min(pos)

rows = []
for line in open(sys.argv[1]):
    if 'load_model:' not in line: continue
    f = line.split()
    d = {}
    for i, tok in enumerate(f):
        if tok in ('node','pieces','n','self','pair','pa_sum_s','pa_max_s',
                   'pb_sum_s','pb_max_s'):
            d[tok] = float(f[i+1])
    if len(d) == 9: rows.append(d)

print(f"processes parsed: {len(rows)}")
if not rows: sys.exit("no load_model lines found")

pair = [r['pair'] for r in rows]
self_ = [r['self'] for r in rows]
pa    = [r['pa_sum_s'] for r in rows]
pb    = [r['pb_sum_s'] for r in rows]
npart = [r['n'] for r in rows]
pieces= [r['pieces'] for r in rows]

print()
print("--- observed spread over processes (max/min, positive values only) ---")
for name, v in (("pieces",pieces),("n (particles)",npart),("self",self_),
                ("pair",pair),("actual phaseA (pa_sum_s)",pa),
                ("actual phaseB (pb_sum_s)",pb)):
    print(f"  {name:<26} min {min(v):>12.6g}  max {max(v):>12.6g}  spread {spread(v):>10.4g}x")

print()
print("--- phaseB predictors vs actual pb_sum_s over the %d processes ---" % len(rows))
print(f"  {'form':<22} {'Pearson r':>10} {'spread':>12}")
forms = [("pair (as implemented)", pair),
         ("sqrt(pair)",            [math.sqrt(p) for p in pair]),
         ("pair^0.25",             [p**0.25 for p in pair]),
         ("log(pair)",             [math.log(p) if p > 0 else 0.0 for p in pair]),
         ("n^2 (no volume)",       [x*x for x in npart]),
         ("pieces",                pieces)]
for name, v in forms:
    print(f"  {name:<22} {pearson(v, pb):>+10.3f} {spread(v):>12.4g}x")
print(f"  {'(actual pb_sum_s)':<22} {'--':>10} {spread(pb):>12.4g}x")

print()
print("--- phaseA predictors vs actual pa_sum_s ---")
print(f"  {'form':<22} {'Pearson r':>10} {'spread':>12}")
for name, v in [("self (sum n^1.2)", self_),
                ("n (particles)",    npart),
                ("pieces",           pieces)]:
    print(f"  {name:<22} {pearson(v, pa):>+10.3f} {spread(v):>12.4g}x")
print(f"  {'(actual pa_sum_s)':<22} {'--':>10} {spread(pa):>12.4g}x")

# c/a: the ratio that calibrates PARATREET_LB_K, from least-squares through
# the origin, using sqrt(pair) as the phaseB term (the Anvil winner).
sq = [math.sqrt(p) for p in pair]
a = sum(s*p for s, p in zip(self_, pa)) / sum(s*s for s in self_)
c = sum(s*p for s, p in zip(sq, pb))    / sum(s*s for s in sq)
print()
print(f"--- calibration (least squares through the origin) ---")
print(f"  phaseA ~ a * self       a = {a:.6g}")
print(f"  phaseB ~ c * sqrt(pair) c = {c:.6g}")
print(f"  K = c/a = {c/a:.6g}")

# the straggler
worst = max(rows, key=lambda r: r['pb_sum_s'])
med   = sorted(pb)[len(pb)//2]
print()
print(f"--- the straggler ---")
print(f"  worst process: node {int(worst['node'])}  pieces {int(worst['pieces'])} "
      f"n {worst['n']:.0f}  pb_sum_s {worst['pb_sum_s']:.3f}  pb_max_s {worst['pb_max_s']:.3f}")
print(f"  median pb_sum_s {med:.3f}  ->  straggler is {worst['pb_sum_s']/med:.1f}x the median process")
rank = sorted(rows, key=lambda r: -math.sqrt(r['pair']))
pos = next(i for i, r in enumerate(rank) if r['node'] == worst['node'])
print(f"  sqrt(pair) ranks that process #{pos+1} of {len(rows)} "
      f"(a targeted-shedding v2 has to find it in the top few)")
