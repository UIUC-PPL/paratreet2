#!/usr/bin/env python3
"""A cube cut out of a Tipsy or NChilada snapshot, written as a standard
big-endian Tipsy file (design/subhalo-checks.md, "cosmo25: HBT-HERONS on a
sub-box").

  subbox.py <snapshot> <out.tipsy> <side> [--centre cx cy cz] [--workers W]
            [--families gas,dark]

Keeps every particle with -side/2 <= x - c < side/2 in each coordinate
(no periodic wrap: the cube must lie inside the box). With the default
centre 0 the positions are written unchanged, bit for bit, so the sub-box is
in the input's own coordinates; with another centre they are written as
offsets from it in single precision.

Families are kept, in file order: gas, then dark, then star. A gas record
gets mass, position and velocity from the input, density 0, temperature
500 (the inputs' gas is collisionless at 500 K), smoothing length and
softening the family's softening, metals 0, potential 0. A dark record gets
mass, position, velocity, softening, potential 0. Stars are not written:
an input with stars needs --families without star (h34462: --families dark).
A directory is read as NChilada (field files gas/ and dark/: pos, vel, mass,
soft), a file as Tipsy (a Tipsy gas record has no softening; the dark
family's first value is used).

Reads in parallel (W processes, default 48), each a run of 2^24 particles.
"""
import os
import struct
import sys
from multiprocessing import Pool

import numpy as np

CHUNK = 1 << 24
GAS = np.dtype([("m", ">f4"), ("x", ">f4", 3), ("v", ">f4", 3), ("rho", ">f4"),
                ("temp", ">f4"), ("h", ">f4"), ("z", ">f4"), ("phi", ">f4")])
DARK = np.dtype([("m", ">f4"), ("x", ">f4", 3), ("v", ">f4", 3), ("eps", ">f4"),
                 ("phi", ">f4")])
STAR = np.dtype([("m", ">f4"), ("x", ">f4", 3), ("v", ">f4", 3), ("z", ">f4"),
                 ("tf", ">f4"), ("eps", ">f4"), ("phi", ">f4")])


def nchilada_field(path):
    with open(path, "rb") as f:
        magic, t, n, ndim, code = struct.unpack(">idQii", f.read(28))
        if code != 9:
            sys.exit("subbox: %s has type code %d, expected 9 (float32)" % (path, code))
        lo = struct.unpack(">" + "f" * ndim, f.read(4 * ndim))
        hi = struct.unpack(">" + "f" * ndim, f.read(4 * ndim))
    off = 28 + 8 * ndim
    if os.path.getsize(path) != off + 4 * n * ndim:
        sys.exit("subbox: %s size does not match its header" % path)
    return t, n, ndim, off, lo, hi


# Work items: (kind, args) with kind "nc" (NChilada family directory) or
# "tipsy" (a Tipsy family's byte offset and dtype). Each returns the kept
# particles of one run as (m, x, v, eps) arrays.
def work(item):
    kind, a0, k, half, centre, info = item
    if kind == "nc":
        d, n, poff, voff, moff, soft_const, soff = info
        x = np.fromfile(os.path.join(d, "pos"), dtype=">f4", count=3 * k,
                        offset=poff + 12 * a0).reshape(k, 3)
        dd = x.astype(np.float64) - centre
        keep = np.nonzero(np.all((dd >= -half) & (dd < half), axis=1))[0]
        if len(keep) == 0:
            return None
        vm = np.memmap(os.path.join(d, "vel"), dtype=">f4", mode="r", offset=voff,
                       shape=(n, 3))
        mm = np.memmap(os.path.join(d, "mass"), dtype=">f4", mode="r", offset=moff,
                       shape=(n,))
        v = np.array(vm[a0 + keep])
        m = np.array(mm[a0 + keep])
        if soft_const is not None:
            eps = np.full(len(keep), soft_const, dtype=">f4")
        else:
            sm = np.memmap(os.path.join(d, "soft"), dtype=">f4", mode="r", offset=soff,
                           shape=(n,))
            eps = np.array(sm[a0 + keep])
        return m, x[keep], dd[keep], v, eps
    path, off, dtname = info
    dt = {"gas": GAS, "dark": DARK}[dtname]
    r = np.fromfile(path, dtype=dt, count=k, offset=off + dt.itemsize * a0)
    dd = r["x"].astype(np.float64) - centre
    keep = np.nonzero(np.all((dd >= -half) & (dd < half), axis=1))[0]
    if len(keep) == 0:
        return None
    r = r[keep]
    eps = r["eps"] if dtname == "dark" else np.zeros(len(keep), dtype=">f4")
    return r["m"], r["x"], dd[keep], r["v"], eps


