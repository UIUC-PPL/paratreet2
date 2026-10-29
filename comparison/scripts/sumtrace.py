#!/usr/bin/env python3
"""Aggregate Charm++ `-tracemode summary` traces.

Summary mode writes, per PE, a small text file:
  line 1  ver:.. <pe>/<npes> count:<nbins> ep:<nEPs> interval:<binsize>
  line 2  utilization histogram, %4d values, run-length encoded as "v+count"
  EPExeTime:  <us per entry method>          <- total time in each EP
  EPCallTime: <invocations per entry method>
  MaxEPTime:  <longest single invocation, us>
  ...
  IdlePercent: <same RLE encoding as line 2>

EP index -> name comes from <prog>.sum.sts ("ENTRY CHARE <idx> <name> ...").

Usage: sumtrace.py <tracedir> [--top N] [--timeline]
"""
import sys, os, glob, re
from collections import defaultdict

def parse_rle(tok_line):
    out = []
    for tok in tok_line.split():
        if '+' in tok:
            v, c = tok.split('+', 1)
            out.extend([int(v)] * int(c))
        else:
            out.append(int(tok))
    return out

def load_sts(d):
    """ENTRY CHARE <idx> "<name>" <chareIdx> <msgIdx>  -- the name is quoted
    and contains spaces ("goDown(const CkArrayIndex&, ...)"), so it has to be
    pulled out with a regex, not by splitting on whitespace."""
    names, chares, ep_chare = {}, {}, {}
    ent = re.compile(r'^ENTRY\s+\S+\s+(\d+)\s+"(.*)"\s+(-?\d+)\s+(-?\d+)')
    chr_ = re.compile(r'^CHARE\s+(\d+)\s+"(.*)"')
    for f in glob.glob(os.path.join(d, '*.sts')):
        for line in open(f, errors='replace'):
            m = ent.match(line)
            if m:
                names[int(m.group(1))] = m.group(2)
                ep_chare[int(m.group(1))] = int(m.group(3))
                continue
            m = chr_.match(line)
            if m:
                chares[int(m.group(1))] = m.group(2)
    for i, c in ep_chare.items():
        cn = chares.get(c)
        if cn and cn not in ('null', 'Chare', 'Group', 'SDAG'):
            names[i] = cn + '::' + names[i]
    return names

def main():
    d = sys.argv[1]
    top = 25
    if '--top' in sys.argv:
        top = int(sys.argv[sys.argv.index('--top') + 1])
    want_timeline = '--timeline' in sys.argv

    names = load_sts(d)
    files = sorted(glob.glob(os.path.join(d, '*.sum')))
    files = [f for f in files if not f.endswith('.sum.sts')]
    if not files:
        print('no .sum files in', d); return 1

    ep_time = defaultdict(float)   # seconds, summed over PEs
    ep_calls = defaultdict(int)
    ep_max = defaultdict(float)
    util_sum, idle_sum, nbins_max, binsize = [], [], 0, None
    n_pe = 0
    per_pe_busy = []

    for f in files:
        try:
            lines = open(f, errors='replace').read().split('\n')
        except OSError:
            continue
        if len(lines) < 3 or not lines[0].startswith('ver:'):
            continue
        n_pe += 1
        m = re.search(r'interval:(\S+)', lines[0])
        if m and binsize is None:
            binsize = float(m.group(1))
        util = parse_rle(lines[1])
        idle = []
        for ln in lines:
            if ln.startswith('EPExeTime:'):
                for i, v in enumerate(ln.split()[1:]):
                    ep_time[i] += int(v) * 1e-6
            elif ln.startswith('EPCallTime:'):
                for i, v in enumerate(ln.split()[1:]):
                    ep_calls[i] += int(v)
            elif ln.startswith('MaxEPTime:'):
                for i, v in enumerate(ln.split()[1:]):
                    ep_max[i] = max(ep_max[i], int(v) * 1e-6)
            elif ln.startswith('IdlePercent:'):
                idle = parse_rle(ln.split(':', 1)[1])
        nbins_max = max(nbins_max, len(util))
        per_pe_busy.append(sum(util) * (binsize or 0) / 100.0)
        for arr, acc in ((util, util_sum), (idle, idle_sum)):
            if len(acc) < len(arr):
                acc.extend([0] * (len(arr) - len(acc)))
            for i, v in enumerate(arr):
                acc[i] += v

    total = sum(ep_time.values())
    print(f'PEs {n_pe}  bins {nbins_max}  binsize {binsize}s  '
          f'span {nbins_max*(binsize or 0):.1f}s')
    print(f'total EP time across PEs: {total:.1f} PE-s  '
          f'({total/max(n_pe,1):.3f} s/PE)')
    print()
    print(f'{"entry method":<70}{"PE-s":>10}{"%":>7}{"calls":>13}{"max_ms":>9}')
    for i, t in sorted(ep_time.items(), key=lambda kv: -kv[1])[:top]:
        if t <= 0: break
        nm = names.get(i, 'ep%d' % i)[:70]
        print(f'{nm:<70}{t:>10.2f}{100*t/total:>7.1f}'
              f'{ep_calls[i]:>13d}{ep_max[i]*1e3:>9.1f}')

    if want_timeline and n_pe:
        print()
        print('timeline (mean over PEs; u=busy%, i=idle%), 1 row per second')
        per_s = max(1, int(round(1.0 / (binsize or 1))))
        print(f'{"t(s)":>7} {"busy%":>6} {"idle%":>6}  bar')
        for s in range(0, len(util_sum) // per_s + 1):
            lo, hi = s * per_s, min((s + 1) * per_s, len(util_sum))
            if lo >= hi: break
            u = sum(util_sum[lo:hi]) / ((hi - lo) * n_pe)
            iv = sum(idle_sum[lo:hi]) / ((hi - lo) * n_pe) if idle_sum else 0
            print(f'{s:>7} {u:>6.1f} {iv:>6.1f}  {"#"*int(u/2)}{"."*int(iv/2)}')
    return 0

if __name__ == '__main__':
    sys.exit(main())
