#!/usr/bin/env python3
"""relay18 Part B: band counts with the start-of-trace interval EXCLUDED.

Every PE's trace opens with one OVERHEAD interval that runs from the first
record to the first BEGIN_IDLE.  It is trace startup, not a stall.  In five
of six arms it lands inside the 10-60 ms band and contributes exactly one
interval per PE (896), which swamps the real population.  open_entry == -1
identifies it.
"""
import glob, os, sys, collections
sys.path.insert(0, '/ccs/home/lvkale/software/scripts')
from relay18_state import intervals, pe_of, OVER
import multiprocessing as mp

LO, HI = 10000, 60000

def one(path):
    pe = pe_of(path); n = 0; ms = 0; first = None
    for a, b, st, oe, ce, cs, k in intervals(path):
        if first is None: first = a
        if st != OVER: continue
        if oe == -1: continue                 # start-of-trace interval
        d = b - a
        if LO <= d <= HI: n += 1; ms += d
    return pe, n, ms

def run(d, ppn, poll, label, span):
    files = sorted(glob.glob(os.path.join(d, '*.log.gz')))
    tot = 0; totms = 0
    byrank = collections.Counter(); msrank = collections.Counter()
    with mp.Pool(16) as p:
        for pe, n, ms in p.imap_unordered(one, files, chunksize=8):
            tot += n; totms += ms
            byrank[pe % ppn] += n; msrank[pe % ppn] += ms
    npe = len(files)
    pol = sum(byrank[r] for r in range(ppn) if r % poll == 0)
    cot = tot - pol
    npol = len([r for r in range(ppn) if r % poll == 0])
    print("%-16s band %5d  %8.1f ms  %.4f/PE-s | pollers %d/proc carry %5d (%.1f%%), "
          "co-tenants %5d | per-poller-PE %.1f vs per-co-tenant-PE %.1f"
          % (label, tot, totms/1000.0, tot/(npe*span), npol, pol, 100.0*pol/tot if tot else 0,
             cot, pol/(npol*npe/ppn), (cot/((ppn-npol)*npe/ppn)) if ppn > npol else 0))
    print("                 by rank: " + "  ".join("r%d%s=%d" % (r, '*' if r % poll == 0 else '', byrank[r]) for r in range(ppn)))

if __name__ == '__main__':
    T = '/lustre/orion/csc710/scratch/lvkale/s3ab/5310158/traces'
    for lab, ndev, poll, span in [('M-p7d4-poll2-r1',4,2,23.754), ('M-p7d4-poll2-r2',4,2,11.521),
                                  ('L-p7d7-poll1-r1',7,1,11.738), ('L-p7d7-poll1-r2',7,1,11.672),
                                  ('H-p7d1-poll7-r1',1,7,11.775), ('H-p7d1-poll7-r2',1,7,11.461)]:
        run(os.path.join(T, lab), 7, poll, lab, span)
    print()
    print("HIS RUN 5307458 for reference (Part A, same state machine): 317 band, "
          "10.709 s span, 0.0330/PE-s, 310 of 317 on pollers")
