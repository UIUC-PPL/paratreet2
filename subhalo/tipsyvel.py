#!/usr/bin/env python3
"""Copies of a standard (big-endian) Tipsy file with every velocity
multiplied by a factor (design/subhalo-checks.md, "The velocity convention
before z = 0").

  tipsyvel.py <in.tipsy> <out prefix> <s1> [s2 ...]

Writes <out prefix>.<i>.tipsy for each factor s_i, in the order given.
Everything except the velocities is copied unchanged.
"""
import os
import struct
import sys

import numpy as np

GAS = np.dtype([("m", ">f4"), ("x", ">f4", 3), ("v", ">f4", 3), ("rest", ">f4", 5)])
DARK = np.dtype([("m", ">f4"), ("x", ">f4", 3), ("v", ">f4", 3), ("rest", ">f4", 2)])
STAR = np.dtype([("m", ">f4"), ("x", ">f4", 3), ("v", ">f4", 3), ("rest", ">f4", 4)])

src, prefix = sys.argv[1], sys.argv[2]
factors = [float(f) for f in sys.argv[3:]]
with open(src, "rb") as f:
    t, n, ndim, nsph, ndark, nstar = struct.unpack(">diiiii", f.read(28))
body = nsph * GAS.itemsize + ndark * DARK.itemsize + nstar * STAR.itemsize
hdr = 32 if os.path.getsize(src) == 32 + body else 28
if os.path.getsize(src) != hdr + body:
    sys.exit("tipsyvel: %s does not match its header" % src)
head = open(src, "rb").read(hdr)
parts = []
off = hdr
for cnt, dt in ((nsph, GAS), (ndark, DARK), (nstar, STAR)):
    parts.append(np.fromfile(src, dtype=dt, count=cnt, offset=off) if cnt else None)
    off += cnt * dt.itemsize
for i, s in enumerate(factors):
    out = "%s.%d.tipsy" % (prefix, i)
    with open(out, "wb") as f:
        f.write(head)
        for p in parts:
            if p is None:
                continue
            q = p.copy()
            q["v"] = (p["v"].astype(np.float64) * s).astype(">f4")
            q.tofile(f)
    print("tipsyvel: wrote %s (velocities times %.10g)" % (out, s))
