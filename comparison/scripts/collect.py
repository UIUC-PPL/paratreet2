#!/usr/bin/env python3
"""Collect every FoF-comparison point from the job logs into one table.

Parses the three VERDICT lines the run scripts emit:
  VERDICT      (FoF3)    ... it0_ms=  components: N max_size M b_exact=B
  VERDICT_AX   (ArborX)  ... components: N max_size M imbalance I
  VERDICT_SW   (SWIFT)   ... fof_ms=   groups=N max_size=M min_group_size=K

The correctness columns are printed ALONGSIDE the timings on purpose. A FoF
answer is a property of the data, so `components` and `max_size` must be
identical at every node count and across all three codes; a timing whose
answer does not match the gate is not a data point, and the table should not
let a reader miss that.

SWIFT's `groups` is NOT directly comparable to FoF3's `components` unless
min_group_size is 1: it counts only groups of at least min_group_size
particles. For the production runs (min_group_size 32) the comparable
quantity is derived from FoF3's log2 histogram -- buckets 5 and up -- which
this script computes from the HIST lines.
"""
import glob, os, re, sys
from collections import defaultdict

LOGDIR = "/lustre/orion/csc710/scratch/rrao/compare/logs"

def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None

def kv(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))

rows = []
hist = {}

# FoF3's full component histogram lives in the streamed .log, and the HIST
# line in early jobs captured the wrong histogram (FoF3 prints an unrelated
# log2_histogram on its "redundancy_concentration" line, and that one comes
# first). Read the histogram from the components line itself, which is
# unambiguous and works retroactively for jobs already run.
for f in sorted(glob.glob(os.path.join(LOGDIR, "fof3_*.log"))):
    try:
        txt = open(f, errors="replace").read()
    except OSError:
        continue
    m = re.search(r"fof3_(\w+?)_(\d+)n\.", os.path.basename(f))
    c = re.search(r"FOF3STAT components: \d+ max_size \d+ (log2_histogram:[^\n]*)", txt)
    if m and c:
        hist[(m.group(1), int(m.group(2)))] = dict(
            (int(a), int(b)) for a, b in re.findall(r"(\d+):(\d+)", c.group(1)))

for f in sorted(glob.glob(os.path.join(LOGDIR, "*.out"))):
    try:
        txt = open(f, errors="replace").read()
    except OSError:
        continue
    jid = re.search(r"\.(\d+)\.out$", f)
    jid = jid.group(1) if jid else "?"

    for line in txt.splitlines():
        if line.startswith("VERDICT "):
            d = kv(line)
            m = re.search(r"components: (\d+) max_size (\d+)", line)
            rows.append(dict(
                code="fof3", jid=jid, dset=d.get("dset"),
                nodes=int(d["nodes"]) if "nodes" in d else None,
                shape=f"{d.get('tasks')}r", status=d.get("status"),
                load=num(d.get("load_ms")), decomp=num(d.get("decomp_ms")),
                solve=num(d.get("it0_ms")),
                comp=int(m.group(1)) if m else None,
                maxsz=int(m.group(2)) if m else None,
                extra=f"b={d.get('b_exact','NA')}"))

        elif line.startswith("VERDICT_AX "):
            d = kv(line)
            m = re.search(r"components: (\d+) max_size (\d+)", line)
            rows.append(dict(
                code="arborx", jid=jid, dset=d.get("dset"),
                nodes=int(d["nodes"]) if "nodes" in d else None,
                shape=f"{d.get('ranks')}r", status=d.get("status"),
                load=None, decomp=None, solve=None,
                comp=int(m.group(1)) if m else None,
                maxsz=int(m.group(2)) if m else None,
                extra=""))

        elif line.startswith("DETAIL_AX "):
            d = kv(line)
            if rows and rows[-1]["code"] == "arborx":
                rows[-1]["load"] = num(d.get("read_s"))
                rows[-1]["decomp"] = num(d.get("redist_s"))
                rows[-1]["solve"] = num(d.get("dbscan_s"))
                # seconds -> ms so the column means one thing
                for k in ("load", "decomp", "solve"):
                    if rows[-1][k] is not None:
                        rows[-1][k] *= 1000.0

        elif line.startswith("VERDICT_SW "):
            d = kv(line)
            rows.append(dict(
                code="swift", jid=jid, dset=d.get("dset"),
                nodes=int(d["nodes"]) if "nodes" in d else None,
                shape=f"{d.get('rpn')}x{d.get('threads')}",
                status=d.get("status"),
                load=num(d.get("read_ms")), decomp=num(d.get("spaceinit_ms")),
                solve=num(d.get("fof_ms")),
                comp=int(d["groups"]) if d.get("groups", "NA").isdigit() else None,
                maxsz=int(d["max_size"]) if d.get("max_size", "NA").isdigit() else None,
                extra=f"mgs={d.get('min_group_size')}"))

if not rows:
    print("no points found in", LOGDIR)
    sys.exit(0)

order = {"cosmo2b": 0, "cosmo25": 1, "romulus25": 2, "smoke": 3}
rows.sort(key=lambda r: (order.get(r["dset"], 9), r["nodes"] or 0,
                         r["code"], r["jid"]))

hdr = (f"{'code':8} {'dset':10} {'nodes':>5} {'shape':>8} {'status':>7} "
       f"{'load_s':>9} {'decomp_s':>9} {'solve_s':>9} "
       f"{'components':>13} {'max_size':>13}  notes")
print(hdr)
print("-" * len(hdr))
for r in rows:
    def s(x):
        return "        -" if x is None else f"{x/1000.0:9.3f}"
    print(f"{r['code']:8} {r['dset'] or '?':10} {r['nodes'] or 0:5d} "
          f"{r['shape']:>8} {r['status'] or '?':>7} "
          f"{s(r['load'])} {s(r['decomp'])} {s(r['solve'])} "
          f"{r['comp'] if r['comp'] is not None else '-':>13} "
          f"{r['maxsz'] if r['maxsz'] is not None else '-':>13}  {r['extra']}")

# ---- correctness gate -----------------------------------------------------
print()
print("=== correctness gate: one answer per dataset, at every scale and code ===")
by = defaultdict(set)
for r in rows:
    if r["comp"] is not None and r["status"] == "OK":
        by[(r["dset"], r["code"], r["extra"])].add((r["comp"], r["maxsz"]))
for k in sorted(by, key=str):
    vals = by[k]
    tag = "SCALE-INVARIANT" if len(vals) == 1 else "*** DRIFTS WITH SCALE ***"
    print(f"  {k[0]:10} {k[1]:8} {k[2]:32} {tag}: {sorted(vals)}")

if hist:
    print()
    print("=== FoF3 groups of size >= 32 (log2 buckets 5+), for comparison")
    print("    with SWIFT's `No. of groups` at min_group_size 32 ===")
    for (dset, nodes), b in sorted(hist.items()):
        ge32 = sum(v for k, v in b.items() if k >= 5)
        tot = sum(b.values())
        print(f"  {dset:10} {nodes:4d} nodes: total {tot:>13,d}   "
              f"size>=32 {ge32:>12,d}   singletons {b.get(0,0):>13,d}")
