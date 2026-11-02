#!/usr/bin/env python3
"""Offline view of a PARATREET_WALK_PIECES CSV (one row per TreePiece).

usage: python3 -I walkpieces.py <prefix>.iter<k>.csv [ppn]

Prints: piece busy distribution; how concentrated the busiest PEs are in
their top pieces; whether the hot pieces are spatially clustered (piece
index is space-filling-curve order); and the piece-reassignment what-ifs
(global LPT, within-process LPT, even split within a process).
"""
import csv, heapq, math, sys
from collections import defaultdict

path = sys.argv[1]
ppn = int(sys.argv[2]) if len(sys.argv) > 2 else 7
rows = []
with open(path) as f:
    for r in csv.DictReader(f):
        rows.append((int(r["piece"]), int(r["pe"]), int(r["node"]), int(r["n"]),
                     int(r["calls"]), int(r["fetches"]), float(r["busy_s"]),
                     int(r["leaf_ints"])))
rows.sort(key=lambda r: -r[6])
P = max(r[1] for r in rows) + 1
N = max(r[2] for r in rows) + 1
busy = [r[6] for r in rows]
tot = sum(busy)
pe_busy = defaultdict(float)
node_busy = defaultdict(float)
pe_pieces = defaultdict(list)
for r in rows:
    pe_busy[r[1]] += r[6]
    node_busy[r[2]] += r[6]
    pe_pieces[r[1]].append(r)


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]


print(f"{path}: {len(rows)} pieces, {P} PEs, {N} processes, walk busy total {tot:.3f} s")
print(f"piece busy ms: p50 {1e3*pct(busy,.5):.3f} p90 {1e3*pct(busy,.9):.3f} "
      f"p99 {1e3*pct(busy,.99):.3f} p99.9 {1e3*pct(busy,.999):.3f} max {1e3*busy[0]:.2f}")
cum, k50 = 0.0, 0
for b in busy:
    cum += b
    k50 += 1
    if cum >= tot / 2:
        break
print(f"half of all walk busy is in the top {k50} pieces ({100*k50/len(rows):.2f}%)")

pe_avg = tot / P
hot = sorted(pe_busy.items(), key=lambda kv: -kv[1])[:8]
print(f"PE busy avg {1e3*pe_avg:.2f} ms; hottest PEs (ms, pieces, top-piece share, top piece):")
for pe, b in hot:
    ps = sorted(pe_pieces[pe], key=lambda r: -r[6])
    print(f"  pe {pe:5d} node {pe // ppn:4d}: {1e3*b:7.2f} ms  {len(ps):3d} pieces  "
          f"top1 {100*ps[0][6]/b:4.0f}%  piece {ps[0][0]} ({1e3*ps[0][6]:.2f} ms, "
          f"{ps[0][3]} particles, {ps[0][4]} calls, {ps[0][5]} fetches, {ps[0][7]:.3g} ints)")

# Spatial clustering: hot pieces' index gaps (SFC order => small gaps = one region).
top = sorted(r[0] for r in rows[:64])
gaps = [b - a for a, b in zip(top, top[1:])]
runs = 1 + sum(1 for g in gaps if g > 64)
print(f"top 64 pieces by busy span piece indices {top[0]}..{top[-1]} in {runs} "
      f"clusters (gap > 64 starts a new cluster)")
hot_nodes = sorted({r[2] for r in rows[:64]})
print(f"top 64 pieces live on {len(hot_nodes)} processes: {hot_nodes[:24]}"
      f"{' ...' if len(hot_nodes) > 24 else ''}")


def lpt(pieces, bins):
    h = [0.0] * bins
    heapq.heapify(h)
    mk = 0.0
    for p in sorted(pieces, reverse=True):
        l = heapq.heappop(h) + p
        heapq.heappush(h, l)
        mk = max(mk, l)
    return mk


by_node = defaultdict(list)
for r in rows:
    by_node[r[2]].append(r[6])
now = max(pe_busy.values())
# Static round-robin: each process deals its pieces (index order) over its PEs.
rr = defaultdict(float)
node_rows = defaultdict(list)
for r in rows:
    node_rows[r[2]].append(r)
for nd, v in node_rows.items():
    for i, r in enumerate(sorted(v)):
        rr[(nd, i % ppn)] += r[6]
print(f"what-if (s): now {now:.4f}  floor {max(busy[0], pe_avg):.4f}  "
      f"lpt_global {lpt(busy, P):.4f}  "
      f"lpt_in_process {max(lpt(v, ppn) for v in by_node.values()):.4f}  "
      f"proc_even {max(v / ppn for v in node_busy.values()):.4f}  "
      f"round_robin_in_process {max(rr.values()):.4f}")


def pearson(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    return sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else 0.0


print(f"r(busy, leaf_ints) {pearson([r[7] for r in rows], busy):.3f}  "
      f"r(busy, fetches) {pearson([r[5] for r in rows], busy):.3f}  "
      f"r(busy, particles) {pearson([r[3] for r in rows], busy):.3f}")
