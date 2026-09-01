#!/usr/bin/env python3
"""Does an inter-process QUIET WINDOW exist at all?  (relay13)

The LCI idle-stall needs ~1 s without traffic to arm (charm_best_practices
"WORKAROUND, measured 2026-08-04"), and ANY background traffic in the job
suppresses it -- including traffic from ranks not involved in the exchange.
The trigger model therefore rests on a premise that can be checked directly
against a trace already on disk: is phaseA actually quiet on the wire?

Projections BEGIN_PROCESSING records the SOURCE PE of the message being
executed:
    2 mtype entry time(us) event pe msglen ... recvTime
so an arrival is inter-process when src//ppn != dst//ppn.  The keep-alive
ring is invisible here (raw Converse handlers are not traced), which is what
we want: this measures the APPLICATION's own ambient traffic, the thing that
decides whether the stall's precondition still exists when the ring is off.

Usage: proj-quiet-scan.py --dir DIR [--procs 5,40] [--ppn 14] [--bucket 0.1]
"""
import argparse, glob, gzip, os, sys, collections

def scan_pe(path, pe, ppn):
    """returns (all_arrival_times_s, interproc_arrival_times_s, t_end)"""
    allt, inter = [], []
    myproc = pe // ppn
    with gzip.open(path, 'rt') as f:
        f.readline()
        for line in f:
            if line[0] != '2' or line[1] != ' ':
                continue
            p = line.split()
            t = int(p[3]) * 1e-6
            src = int(p[5])
            allt.append(t)
            if src // ppn != myproc:
                inter.append(t)
    return allt, inter

def gaps(times, floor=0.0, ceil=None):
    times = sorted(times)
    out = []
    for a, b in zip(times, times[1:]):
        if b - a >= floor and (ceil is None or a >= ceil[0] and b <= ceil[1]):
            out.append((a, b, b - a))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', required=True)
    ap.add_argument('--procs', default='0,5,40',
                    help='comma-separated PROCESS indices to scan (14 PEs each)')
    ap.add_argument('--ppn', type=int, default=14)
    ap.add_argument('--bucket', type=float, default=0.1)
    a = ap.parse_args()

    stub = glob.glob(os.path.join(a.dir, '*.0.log.gz'))
    if not stub:
        print('no trace files in', a.dir); sys.exit(1)
    stub = stub[0][:-len('0.log.gz')]

    for proc in [int(x) for x in a.procs.split(',')]:
        pes = range(proc * a.ppn, (proc + 1) * a.ppn)
        per_pe_inter = {}
        allt, inter = [], []
        for pe in pes:
            path = f'{stub}{pe}.log.gz'
            if not os.path.exists(path):
                print(f'  (missing {path})'); continue
            A, I = scan_pe(path, pe, a.ppn)
            per_pe_inter[pe] = I
            allt += A; inter += I
        if not allt:
            continue
        t0, t1 = min(allt), max(allt)
        print(f'=== process {proc} (PEs {pes.start}-{pes.stop-1}) ===')
        print(f'  trace span {t0:.3f}-{t1:.3f} s')
        print(f'  arrivals: {len(allt):,} total, {len(inter):,} inter-process '
              f'({len(inter)/len(allt):.1%})')
        print(f'  inter-process arrival rate, process-wide: '
              f'{len(inter)/(t1-t0):,.0f}/s')

        # process-wide quiet windows
        g = gaps(inter, floor=0.2)
        print(f'  PROCESS-WIDE inter-process gaps >= 200 ms: {len(g)}')
        for s, e, d in sorted(g, key=lambda x: -x[2])[:8]:
            print(f'      {d*1000:8.1f} ms   {s:.3f} -> {e:.3f} s')
        # per-PE (the stall is thread-local)
        worst = []
        for pe, I in per_pe_inter.items():
            if len(I) < 2:
                worst.append((pe, float('inf'), 0, 0)); continue
            gg = gaps(I, floor=0.0)
            m = max(gg, key=lambda x: x[2])
            worst.append((pe, m[2], m[0], len(I)))
        worst.sort(key=lambda x: -x[1])
        print('  worst PER-PE inter-process gap (thread-local view):')
        for pe, d, s, n in worst[:5]:
            print(f'      pe {pe:5d}  {d*1000:8.1f} ms starting {s:.3f} s   '
                  f'({n} inter-process arrivals)')
        # occupancy histogram
        nb = int((t1 - t0) / a.bucket) + 1
        hist = [0] * nb
        for t in inter:
            hist[int((t - t0) / a.bucket)] += 1
        empty = sum(1 for h in hist if h == 0)
        print(f'  {a.bucket*1000:.0f} ms buckets with ZERO inter-process arrivals: '
              f'{empty} of {nb}')
        runs, cur = [], 0
        for h in hist:
            if h == 0: cur += 1
            elif cur: runs.append(cur); cur = 0
        if cur: runs.append(cur)
        if runs:
            print(f'  longest run of empty buckets: {max(runs)} '
                  f'({max(runs)*a.bucket*1000:.0f} ms)')
        print()

if __name__ == '__main__':
    main()
