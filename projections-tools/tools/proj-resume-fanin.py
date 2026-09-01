#!/usr/bin/env python3
"""The SECOND precondition of the LCI idle-stall (relay13).

charm_best_practices (2026-08-03/04) settled what the bug needs:
  (1) about a second of quiet, and
  (2) resumption as a repeated TWO-WAY exchange cycled over MANY distinct
      peers from ONE thread.  Fan-in alone is clean (0 stalls), fan-out
      alone is clean, and 15 threads with one peer each are clean; only one
      thread cycling two-way over many peers stalls.

proj-quiet-scan.py measures (1).  This measures (2): for each PE, find its
longest inter-process silence, then characterise the RESUMPTION -- how many
distinct peer PROCESSES it exchanges with in the window right after, and
whether the exchange is two-way (it sent to that peer as well as received).

Sends are CREATION records, which do not name a destination, so two-way is
approximated at the PROCESS level: a peer process counts as two-way if this
PE both received from it and (within the same window) some PE of this
process received from us -- unknowable from one file.  What IS knowable per
PE, and is what the LCI experiments varied, is the PEER COUNT K.  That is
what this reports.

Usage: proj-resume-fanin.py --dir DIR [--procs 5] [--ppn 14] [--window 0.2]
"""
import argparse, glob, gzip, os, sys

def scan(path, pe, ppn):
    myproc = pe // ppn
    ev = []
    with gzip.open(path, 'rt') as f:
        f.readline()
        for line in f:
            if line[0] != '2' or line[1] != ' ':
                continue
            p = line.split()
            src = int(p[5])
            if src // ppn != myproc:
                ev.append((int(p[3]) * 1e-6, src))
    return ev

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', required=True)
    ap.add_argument('--procs', default='5')
    ap.add_argument('--ppn', type=int, default=14)
    ap.add_argument('--window', type=float, default=0.2)
    a = ap.parse_args()
    stub = glob.glob(os.path.join(a.dir, '*.0.log.gz'))[0][:-len('0.log.gz')]

    for proc in [int(x) for x in a.procs.split(',')]:
        print(f'=== process {proc}: resumption after the longest silence ===')
        print(f'{"pe":>6} {"silence_ms":>11} {"ends_at_s":>10} '
              f'{"peers_PE":>9} {"peers_proc":>11} {"msgs":>7}   (first '
              f'{a.window*1000:.0f} ms after)')
        for pe in range(proc * a.ppn, (proc + 1) * a.ppn):
            path = f'{stub}{pe}.log.gz'
            if not os.path.exists(path): continue
            ev = scan(path, pe, a.ppn)
            if len(ev) < 3: continue
            best = (0, None)
            for (t0, _), (t1, _) in zip(ev, ev[1:]):
                if t1 - t0 > best[0]: best = (t1 - t0, t1)
            gap, tend = best
            win = [s for t, s in ev if tend <= t < tend + a.window]
            print(f'{pe:>6} {gap*1000:>11.1f} {tend:>10.3f} '
                  f'{len(set(win)):>9} {len(set(s//a.ppn for s in win)):>11} '
                  f'{len(win):>7}')
        print()

if __name__ == '__main__':
    main()
