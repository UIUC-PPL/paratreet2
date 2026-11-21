#!/usr/bin/env python3
"""Velocity and mass statistics of a Tipsy or NChilada snapshot, from evenly
spaced samples (design/subhalo-checks.md, "cosmo25: the velocity check").

  velstats.py <snapshot> [chunks] [chunk_size]

A directory is read as NChilada (big-endian field files: 28-byte header,
min and max, then the values), a file as standard big-endian Tipsy.
`chunks` runs of `chunk_size` particles (default 4096 of 16384), evenly
spaced in file order, are read from each family.

Prints per family: the count, the scale factor, the total mass (the sampled
mean mass times the count), the mass-weighted mean velocity, the
mass-weighted rms speed, the rms speed about the mean, the rms of one
component and the largest speed sampled; and the same over all families,
weighted by mass. In N-Body Shop code units (G = 1, critical density 1,
box side 1) the total mass of a collisionless box is Omega_m.
"""
import os
import struct
import sys

import numpy as np


def starts(n, chunks, size):
    if n <= chunks * size:
        return [(0, n)]
    s = (np.arange(chunks, dtype=np.int64) * (n - size)) // (chunks - 1)
    return [(int(a), size) for a in s]


def tipsy_families(path):
    with open(path, "rb") as f:
        t, n, ndim, nsph, ndark, nstar = struct.unpack(">diiiii", f.read(28))
    gas = np.dtype([("m", ">f4"), ("x", ">f4", 3), ("v", ">f4", 3), ("rho", ">f4"),
                    ("temp", ">f4"), ("h", ">f4"), ("z", ">f4"), ("phi", ">f4")])
    dark = np.dtype([("m", ">f4"), ("x", ">f4", 3), ("v", ">f4", 3), ("eps", ">f4"),
                     ("phi", ">f4")])
    star = np.dtype([("m", ">f4"), ("x", ">f4", 3), ("v", ">f4", 3), ("z", ">f4"),
                     ("tf", ">f4"), ("eps", ">f4"), ("phi", ">f4")])
    off = 32
    out = []
    for name, cnt, dt in (("gas", nsph, gas), ("dark", ndark, dark), ("star", nstar, star)):
        if cnt > 0:
            mm = np.memmap(path, dtype=dt, mode="r", offset=off, shape=(cnt,))

            def read(a, k, mm=mm):
                r = np.array(mm[a:a + k])
                return r["m"].astype(np.float64), r["v"].astype(np.float64)
            out.append((name, cnt, t, read))
        off += cnt * dt.itemsize
    if os.path.getsize(path) != off:
        sys.exit("velstats: %s is %d bytes, header says %d" % (path, os.path.getsize(path), off))
    return out


def nchilada_field(path):
    with open(path, "rb") as f:
        magic, t, n, ndim, code = struct.unpack(">idQii", f.read(28))
    if code != 9:
        sys.exit("velstats: %s has type code %d, expected 9 (float32)" % (path, code))
    off = 28 + 2 * 4 * ndim
    shape = (n, ndim) if ndim > 1 else (n,)
    if os.path.getsize(path) != off + 4 * n * ndim:
        sys.exit("velstats: %s size does not match its header" % path)
    return t, n, np.memmap(path, dtype=">f4", mode="r", offset=off, shape=shape)


def nchilada_families(path):
    out = []
    for name in ("gas", "dark", "star"):
        d = os.path.join(path, name)
        if not os.path.isdir(d):
            continue
        t, n, mass = nchilada_field(os.path.join(d, "mass"))
        t2, n2, vel = nchilada_field(os.path.join(d, "vel"))
        if (t2, n2) != (t, n):
            sys.exit("velstats: %s mass and vel headers differ" % d)

        def read(a, k, mass=mass, vel=vel):
            return (np.array(mass[a:a + k]).astype(np.float64),
                    np.array(vel[a:a + k]).astype(np.float64))
        out.append((name, n, t, read))
    return out


class Sums:
    def __init__(self):
        self.k = 0
        self.m = 0.0
        self.mv = np.zeros(3)
        self.mv2 = 0.0
        self.v2 = 0.0
        self.vmax = 0.0

    def add(self, m, v):
        s2 = (v * v).sum(axis=1)
        self.k += len(m)
        self.m += m.sum()
        self.mv += (m[:, None] * v).sum(axis=0)
        self.mv2 += (m * s2).sum()
        self.v2 += s2.sum()
        self.vmax = max(self.vmax, float(np.sqrt(s2.max())))

    def merge(self, o, w):
        # o's sums scaled by w (count / sampled), for the all-family line
        self.k += o.k
        self.m += o.m * w
        self.mv += o.mv * w
        self.mv2 += o.mv2 * w
        self.vmax = max(self.vmax, o.vmax)


def report(label, n, a, total_mass, s):
    mean = s.mv / s.m
    rms = np.sqrt(s.mv2 / s.m)
    about = np.sqrt(max(s.mv2 / s.m - (mean * mean).sum(), 0.0))
    print("VELSTATS %s: n %d a %.15g total_mass %.8g mean_v %.4e %.4e %.4e rms_speed %.6f "
          "rms_speed_about_mean %.6f rms_1d %.6f max_speed_sampled %.4f sampled %d"
          % (label, n, a, total_mass, mean[0], mean[1], mean[2], rms, about,
             about / np.sqrt(3.0), s.vmax, s.k))


def main():
    path = sys.argv[1]
    chunks = int(sys.argv[2]) if len(sys.argv) > 2 else 4096
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 16384
    fams = nchilada_families(path) if os.path.isdir(path) else tipsy_families(path)
    print("VELSTATS input %s (%s), %d chunks of %d" %
          (path, "NChilada" if os.path.isdir(path) else "Tipsy", chunks, size))
    allf = Sums()
    ntot = 0
    mtot = 0.0
    for name, n, a, read in fams:
        s = Sums()
        for a0, k in starts(n, chunks, size):
            m, v = read(a0, k)
            s.add(m, v)
        tm = s.m / s.k * n
        report(name, n, a, tm, s)
        allf.merge(s, n / s.k)
        ntot += n
        mtot += tm
    report("all", ntot, fams[0][2], mtot, allf)


if __name__ == "__main__":
    main()
