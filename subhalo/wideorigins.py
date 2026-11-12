#!/usr/bin/env python3
"""Groups at least half the box wide: where they are, and how much their
first frame depends on the particle its periodic offsets are taken from.

  wideorigins.py list <snapshot.hdf5> [--write PREFIX]
  wideorigins.py compare <snapshot.hdf5> <dump A> <dump B>
  wideorigins.py hbt <snapshot.hdf5> <HBT-HERONS log> <out.txt>

A group's frame position is the mass-weighted mean of nearest-image offsets
from one of its particles, the origin. For a group narrower than half the
box every origin gives the same frame. For a wider group it does not.
`unbind` and `unbindspec` take the group's lowest particle id; HBT-HERONS
takes, for the first frame, the first particle of its merged list before it
sorts the list by id, which the input does not fix (design/subhalo-checks.md,
"Groups at least half the box wide").

list     prints each wide group and, for several origins, how far the first
         frame moves from the lowest-id one: in box lengths and as the
         Hubble term H a |dX| in km/s, next to the group's velocity
         dispersion. With --write it writes PREFIX.<origin>.txt files for
         `unbind --first-origin` (lines: group id, particle id).
compare  per wide group, two `unbind --dump` results: bound counts and the
         particles bound in exactly one of them.

hbt      reads the FIRSTPARTICLE lines of an HBT-HERONS run built with the
         one-line diagnostic of the design note (group size, id of the
         particle its first frame started from), finds each particle's group
         and writes the file for `unbind --first-origin`. For the wide
         groups it prints how far that frame is from the lowest-id one.

Needs numpy and HDF5's h5dump (through capdistance.py's readers).
"""
import os
import sys

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capdistance as cd  # noqa: E402

H5DUMP = os.environ.get("H5DUMP", "/opt/cray/pe/hdf5/1.14.3.9/gnu/12.3/bin/h5dump")
ORIGINS = ("highest_id", "low_end", "high_end", "middle", "random1", "random2")


def read(snap):
    s = {"box": float(cd.attribute(H5DUMP, snap, "/Header/BoxSize")),
         "a": float(cd.attribute(H5DUMP, snap, "/Cosmology/Scale-factor")),
         "H": float(cd.attribute(H5DUMP, snap, "/Cosmology/H [internal units]")),
         "null": int(cd.attribute(H5DUMP, snap, "/Parameters/FOF:group_id_default"))}
    g = cd.raw(H5DUMP, snap, "/PartType1/FOFGroupIDs", "<i8")
    sel = np.flatnonzero(g != s["null"])
    order = sel[np.lexsort((sel, g[sel]))]          # by group, then by position in the file
    s["g"] = g[order]
    s["id"] = cd.raw(H5DUMP, snap, "/PartType1/ParticleIDs", "<i8")[order]
    s["x"] = cd.raw(H5DUMP, snap, "/PartType1/Coordinates", "<f8").reshape(-1, 3)[order]
    s["v"] = cd.raw(H5DUMP, snap, "/PartType1/Velocities", "<f4").reshape(-1, 3)[order].astype(np.float64)
    s["m"] = cd.raw(H5DUMP, snap, "/PartType1/Masses", "<f4")[order].astype(np.float64)
    s["start"] = np.flatnonzero(np.r_[True, s["g"][1:] != s["g"][:-1]])
    s["end"] = np.r_[s["start"][1:], len(s["g"])]
    return s


def near(d, box):
    return d - box * np.round(d / box)


def wide_groups(s):
    """(first, last, extent/box) of each group at least half the box wide."""
    out = []
    for b, e in zip(s["start"], s["end"]):
        if e - b < 20:
            continue
        d = near(s["x"][b:e] - s["x"][b], s["box"])
        ext = (d.max(0) - d.min(0)) / s["box"]
        if ext.max() >= 0.5:
            out.append((b, e, ext))
    return out


def frame(s, b, e, k):
    """First-frame position of group b..e with particle b + k as origin."""
    d = near(s["x"][b:e] - s["x"][b + k], s["box"])
    return s["x"][b + k] + (d * s["m"][b:e, None]).sum(0) / s["m"][b:e].sum()


