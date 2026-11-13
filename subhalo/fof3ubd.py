#!/usr/bin/env python3
"""FoF3 -U's per-process unbinding output (<prefix>.<process>.ubd) next to
the standalone tools (design/subhalo-checks.md, "Step 2: unbinding inside
FoF3").

  fof3ubd.py bin <snapshot.hdf5> <prefix> <out.particles.bin> [--period P]
  fof3ubd.py compare <snapshot.hdf5> <prefix> <unbind dump.hdf5> [--period P]

The snapshot is the one tipsy2hbt wrote from the same Tipsy file and
FoF3's label files: ParticleIDs = order + 1, FOFGroupIDs its group ids.
FoF3 works in N-Body Shop code units (G = 1, box side P, critical density
1), so an energy in (km/s)^2 is FoF3's times u^2, u = (BoxSize / P) * 100 h
/ 2.894405 with BoxSize (Mpc) and h from the snapshot: tipsy2hbt's velocity
unit without the expansion factor, which FoF3 has already applied.

bin      writes unbindcheck's .particles.bin (one 24-byte record per bound
         particle: id, group, energy in (km/s)^2), in the snapshot's
         numbering, so unbindcheck and unbindcmp read FoF3's result.
compare  per particle against an `unbind --dump` of the snapshot: bound
         flags, |dE|/S (S the median |E| of the dump's bound particles of
         the group), the most bound particle and, if both have them, the
         centre refinement's energies.

A .ubd record is 40 bytes, native byte order: int64 order, int64 label,
float64 energy, float64 refined energy (NaN = not refined), uint8 bound,
uint8 most bound, uint8 refined group, 5 bytes padding
(fof/FoFCatalog.h, FoFUnbindOut). Needs numpy and HDF5's h5dump.
"""
import glob
import os
import subprocess
import sys

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capdistance as cd  # noqa: E402

H5DUMP = os.environ.get("H5DUMP", "/opt/cray/pe/hdf5/1.14.3.9/gnu/12.3/bin/h5dump")
UBD = np.dtype([("order", "<i8"), ("label", "<i8"), ("e", "<f8"), ("r", "<f8"),
                ("bound", "u1"), ("most", "u1"), ("refined", "u1"), ("pad", "V5")])
REC = np.dtype([("id", "<i8"), ("g", "<i8"), ("e", "<f8")])


def read_ubd(prefix):
    files = sorted(glob.glob(prefix + ".*.ubd"))
    if not files:
        sys.exit(f"fof3ubd: no files {prefix}.*.ubd")
    u = np.concatenate([np.fromfile(f, dtype=UBD) for f in files])
    if len(np.unique(u["order"])) != len(u):
        sys.exit("fof3ubd: a particle appears in two .ubd records")
    return u, len(files)


def snapshot(snap, period):
    ids = cd.raw(H5DUMP, snap, "/PartType1/ParticleIDs", "<i8")
    if not np.array_equal(ids, np.arange(1, len(ids) + 1)):
        sys.exit("fof3ubd: the snapshot's ParticleIDs are not order + 1 in file order")
    g = cd.raw(H5DUMP, snap, "/PartType1/FOFGroupIDs", "<i8")
    box = float(cd.attribute(H5DUMP, snap, "/Header/BoxSize"))
    h = float(cd.attribute(H5DUMP, snap, "/Cosmology/h"))
    u = box / period * 100.0 * h / 2.894405
    return g, u * u


def mapped(snap, prefix, period):
    u, nfiles = read_ubd(prefix)
    g_snap, e2 = snapshot(snap, period)
    g = g_snap[u["order"]]
    # Every FoF3 label must be one snapshot group and the other way round.
    pairs = np.unique(np.stack([u["label"], g]), axis=1)
    if len(np.unique(pairs[0])) != pairs.shape[1] or len(np.unique(pairs[1])) != pairs.shape[1]:
        sys.exit("fof3ubd: FoF3's labels and the snapshot's groups do not match one to one")
    return u, g, e2, nfiles


def cmd_bin(snap, prefix, out, period):
    u, g, e2, nfiles = mapped(snap, prefix, period)
    b = u["bound"] == 1
    r = np.zeros(int(b.sum()), dtype=REC)
    r["id"] = u["order"][b] + 1
    r["g"] = g[b]
    r["e"] = u["e"][b] * e2
    r = r[np.lexsort((r["id"], r["g"]))]
    r.tofile(out)
    print(f"fof3ubd: {nfiles} files, {len(u)} particles in {len(np.unique(u['label']))} groups; "
          f"{len(r)} bound in {len(np.unique(r['g']))} groups; energies times {e2:.8g}; "
          f"wrote {out}")


