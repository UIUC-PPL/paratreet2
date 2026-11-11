#!/usr/bin/env python3
"""A cube cut out of a periodic dark-matter-only Tipsy file, written as a
Tipsy file whose positions run from -side/2 to side/2, to be treated as a
periodic box of that side (FoF3 -P side, tipsy2hbt --period side).

  subcube_tipsy.py in.tipsy out.tipsy cx cy cz side [--period P]

cx cy cz is the cube's centre in the input's coordinates (-P/2 .. P/2), side
its side in the same units. Masses, velocities and softening are copied.

Used to make a group at least half the box wide out of real particles
(design/subhalo-checks.md, "Groups at least half the box wide"): the cube is
a little wider than one large halo. The periodic boundary of the cube is
artificial: the particles are real, the box is not.
"""
import sys
import numpy as np

args = [a for a in sys.argv[1:] if not a.startswith("--")]
period = 1.0
if "--period" in sys.argv:
    period = float(sys.argv[sys.argv.index("--period") + 1])
    args.remove(sys.argv[sys.argv.index("--period") + 1])
src, dst = args[0], args[1]
centre = np.array([float(v) for v in args[2:5]])
side = float(args[5])

hdr_t = np.dtype([("time", ">f8"), ("n", ">i4"), ("ndim", ">i4"), ("nsph", ">i4"),
                  ("ndark", ">i4"), ("nstar", ">i4"), ("pad", ">i4")])
rec = np.dtype([("mass", ">f4"), ("pos", ">f4", 3), ("vel", ">f4", 3), ("eps", ">f4"),
                ("phi", ">f4")])
hdr = np.fromfile(src, dtype=hdr_t, count=1)
assert hdr["nsph"][0] == 0 and hdr["nstar"][0] == 0 and hdr["n"][0] == hdr["ndark"][0]
p = np.memmap(src, dtype=rec, mode="r", offset=32, shape=(int(hdr["n"][0]),))
d = p["pos"].astype(np.float64) - centre
d -= period * np.round(d / period)
keep = np.all((d >= -0.5 * side) & (d < 0.5 * side), axis=1)
q = np.array(p[keep])
off = d[keep].astype(">f4")
# A single-precision offset can round up to side/2; fold it to the low edge.
off[off >= np.float32(0.5 * side)] = np.float32(-0.5 * side)
q["pos"] = off
out = hdr.copy()
out["n"] = out["ndark"] = len(q)
with open(dst, "wb") as f:
    out.tofile(f)
    q.tofile(f)
print(f"{len(q)} of {len(p)} particles in a cube of side {side:g} centred on "
      f"{centre[0]:g} {centre[1]:g} {centre[2]:g}; time {hdr['time'][0]}, "
      f"mass sum {q['mass'].astype(np.float64).sum():.6f}, "
      f"pos range {q['pos'].min():.5f} .. {q['pos'].max():.5f}")
