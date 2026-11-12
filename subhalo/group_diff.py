#!/usr/bin/env python3
"""Per group: bound counts of two .particles.bin results, particles bound in
only one, and energies beyond 10 tau, for the groups where anything differs.
  group_diff.py <reference> <candidate> [tau]"""
import sys
import numpy as np
REC = np.dtype([("id", "<i8"), ("g", "<i8"), ("e", "<f8")])
a = np.fromfile(sys.argv[1], dtype=REC); b = np.fromfile(sys.argv[2], dtype=REC)
tau = float(sys.argv[3]) if len(sys.argv) > 3 else 0.00141
n = int(max(a["id"].max(), b["id"].max())) + 1
ea = np.full(n, np.nan); eb = np.full(n, np.nan); g = np.zeros(n, dtype=np.int64)
ea[a["id"]] = a["e"]; eb[b["id"]] = b["e"]; g[a["id"]] = a["g"]; g[b["id"]] = b["g"]
ina, inb = ~np.isnan(ea), ~np.isnan(eb)
print("group | bound in reference | bound in candidate | only reference | only candidate | energies beyond 10 tau | largest |dE|/S")
for k in np.unique(g[ina | inb]):
    m = g == k
    ra, rb = ina & m, inb & m
    if not ra.any():
        print(f"{k} | 0 | {int(rb.sum())} | 0 | {int(rb.sum())} | - | -"); continue
    S = np.median(np.abs(ea[ra]))
    both = ra & rb
    d = np.abs(ea[both] - eb[both]) / S if both.any() else np.array([0.0])
    oa, ob, far = int((ra & ~rb).sum()), int((rb & ~ra).sum()), int((d > 10 * tau).sum())
    if oa or ob or far:
        print(f"{k} | {int(ra.sum())} | {int(rb.sum())} | {oa} | {ob} | {far} | {d.max():.3g}")