def cmd_compare(snap, prefix, dump, period):
    u, g, e2, nfiles = mapped(snap, prefix, period)
    d = cd.load(H5DUMP, dump)
    order = np.argsort(d["id"])
    k = order[np.searchsorted(d["id"][order], u["order"] + 1)]
    if not np.array_equal(d["id"][k], u["order"] + 1):
        sys.exit("fof3ubd: a FoF3 particle is not in the dump")
    if len(d["id"]) != len(u):
        print(f"fof3ubd: the dump has {len(d['id'])} particles, FoF3 {len(u)}")
    if not np.array_equal(d["g"][k], g):
        sys.exit("fof3ubd: the dump's groups are not the snapshot's")
    bf, bd = u["bound"] == 1, d["b"][k]
    both = bf & bd
    ef, ed = u["e"] * e2, d["e"][k]
    # S per group: median |E| of the dump's bound particles.
    gs = g[bd]
    S = {}
    for grp in np.unique(gs):
        S[int(grp)] = float(np.median(np.abs(ed[bd][gs == grp])))
    Sp = np.array([S.get(int(x), np.nan) for x in g[both]])
    rel = np.abs(ef[both] - ed[both]) / Sp
    print(f"fof3ubd: {nfiles} files; {len(u)} particles; bound: FoF3 {int(bf.sum())}, dump "
          f"{int(bd.sum())}, both {int(both.sum())}, only FoF3 {int((bf & ~bd).sum())}, only "
          f"dump {int((bd & ~bf).sum())}")
    print(f"fof3ubd: groups with a sub-halo: FoF3 {len(np.unique(g[bf]))}, dump "
          f"{len(np.unique(gs))}")
    if len(rel):
        print(f"fof3ubd: |dE|/S over particles bound in both: median {np.median(rel):.3g}, "
              f"99.9% {np.percentile(rel, 99.9):.3g}, largest {rel.max():.3g}")
    groups_differ = len(np.unique(g[bf != bd]))
    print(f"fof3ubd: groups whose bound sets differ: {groups_differ}")
    mf = u["most"] == 1
    listing = subprocess.run([H5DUMP, "-n", dump], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL).stdout.decode()
    if "/MostBound" in listing and "/RefinedEnergy" in listing:
        md = cd.raw(H5DUMP, dump, "/MostBound", np.uint8)[k].astype(bool)
        rd = cd.raw(H5DUMP, dump, "/RefinedEnergy", "<f8")[k]
    else:   # a dump written without --refine
        md, rd = None, None
    if md is None:
        # Without --refine the dump's most bound particle is its lowest energy.
        md = np.zeros(len(u), dtype=bool)
        idx = np.flatnonzero(bd)
        o = np.lexsort((u["order"][idx], ed[idx], g[idx]))
        first = np.r_[True, g[idx][o][1:] != g[idx][o][:-1]]
        md[idx[o][first]] = True
    cf = dict(zip(g[mf].tolist(), u["order"][mf].tolist()))
    cdump = dict(zip(g[md].tolist(), u["order"][md].tolist()))
    same = sum(1 for x in cdump if cf.get(x) == cdump[x])
    print(f"fof3ubd: most bound particle the same in {same} of {len(cdump)} groups "
          f"(FoF3 names one in {len(cf)})")
    if rd is not None:
        rf = u["r"] * e2
        m = np.isfinite(rf) & np.isfinite(rd)
        mm = np.isfinite(rf) != np.isfinite(rd)
        if m.any():
            Sr = np.array([S.get(int(x), np.nan) for x in g[m]])
            rr = np.abs(rf[m] - rd[m]) / Sr
            print(f"fof3ubd: refined energies: {int(m.sum())} in both, {int(mm.sum())} in one "
                  f"only; |dE|/S largest {np.nanmax(rr):.3g}")
        else:
            print(f"fof3ubd: refined energies: none in both, {int(mm.sum())} in one only")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    period = 1.0
    if "--period" in sys.argv:
        period = float(sys.argv[sys.argv.index("--period") + 1])
        args.remove(sys.argv[sys.argv.index("--period") + 1])
    if len(args) == 4 and args[0] == "bin":
        cmd_bin(args[1], args[2], args[3], period)
    elif len(args) == 4 and args[0] == "compare":
        cmd_compare(args[1], args[2], args[3], period)
    else:
        sys.exit(__doc__)