def cmd_list(snap, prefix):
    s = read(snap)
    wide = wide_groups(s)
    print(f"{snap}: box {s['box']:g}, a {s['a']:g}, H {s['H']:g}; "
          f"{len(s['start'])} groups, {len(wide)} at least half the box wide")
    files = {o: [] for o in ORIGINS}
    rng = np.random.RandomState(20261111)
    for b, e, ext in wide:
        n = e - b
        d = near(s["x"][b:e] - s["x"][b], s["box"])
        axis = int(np.argmax(ext))
        vm = (s["v"][b:e] * s["m"][b:e, None]).sum(0) / s["m"][b:e].sum()
        sigma = float(np.sqrt(((s["v"][b:e] - vm) ** 2).mean()))
        pick = {"highest_id": int(np.argmax(s["id"][b:e])),
                "low_end": int(np.argmin(d[:, axis])),
                "high_end": int(np.argmax(d[:, axis])),
                "middle": int(np.argsort(d[:, axis])[n // 2]),
                "random1": int(rng.randint(n)), "random2": int(rng.randint(n))}
        x0 = frame(s, b, e, int(np.argmin(s["id"][b:e])))
        print(f"group {s['g'][b]}: {n} particles, extent {ext[0]:.3f} {ext[1]:.3f} {ext[2]:.3f} "
              f"of the box, velocity dispersion {sigma:.1f} km/s per axis")
        print("  origin | particle id | first frame moves by (box lengths) | "
              "Hubble term H a |dX| (km/s)")
        for o in ORIGINS:
            k = pick[o]
            shift = near(frame(s, b, e, k) - x0, s["box"])
            dist = float(np.sqrt((shift ** 2).sum()))
            print(f"  {o} | {s['id'][b + k]} | {dist / s['box']:.4f} | {s['H'] * s['a'] * dist:.1f}")
            files[o].append((int(s["g"][b]), int(s["id"][b + k])))
    if prefix:
        for o in ORIGINS:
            with open(f"{prefix}.{o}.txt", "w") as f:
                f.write("# group id, particle id: first-frame origin (wideorigins.py)\n")
                for g, i in files[o]:
                    f.write(f"{g} {i}\n")


def cmd_compare(snap, dump_a, dump_b):
    s = read(snap)
    a, b = cd.load(H5DUMP, dump_a), cd.load(H5DUMP, dump_b)
    assert np.array_equal(a["id"], b["id"]) and np.array_equal(a["g"], b["g"])
    for lo, hi, _ in wide_groups(s):
        sel = a["g"] == s["g"][lo]
        ba, bb = a["b"][sel], b["b"][sel]
        print(f"group {s['g'][lo]}: {hi - lo} particles; bound {int(ba.sum())} against "
              f"{int(bb.sum())}; bound in one result only {int((ba != bb).sum())}")
    other = ~np.isin(a["g"], [s["g"][lo] for lo, _, _ in wide_groups(s)])
    print(f"all other groups: {int((a['b'][other] != b['b'][other]).sum())} particles differ")


def cmd_hbt(snap, log, out):
    s = read(snap)
    g_all = cd.raw(H5DUMP, snap, "/PartType1/FOFGroupIDs", "<i8")
    id_all = cd.raw(H5DUMP, snap, "/PartType1/ParticleIDs", "<i8")
    if not np.array_equal(id_all, np.arange(1, len(id_all) + 1)):
        sys.exit("wideorigins: the snapshot's ParticleIDs are not 1..N")
    size = dict(zip(s["g"][s["start"]].tolist(), (s["end"] - s["start"]).tolist()))
    first = {}
    for line in open(log, errors="replace"):
        w = line.split()
        if len(w) == 3 and w[0] == "FIRSTPARTICLE":
            n, pid = int(w[1]), int(w[2])
            g = int(g_all[pid - 1])
            if size.get(g) != n:
                sys.exit(f"wideorigins: particle {pid} is in group {g} of {size.get(g)} "
                         f"particles, HBT-HERONS printed {n}")
            if g in first:
                sys.exit(f"wideorigins: group {g} is printed twice")
            first[g] = pid
    want = [g for g, n in size.items() if n >= 20]
    missing = [g for g in want if g not in first]
    if missing:
        sys.exit(f"wideorigins: {len(missing)} groups of 20 or more particles have no "
                 f"FIRSTPARTICLE line, e.g. group {missing[0]}")
    with open(out, "w") as f:
        f.write("# group id, particle id: HBT-HERONS's first particle (wideorigins.py hbt)\n")
        for g in sorted(first):
            f.write(f"{g} {first[g]}\n")
    lowest = sum(1 for b in s["start"] if first.get(int(s["g"][b])) == int(s["id"][b]))
    print(f"{len(first)} groups; HBT-HERONS's first particle is the lowest id in {lowest}")
    for b, e, ext in wide_groups(s):
        g = int(s["g"][b])
        k = int(np.flatnonzero(s["id"][b:e] == first[g])[0])
        shift = near(frame(s, b, e, k) - frame(s, b, e, int(np.argmin(s["id"][b:e]))), s["box"])
        dist = float(np.sqrt((shift ** 2).sum()))
        print(f"wide group {g}: {e - b} particles; HBT-HERONS's first particle {first[g]} "
              f"(lowest id {int(s['id'][b:e].min())}); its first frame is {dist / s['box']:.4f} "
              f"box lengths from the lowest-id one, Hubble term {s['H'] * s['a'] * dist:.1f} km/s")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "list":
        cmd_list(sys.argv[2], sys.argv[4] if len(sys.argv) > 4 and sys.argv[3] == "--write" else None)
    elif len(sys.argv) == 5 and sys.argv[1] == "compare":
        cmd_compare(*sys.argv[2:5])
    elif len(sys.argv) == 5 and sys.argv[1] == "hbt":
        cmd_hbt(*sys.argv[2:5])
    else:
        sys.exit(__doc__)
