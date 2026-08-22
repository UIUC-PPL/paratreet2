#!/usr/bin/env python3
"""relay75 -- attribute UNION-FIND across the WHOLE run, not from the uf2
bracket.

Kale, 2026-08-22: "union-find overlaps with dual tree walk (with the walk
submitting edges in batches of 16)".  FoFPhase3.h confirms it --
t_uf2 = t3 - t2 brackets only fireUF2Edges + QD + find_components, i.e. the
POST-WALK residue, while the cascade triggered by the streamed batches runs
concurrently with the walk and is charged to phase3_walk.  So the uf2 row
cannot see the drain and this reads the entry methods instead.

.sumd format (authoritative: paratreet2/.claude/skills/sumdetail-analysis):
EP-MAJOR -- EP0's M intervals, then EP1's M, ...; RLE "a+b" = a repeated b
TIMES (not b+1); a value v means the PE spent v microseconds of that 1 ms
interval in that EP.  Sanity check: the RLE must expand to EXACTLY
numIntervals*numEPs.
"""
import sys, os, glob, re
from multiprocessing import Pool

GROUPS = {
    'walk':      r'^goDown|^startDual',
    'cache':     r'^requestNodes|^addCache|^recvTC',
    'unionfind': r'^union_request|^insertDataFindBoss|^add_size|^set_component'
                 r'|^need_boss|^find_components|^compression_wave'
                 r'|^component_count_done|^prune_components|^UnionFindLib',
    'histogram': r'^histogramShard|^depositLabelCounts|^collectTouchedCounts',
    'relabel':   r'^applyUF2Labels|^collectUF2Labels|^applyTipEncoding',
    'phase1':    r'^startPhase1Chain|^relabelChained',
}

def ep_map(d):
    sts = [f for f in glob.glob(os.path.join(d, '*.sts')) if not f.endswith('.sum.sts')]
    sts = sts[0] if sts else glob.glob(os.path.join(d, '*.sts'))[0]
    names = {}
    for line in open(sts):
        m = re.match(r'ENTRY\s+\S+\s+(\d+)\s+"([^"]*)"', line)
        if m: names[int(m.group(1))] = m.group(2)
    groups = {g: set() for g in GROUPS}
    for eid, nm in names.items():
        for g, rx in GROUPS.items():
            if re.search(rx, nm): groups[g].add(eid)
    return names, groups

def rle(toks):
    for t in toks:
        if '+' in t:
            v, r = t.split('+'); yield int(v), int(r)
        else:
            yield int(t), 1

def read(args):
    path, groups = args
    with open(path) as f:
        h = f.readline().split()
        nint = int([x for x in h if x.startswith('numIntervals')][0].split(':')[1])
        neps = int([x for x in h if x.startswith('numEPs')][0].split(':')[1])
        exe = f.readline().split()[1:]
    tot = 0
    for _, r in rle(exe): tot += r
    assert tot == nint*neps, f"{path}: RLE {tot} != {nint}*{neps}"
    ser = {g: [0.0]*nint for g in groups}
    allser = [0.0]*nint
    pos = 0
    for v, r in rle(exe):
        if v:
            s, e = pos, pos+r
            while s < e:
                ep = s//nint
                if ep >= neps: break
                hi = min(e, (ep+1)*nint)
                a, b = s-ep*nint, hi-ep*nint
                for k in range(a, b): allser[k] += v/1000.0
                for g, ids in groups.items():
                    if ep in ids:
                        tgt = ser[g]
                        for k in range(a, b): tgt[k] += v/1000.0
                s = hi
        pos += r
    return ser, allser

def main(d, label, bin_ms=10):
    names, groups = ep_map(d)
    files = sorted(glob.glob(os.path.join(d, '*.sumd')))
    print(f"=== {label}\n    {d}\n    {len(files)} PEs")
    for g in GROUPS:
        print(f"    {g:10s} {len(groups[g]):3d} EPs")
    with Pool(16) as p:
        res = p.map(read, [(f, groups) for f in files], chunksize=8)
    npe = len(res)
    nint = max(len(r[1]) for r in res)
    agg = {g: [0.0]*nint for g in GROUPS}
    act = {g: [0]*nint for g in GROUPS}
    allsum = [0.0]*nint; allact = [0]*nint
    for ser, allser in res:
        for g in GROUPS:
            for i, v in enumerate(ser[g]):
                if v: agg[g][i] += v; act[g][i] += 1
        for i, v in enumerate(allser):
            if v: allsum[i] += v; allact[i] += 1
    print(f"\n    TOTAL CPU by group over the whole run (PE-ms)")
    for g in GROUPS:
        print(f"      {g:10s} {sum(agg[g]):10.1f}")
    print(f"      {'ALL':10s} {sum(allsum):10.1f}")
    # locate the walk window and the union-find span
    def span(x):
        live = [i for i in range(nint) if x[i] > 0]
        return (live[0], live[-1]) if live else (None, None)
    wa, wb = span(agg['walk']); ua, ub = span(agg['unionfind'])
    print(f"\n    walk      active bins {wa}..{wb}")
    print(f"    unionfind active bins {ua}..{ub}")
    if wa is None: return
    lo = min(wa, ua or wa); hi = max(wb, ub or wb)
    print(f"\n    PROFILE over bins {lo}..{hi} ({hi-lo+1} ms), {bin_ms} ms buckets")
    print(f"      {'bin':>7} {'walkPE':>7} {'ufPE':>7} {'allPE':>7} {'util%':>6}  {'walk ms':>8} {'uf ms':>8}")
    for s in range(lo, hi+1, bin_ms):
        e = min(hi+1, s+bin_ms); w = e-s
        print(f"      {s:7d} {sum(act['walk'][s:e])/w:7.1f} {sum(act['unionfind'][s:e])/w:7.1f}"
              f" {sum(allact[s:e])/w:7.1f} {100*sum(allsum[s:e])/(w*npe):6.2f}"
              f"  {sum(agg['walk'][s:e]):8.1f} {sum(agg['unionfind'][s:e]):8.1f}")
    # THE DRAIN: after walk CPU has essentially stopped, what is left?
    wtot = sum(agg['walk'])
    cum = 0.0; wend = wb
    for i in range(wa, wb+1):
        cum += agg['walk'][i]
        if cum >= 0.99*wtot: wend = i; break
    print(f"\n    walk is 99% complete by bin {wend}; union-find continues to {ub}")
    dl = ub - wend
    if dl > 0:
        ufd = sum(agg['unionfind'][wend+1:ub+1])
        alld = sum(allsum[wend+1:ub+1])
        print(f"    THE DRAIN: {dl} ms after the walk is 99% done")
        print(f"      union-find CPU in it {ufd:9.1f} PE-ms  ({100*ufd/max(1e-9,sum(agg['unionfind'])):.1f}% of all union-find)")
        print(f"      ALL CPU in it        {alld:9.1f} PE-ms")
        print(f"      machine utilisation  {100*alld/(dl*npe):.2f}%")
        print(f"      mean active PEs      {sum(allact[wend+1:ub+1])/dl:.1f} of {npe}")
        print(f"      if that work were perfectly spread it would take {alld/npe:.1f} ms, not {dl} ms")
    print()

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 10)
