#!/usr/bin/env python3
"""relay91 -- SPLIT requestNodes' cost into PER-MESSAGE OVERHEAD and PER-KEY WORK.

Aggregation removes only the fixed term, so this is what decides whether it is
worth building.

PAIRING WITHOUT CROSS-PE MATCHING.  A requestNodes execution CREATES its
addCache reply inside its own BEGIN..END block, and Projections logs creation
records in execution order on that PE.  So a single sequential pass over each
server log pairs them:
    2 ... requestNodes ... t0     -> open the block
    1 ... addCache ... msglen     -> this reply belongs to the open block
    3 ... requestNodes ... t1     -> close: duration t1-t0, bytes = sum msglen
No (source, event) matching and no second file needed.

Nodes are inferred as bytes / BYTES_PER_NODE (relay88 measured 214 B/node from
8,716.6 MB over 40.7M cached nodes).  The regression is duration = a + b*nodes:
a is the per-message overhead an aggregator could remove, b is the lookup and
pack that it cannot.
"""
import sys, os, glob, gzip, re
from multiprocessing import Pool

BYTES_PER_NODE = 214.0

def ep_ids(d, rx):
    sts=[f for f in glob.glob(os.path.join(d,'*.sts')) if not f.endswith('.sum.sts')]
    sts=sts[0] if sts else glob.glob(os.path.join(d,'*.sts'))[0]
    return {int(m.group(1)) for m in
            (re.match(r'ENTRY\s+\S+\s+(\d+)\s+"([^"]*)"',l) for l in open(sts))
            if m and re.search(rx, m.group(2))}

def scan(a):
    path, req_ids, add_ids = a
    pairs=[]
    open_t=None; acc=0; nrep=0
    try:
        with gzip.open(path,'rt') as f:
            for line in f:
                c=line[0]
                if c not in '123' or line[1]!=' ': continue
                p=line.split()
                if len(p)<5: continue
                try: e=int(p[2]); t=int(p[3])
                except ValueError: continue
                if c=='2':
                    if e in req_ids: open_t=t; acc=0; nrep=0
                    elif open_t is not None: pass   # nested exec: ignore
                elif c=='1':
                    if open_t is not None and e in add_ids and len(p)>=7:
                        try: acc+=int(p[6]); nrep+=1
                        except ValueError: pass
                elif c=='3':
                    if e in req_ids and open_t is not None:
                        pairs.append((t-open_t, acc, nrep)); open_t=None
    except Exception:
        return None
    return pairs

def main(d,label,maxpe=256):
    req=ep_ids(d, r'^requestNodes'); add=ep_ids(d, r'^addCache')
    files=sorted(glob.glob(os.path.join(d,'*.log.gz')))[:maxpe]
    print(f"=== {label}\n    {d}\n    {len(files)} PE logs (of {len(glob.glob(os.path.join(d,'*.log.gz')))})")
    with Pool(16) as p:
        res=[r for r in p.map(scan,[(f,req,add) for f in files],chunksize=4) if r]
    P=[x for r in res for x in r]
    P=[(dur,b,n) for dur,b,n in P if 0<dur<100000]
    if not P: print("    no paired executions"); return
    n=len(P)
    print(f"    paired requestNodes executions: {n:,}")
    withrep=[x for x in P if x[1]>0]
    print(f"    of which created a reply: {len(withrep):,} ({100*len(withrep)/n:.1f}%)"
          f"   -- the rest are PENDING-REQUEST JOINS (no lookup-and-pack)")
    if withrep:
        dj=sorted(x[0] for x in P if x[1]==0)
        dr=sorted(x[0] for x in withrep)
        if dj: print(f"    join-only duration us: median {dj[len(dj)//2]}  mean {sum(dj)/len(dj):.2f}")
        print(f"    replying duration us: median {dr[len(dr)//2]}  mean {sum(dr)/len(dr):.2f}")
    # least squares duration = a + b*nodes
    xs=[x[1]/BYTES_PER_NODE for x in withrep]; ys=[x[0] for x in withrep]
    m=len(xs); sx=sum(xs); sy=sum(ys)
    sxx=sum(v*v for v in xs); sxy=sum(xs[i]*ys[i] for i in range(m))
    den=m*sxx-sx*sx
    if den==0: print("    degenerate"); return
    b=(m*sxy-sx*sy)/den; a=(sy-b*sx)/m
    ybar=sy/m
    ss_tot=sum((v-ybar)**2 for v in ys); ss_res=sum((ys[i]-(a+b*xs[i]))**2 for i in range(m))
    print(f"\n    REGRESSION  duration_us = a + b * nodes_in_reply")
    print(f"      a (per-message overhead) = {a:8.3f} us")
    print(f"      b (per-node work)        = {b:8.3f} us/node")
    print(f"      R^2 = {1-ss_res/ss_tot:.3f}   n = {m:,}   mean nodes/reply = {sx/m:.2f}")
    mean_dur=sy/m
    print(f"      mean duration {mean_dur:.2f} us  ->  fixed {100*a/mean_dur:.1f}%"
          f"   per-node {100*(mean_dur-a)/mean_dur:.1f}%")

if __name__=='__main__':
    main(sys.argv[1],sys.argv[2],int(sys.argv[3]) if len(sys.argv)>3 else 256)
