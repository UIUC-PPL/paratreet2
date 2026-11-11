#!/usr/bin/env python3
"""How far one unbinding result is from another, group by group.

  capdistance.py <reference dump> <candidate dump> [--above N]
                 [--snapshot snap_0000.hdf5] [--h5dump PATH]

Both dumps are files `unbind --dump` wrote from the same snapshot. The
reference is normally the run without a cap and the candidate a run with
`--cap C`; `--above C` then restricts the tables to the groups the cap
sampled (more than C particles). It is a measurement, not a pass rule
(design/subhalo-checks.md, "How far a capped result is from the exact one").

Per group, with N the reference's bound count:
  count      (candidate's bound count - N) / N
  members    particles bound in exactly one of the two results, over N
  energies   |E_candidate - E_reference| / S over the particles bound in
             both, S the median |E_reference| of the group's bound particles
  centre     whether the most bound particle (lowest energy) is the same,
             and with --snapshot how far apart the two are

Needs numpy and HDF5's h5dump (no h5py): datasets are read through
`h5dump -b`.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

import numpy as np


def raw(h5dump, path, dataset, dtype):
    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as f:
        tmp = f.name
    try:
        subprocess.run([h5dump, "-d", dataset, "-b", "LE", "-o", tmp, path],
                       check=True, stdout=subprocess.DEVNULL)
        return np.fromfile(tmp, dtype=dtype)
    finally:
        os.unlink(tmp)


def attribute(h5dump, path, name):
    out = subprocess.run([h5dump, "-a", name, path], check=True,
                         stdout=subprocess.PIPE).stdout.decode()
    return re.search(r"\(0\):\s*\"?([-+0-9.eE]+)", out).group(1)


def load(h5dump, path):
    d = {"id": raw(h5dump, path, "/ParticleIDs", "<i8"),
         "g": raw(h5dump, path, "/GroupIDs", "<i8"),
         "b": raw(h5dump, path, "/Bound", np.uint8).astype(bool),
         "e": raw(h5dump, path, "/Energy", "<f8")}
    return d


def pct(x, q):
    return float(np.percentile(x, q)) if len(x) else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--above", type=int, default=0)
    ap.add_argument("--snapshot")
    ap.add_argument("--h5dump", default="/opt/cray/pe/hdf5/1.14.3.9/gnu/12.3/bin/h5dump")
    a = ap.parse_args()

    ref, cand = load(a.h5dump, a.reference), load(a.h5dump, a.candidate)
    if not (np.array_equal(ref["id"], cand["id"]) and np.array_equal(ref["g"], cand["g"])):
        sys.exit("capdistance: the two dumps do not list the same particles in the same order")
    g, rb, cb, re_, ce = ref["g"], ref["b"], cand["b"], ref["e"], cand["e"]
    ng = int(g.max()) + 1
    size = np.bincount(g, minlength=ng)
    nref = np.bincount(g, weights=rb, minlength=ng).astype(np.int64)
    ncand = np.bincount(g, weights=cb, minlength=ng).astype(np.int64)
    lost = np.bincount(g, weights=rb & ~cb, minlength=ng).astype(np.int64)
    gained = np.bincount(g, weights=~rb & cb, minlength=ng).astype(np.int64)

    # The dumps are packed group by group: each group is one slice.
    start = np.zeros(ng + 1, dtype=np.int64)
    start[1:] = np.cumsum(size)
    order = np.argsort(g, kind="stable")
    if not np.array_equal(order, np.arange(len(g))):
        sys.exit("capdistance: the dump is not packed group by group")

    pos = None
    if a.snapshot:
        box = float(attribute(a.h5dump, a.snapshot, "/Header/BoxSize"))
        eps = float(attribute(a.h5dump, a.snapshot,
                              "/Parameters/Gravity:comoving_DM_softening"))
        sid = raw(a.h5dump, a.snapshot, "/PartType1/ParticleIDs", "<i8")
        if not np.array_equal(sid, np.arange(1, len(sid) + 1)):
            sys.exit("capdistance: the snapshot's ParticleIDs are not 1..N")
        pos = raw(a.h5dump, a.snapshot, "/PartType1/Coordinates", "<f8").reshape(-1, 3)

    groups = np.nonzero((size > a.above) & (nref > 0))[0]
    one_sided = int(((nref > 0) != (ncand > 0)).sum())
    print(f"reference {a.reference}: {int(rb.sum())} bound in {int((nref > 0).sum())} groups")
    print(f"candidate {a.candidate}: {int(cb.sum())} bound in {int((ncand > 0).sum())} groups")
    print(f"bound in the reference only: {int(lost.sum())}; in the candidate only: "
          f"{int(gained.sum())}; net {int(cb.sum()) - int(rb.sum()):+d}; groups with a "
          f"sub-halo on one side only: {one_sided}")
    untouched = (size <= a.above) & (size > 0)
    print(f"groups of at most {a.above} particles: {int(untouched.sum())}, with "
          f"{int((lost + gained)[untouched].sum())} particles differing")

    rows = []
    for k in groups:
        s, e = start[k], start[k + 1]
        r, c = rb[s:e], cb[s:e]
        S = np.median(np.abs(re_[s:e][r]))
        both = r & c
        de = np.abs(ce[s:e][both] - re_[s:e][both]) / S
        row = {"size": int(size[k]), "n": int(nref[k]),
               "count": (ncand[k] - nref[k]) / nref[k],
               "members": (lost[k] + gained[k]) / nref[k], "de": de}
        if c.any():
            ir = s + np.flatnonzero(r)[np.argmin(re_[s:e][r])]
            ic = s + np.flatnonzero(c)[np.argmin(ce[s:e][c])]
            row["same_centre"] = bool(ir == ic)
            if pos is not None:
                d = pos[ref["id"][ic] - 1] - pos[ref["id"][ir] - 1]
                d -= box * np.round(d / box)
                row["shift"] = float(np.sqrt((d * d).sum())) / eps
        rows.append(row)

    edges = [e for e in (1000, 10000, 100000) if e > a.above]
    bins = [(a.above, edges[0])] if edges else []
    bins += list(zip(edges, edges[1:])) + [((edges[-1] if edges else a.above), 10 ** 12)]
    if len(bins) > 1:
        bins.append((a.above, 10 ** 12))
    print()
    print("particles in group | groups | bound (reference) | count: mean, median |.|, 99% |.|, "
          "largest |.| (%) | members: median, 99%, largest (%) | energies |dE|/S: median, 99% | "
          "same most bound particle (%)"
          + (" | its shift in softening lengths: median, 99%, largest" if pos is not None else ""))
    for lo, hi in bins:
        sel = [r for r in rows if lo < r["size"] <= hi]
        if not sel:
            continue
        name = f"{lo + 1:,} to {hi:,}" if hi < 10 ** 12 else f"above {lo:,}"
        if (lo, hi) == (a.above, 10 ** 12) and len(bins) > 1:
            name = f"all above {lo:,}"
        cnt = np.array([r["count"] for r in sel]) * 100
        mem = np.array([r["members"] for r in sel]) * 100
        de = np.concatenate([r["de"] for r in sel])
        centre = [r["same_centre"] for r in sel if "same_centre" in r]
        line = (f"{name} | {len(sel)} | {sum(r['n'] for r in sel):,} | "
                f"{cnt.mean():+.3f}, {pct(np.abs(cnt), 50):.3f}, {pct(np.abs(cnt), 99):.3f}, "
                f"{np.abs(cnt).max():.3f} | {pct(mem, 50):.3f}, {pct(mem, 99):.3f}, "
                f"{mem.max():.3f} | {pct(de, 50):.2e}, {pct(de, 99):.2e} | "
                f"{100.0 * sum(centre) / max(1, len(centre)):.1f}")
        if pos is not None:
            sh = np.array([r["shift"] for r in sel if "shift" in r])
            line += f" | {pct(sh, 50):.2f}, {pct(sh, 99):.2f}, {sh.max():.2f}"
        print(line)


if __name__ == "__main__":
    main()
