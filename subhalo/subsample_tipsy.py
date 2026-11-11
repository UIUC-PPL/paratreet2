#!/usr/bin/env python3
"""Every tenth particle of a dark-matter-only Tipsy file, masses times ten
(the recipe of inputs/8m-lambb.tipsy in design/subhalo-checks.md).
usage: subsample_tipsy.py in.tipsy out.tipsy"""
import sys
import numpy as np

src, dst = sys.argv[1], sys.argv[2]
hdr = np.fromfile(src, dtype=np.dtype([("time", ">f8"), ("n", ">i4"), ("ndim", ">i4"),
                                       ("nsph", ">i4"), ("ndark", ">i4"), ("nstar", ">i4"),
                                       ("pad", ">i4")]), count=1)
assert hdr["nsph"][0] == 0 and hdr["nstar"][0] == 0 and hdr["n"][0] == hdr["ndark"][0]
rec = np.dtype([("mass", ">f4"), ("pos", ">f4", 3), ("vel", ">f4", 3), ("eps", ">f4"),
                ("phi", ">f4")])
p = np.memmap(src, dtype=rec, mode="r", offset=32, shape=(int(hdr["n"][0]),))
q = np.array(p[::10])
q["mass"] = (q["mass"].astype(np.float32) * np.float32(10)).astype(">f4")
out = hdr.copy()
out["n"] = out["ndark"] = len(q)
with open(dst, "wb") as f:
    out.tofile(f)
    q.tofile(f)
print(f"{len(q)} particles, time {hdr['time'][0]}, eps {float(q['eps'][0]):.6g}, "
      f"mass sum {q['mass'].astype(np.float64).sum():.6f}, "
      f"pos range {q['pos'].min():.4f} .. {q['pos'].max():.4f}")
