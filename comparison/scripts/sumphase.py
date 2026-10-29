#!/usr/bin/env python3
"""Attribute a summary trace to FoF3's phase brackets.

The .sum files carry per-PE utilization/idle histograms over the WHOLE run,
so the phase boundaries have to come from the run log's own printed
brackets. Those are cumulative and the run ends when the trace ends, so the
windows are anchored at the END of the trace and walked backwards.

Usage: sumphase.py <tracedir> <runlog>
"""
import sys, os, glob, re
from collections import defaultdict

def parse_rle(s):
    out = []
    for tok in s.split():
        if '+' in tok:
            v, c = tok.split('+', 1); out.extend([int(v)] * int(c))
        else:
            out.append(int(tok))
    return out

def grab(log, pat, idx, scale=1.0):
    m = re.search(pat, log, re.M)
    return float(m.group(idx)) * scale if m else None

d, logf = sys.argv[1], sys.argv[2]
log = open(logf, errors='replace').read()

util, idle, n = [], [], 0
binsize = None
for f in glob.glob(os.path.join(d, '*.sum')):
    if f.endswith('.sum.sts'): continue
    L = open(f, errors='replace').read().split('\n')
    if not L or not L[0].startswith('ver:'): continue
    n += 1
    if binsize is None:
        binsize = float(re.search(r'interval:(\S+)', L[0]).group(1))
    u = parse_rle(L[1]); i = []
    for ln in L:
        if ln.startswith('IdlePercent:'): i = parse_rle(ln.split(':', 1)[1])
    for arr, acc in ((u, util), (i, idle)):
        if len(acc) < len(arr): acc.extend([0] * (len(arr) - len(acc)))
        for k, v in enumerate(arr): acc[k] += v

T = len(util) * binsize
def mean(lo, hi):
    a, b = int(lo / binsize), min(int(hi / binsize), len(util))
    if b <= a: return 0.0, 0.0, 0
    return (sum(util[a:b]) / ((b - a) * n),
            sum(idle[a:b]) / ((b - a) * n) if idle else 0.0, b - a)

tb  = grab(log, r'^Tree build and sending leaves: ([\d.]+) ms', 1, 1e-3)
pre = grab(log, r'^Pre-traversal .*: ([\d.]+) ms', 1, 1e-3)
tv  = grab(log, r'^Tree traversal: ([\d.]+) ms', 1, 1e-3)
it0 = grab(log, r'^Iteration 0 time: ([\d.]+) ms', 1, 1e-3)

def s(pat, i=1):
    v = grab(log, pat, i)
    return v if v is not None else 0.0
sen = s(r'time_s: fragcheck [\d.]+ tip_sentinel ([\d.]+)')
setup = s(r'time_s: uf2_setup ([\d.]+)')
walk  = s(r'uf2_setup [\d.]+ phase3_walk ([\d.]+)')
gath  = s(r'phase3_walk [\d.]+ edge_gather ([\d.]+)')
uf2   = s(r'edge_gather [\d.]+ uf2 ([\d.]+)')
rel   = s(r'uf2 [\d.]+ relabel ([\d.]+)')
hist  = s(r'time_s: component_histogram ([\d.]+)')
resid = tv - (sen + setup + walk + gath + uf2 + rel + hist)

print(f'{os.path.basename(d)}')
print(f'PEs {n}  binsize {binsize}s  trace span {T:.1f}s  it0 {it0:.3f}s')
print(f'{"bracket":<24}{"wall_s":>9}{"%it0":>7}{"busy%":>8}{"idle%":>8}{"PE-s busy":>12}')
rows = [('tree build', tb), ('pre-traversal', pre)]
t = T - tv - pre - tb
for nm, dur in rows:
    u, i, _ = mean(t, t + dur)
    print(f'{nm:<24}{dur:>9.3f}{100*dur/it0:>7.1f}{u:>8.1f}{i:>8.1f}{dur*u/100*n:>12.0f}')
    t += dur
u, i, _ = mean(t, t + tv)
print(f'{"traversal":<24}{tv:>9.3f}{100*tv/it0:>7.1f}{u:>8.1f}{i:>8.1f}{tv*u/100*n:>12.0f}')
print(f'{"  (inside traversal)":<24}')
for nm, dur in [('tip_sentinel', sen), ('uf2_setup', setup), ('phase3_walk', walk),
                ('edge_gather', gath), ('uf2', uf2), ('relabel', rel),
                ('stats reduce+print', resid), ('component_histogram', hist)]:
    u, i, _ = mean(t, t + dur)
    print(f'  {nm:<22}{dur:>9.3f}{100*dur/it0:>7.1f}{u:>8.1f}{i:>8.1f}{dur*u/100*n:>12.0f}')
    t += dur
# longest stretch under 5% utilization inside the traversal bracket
tv0 = T - tv
a = int(tv0 / binsize)
best = cur = 0; beg = bbeg = a
for k in range(a, len(util)):
    if util[k] / n < 5.0:
        if cur == 0: beg = k
        cur += 1
        if cur > best: best, bbeg = cur, beg
    else:
        cur = 0
print(f'\nlongest dead stretch (<5% busy) in the traversal bracket: '
      f'{best*binsize:.2f}s at t={bbeg*binsize:.2f} '
      f'({(bbeg-a)*binsize:.2f}s into the bracket)')