def main():
    args = [a for a in sys.argv[1:]]
    centre = np.zeros(3)
    workers = 48
    if "--centre" in args:
        i = args.index("--centre")
        centre = np.array([float(v) for v in args[i + 1:i + 4]])
        del args[i:i + 4]
    if "--workers" in args:
        i = args.index("--workers")
        workers = int(args[i + 1])
        del args[i:i + 2]
    wanted = ("gas", "dark", "star")
    if "--families" in args:
        i = args.index("--families")
        wanted = tuple(args[i + 1].split(","))
        del args[i:i + 2]
    src, dst, side = args[0], args[1], float(args[2])
    half = 0.5 * side
    exact = not np.any(centre)

    fams = []   # (name, n, items)
    time = None
    if os.path.isdir(src):
        for name in ("gas", "dark", "star"):
            d = os.path.join(src, name)
            if not os.path.isdir(d) or name not in wanted:
                continue
            if name == "star":
                sys.exit("subbox: star family not supported (use --families)")
            t, n, _, poff, _, _ = nchilada_field(os.path.join(d, "pos"))
            _, n2, _, voff, _, _ = nchilada_field(os.path.join(d, "vel"))
            _, n3, _, moff, _, _ = nchilada_field(os.path.join(d, "mass"))
            _, n4, _, soff, slo, shi = nchilada_field(os.path.join(d, "soft"))
            if not (n == n2 == n3 == n4):
                sys.exit("subbox: field lengths differ in %s" % d)
            time = t
            soft_const = slo[0] if slo[0] == shi[0] else None
            info = (d, n, poff, voff, moff, soft_const, soff)
            items = [("nc", a, min(CHUNK, n - a), half, centre, info) for a in range(0, n, CHUNK)]
            fams.append((name, n, items))
    else:
        with open(src, "rb") as f:
            t, n, ndim, nsph, ndark, nstar = struct.unpack(">diiiii", f.read(28))
        if nstar and "star" in wanted:
            sys.exit("subbox: star family not supported (use --families)")
        off = 32 if os.path.getsize(src) == 32 + nsph * GAS.itemsize + ndark * DARK.itemsize else 28
        time = t
        for name, cnt, dt in (("gas", nsph, GAS), ("dark", ndark, DARK)):
            if cnt and name in wanted:
                items = [("tipsy", a, min(CHUNK, cnt - a), half, centre, (src, off, name))
                         for a in range(0, cnt, CHUNK)]
                fams.append((name, cnt, items))
            off += cnt * dt.itemsize

    kept = {}
    with Pool(workers) as pool:
        for name, n, items in fams:
            parts = [r for r in pool.imap(work, items, chunksize=1) if r is not None]
            if parts:
                kept[name] = tuple(np.concatenate([p[j] for p in parts]) for j in range(5))
            else:
                kept[name] = None
            k = 0 if kept[name] is None else len(kept[name][0])
            mr = ""
            if k:
                m = kept[name][0].astype(np.float64)
                e = kept[name][4].astype(np.float64)
                mr = "; mass %.8g to %.8g, softening %.8g to %.8g" % (m.min(), m.max(), e.min(),
                                                                     e.max())
            print("subbox: %s: %d of %d particles kept%s" % (name, k, n, mr), flush=True)

    dark_eps = None
    if kept.get("dark") is not None:
        dark_eps = kept["dark"][4][0]
    ng = 0 if kept.get("gas") is None else len(kept["gas"][0])
    nd = 0 if kept.get("dark") is None else len(kept["dark"][0])
    with open(dst, "wb") as f:
        f.write(struct.pack(">diiiiii", time, ng + nd, 3, ng, nd, 0, 0))
        for name, dt in (("gas", GAS), ("dark", DARK)):
            if kept.get(name) is None:
                continue
            m, x, dd, v, eps = kept[name]
            r = np.zeros(len(m), dtype=dt)
            r["m"] = m
            r["x"] = x if exact else dd.astype(">f4")
            r["v"] = v
            if name == "gas":
                gas_eps = eps if os.path.isdir(src) else np.full(len(m), dark_eps, dtype=">f4")
                r["temp"] = 500.0
                r["h"] = gas_eps
            else:
                r["eps"] = eps
            r.tofile(f)
    tm = sum(float(kept[k][0].astype(np.float64).sum()) for k in kept if kept[k] is not None)
    print("subbox: wrote %s: %d particles (gas %d, dark %d), a = %.15g, side %g, centre %s, "
          "positions %s, total mass %.8g" % (dst, ng + nd, ng, nd, time, side,
                                             " ".join("%g" % c for c in centre),
                                             "unchanged" if exact else "offsets from the centre", tm))


if __name__ == "__main__":
    main()
