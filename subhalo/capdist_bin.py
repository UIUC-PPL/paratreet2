#!/usr/bin/env python3
"""How far one unbinding result is from another, group by group, from two
.particles.bin files (24-byte records: int64 particle id, int64 group id,
float64 energy; hbtref's and unbindspec's layout).

  capdist_bin.py <snapshot.hdf5> <reference.particles.bin> <candidate.particles.bin>
                 [--above C] [--first] [--ref-first]

The same measures as paratreet2/subhalo/capdistance.py, which reads `unbind
--dump` files: per group above C particles, with N the reference's bound
count,
  count     (candidate's bound count - N) / N
  members   particles bound in exactly one of the two results, over N
  energies  |E_candidate - E_reference| / S over the particles bound in both,
            S the median |E_reference| of the group's bound particles
  centre    whether the candidate's particle of lowest energy is the
            reference's, and how far apart the two are (softening lengths)
With --first the candidate is an HBT-HERONS result, whose records are in its
own order: the first record of a group is the particle HBT-HERONS reports as
most bound, after its centre refinement. A second centre column compares
that particle with the reference's particle of lowest energy. With
--ref-first the reference is an HBT-HERONS result too, and its centre is its
first record.

Used for "HBT-HERONS on the Mac" in design/subhalo-checks.md. numpy and
HDF5's h5dump (H5DUMP in the environment, default `h5dump`); no h5py.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

import numpy as np

H5DUMP = os.environ.get("H5DUMP", "h5dump")
REC = np.dtype([("id", "<i8"), ("g", "<i8"), ("e", "<f8")])


def raw(path, dataset, dtype):
    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as f:
        tmp = f.name
    try:
        subprocess.run([H5DUMP, "-d", dataset, "-b", "LE", "-o", tmp, path],
                       check=True, stdout=subprocess.DEVNULL)
        return np.fromfile(tmp, dtype=dtype)
    finally:
        os.unlink(tmp)


def attribute(path, name):
    out = subprocess.run([H5DUMP, "-a", name, path], check=True,
                         stdout=subprocess.PIPE).stdout.decode()
    return re.search(r"\(0\):\s*\"?([-+0-9.eE]+)", out).group(1)


def pct(x, q):
    return float(np.percentile(x, q)) if len(x) else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("snapshot")
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--above", type=int, default=0)
    ap.add_argument("--first", action="store_true")
    ap.add_argument("--ref-first", action="store_true",
                    help="the reference is an HBT-HERONS result too: its centre is its "
                         "first record, not its particle of lowest energy")
    a = ap.parse_args()

    box = float(attribute(a.snapshot, "/Header/BoxSize"))
    scale = float(attribute(a.snapshot, "/Cosmology/Scale-factor"))
    eps = min(float(attribute(a.snapshot, "/Parameters/Gravity:comoving_DM_softening")),
              float(attribute(a.snapshot, "/Parameters/Gravity:max_physical_DM_softening")) / scale)
    null = int(float(attribute(a.snapshot, "/Parameters/FOF:group_id_default")))
    sid = raw(a.snapshot, "/PartType1/ParticleIDs", "<i8")
    if not np.array_equal(sid, np.arange(1, len(sid) + 1)):
        sys.exit("capdist_bin: the snapshot's ParticleIDs are not 1..N")
    fof = raw(a.snapshot, "/PartType1/FOFGroupIDs", "<i8")
    pos = raw(a.snapshot, "/PartType1/Coordinates", "<f8").reshape(-1, 3)
    grouped = fof != null
    ng = int(fof[grouped].max()) + 1
    size = np.bincount(fof[grouped], minlength=ng)

    ref = np.fromfile(a.reference, dtype=REC)
    cand = np.fromfile(a.candidate, dtype=REC)
    n = len(sid) + 1
    eref = np.full(n, np.nan)
    ecand = np.full(n, np.nan)
    eref[ref["id"]] = ref["e"]
    ecand[cand["id"]] = cand["e"]
    if not (np.array_equal(fof[ref["id"] - 1], ref["g"]) and
            np.array_equal(fof[cand["id"] - 1], cand["g"])):
        sys.exit("capdist_bin: a record's group is not the snapshot's group of that particle")
    rb, cb = ~np.isnan(eref), ~np.isnan(ecand)
    gid = np.zeros(n, dtype=np.int64)
    gid[1:] = np.where(grouped, fof, 0)
    w = np.zeros(n, dtype=bool)
    w[1:] = grouped
    nref = np.bincount(gid[w], weights=rb[w], minlength=ng).astype(np.int64)
    ncand = np.bincount(gid[w], weights=cb[w], minlength=ng).astype(np.int64)
    lost = np.bincount(gid[w], weights=(rb & ~cb)[w], minlength=ng).astype(np.int64)
    gained = np.bincount(gid[w], weights=(~rb & cb)[w], minlength=ng).astype(np.int64)

    # Each group's particle ids, and the first record of each group in the
    # candidate file (HBT-HERONS's own most bound particle).
    order = np.argsort(gid[1:], kind="stable") + 1
    start = np.zeros(ng + 1, dtype=np.int64)
    start[1:] = np.cumsum(np.bincount(gid[1:], minlength=ng))
    first = {}
    if a.first:
        g, idx = np.unique(cand["g"], return_index=True)
        first = dict(zip(g.tolist(), cand["id"][idx].tolist()))

    ref_first = {}
    if a.ref_first:
        g, idx = np.unique(ref["g"], return_index=True)
        ref_first = dict(zip(g.tolist(), ref["id"][idx].tolist()))

    one_sided = np.nonzero((nref > 0) != (ncand > 0))[0]
    print(f"reference {os.path.basename(a.reference)}: {int(rb.sum())} bound in "
          f"{int((nref > 0).sum())} groups")
    print(f"candidate {os.path.basename(a.candidate)}: {int(cb.sum())} bound in "
          f"{int((ncand > 0).sum())} groups")
    print(f"bound in the reference only: {int(lost.sum())}; in the candidate only: "
          f"{int(gained.sum())}; net {int(cb.sum()) - int(rb.sum()):+d}; groups with a "
          f"sub-halo on one side only: {len(one_sided)}"
          + "".join(f" (group {k}: {size[k]} particles, {nref[k]} bound in the reference, "
                    f"{ncand[k]} in the candidate)" for k in one_sided[:5]))
    untouched = (size <= a.above) & (size > 0)
    untouched[0] = untouched[0] and null != 0
    print(f"groups of at most {a.above} particles: {int(untouched.sum())}, with "
          f"{int((lost + gained)[untouched].sum())} particles differing")

    def shift(i, j):
        d = pos[i - 1] - pos[j - 1]
        d -= box * np.round(d / box)
        return float(np.sqrt((d * d).sum())) / eps

    rows = []
    for k in np.nonzero((size > a.above) & (nref > 0))[0]:
        if k == 0 and null == 0:
            continue
        ids = order[start[k]:start[k + 1]]
        r, c = rb[ids], cb[ids]
        S = np.median(np.abs(eref[ids][r]))
        both = r & c
        row = {"size": int(size[k]), "n": int(nref[k]),
               "count": (ncand[k] - nref[k]) / nref[k],
               "members": (lost[k] + gained[k]) / nref[k],
               "de": np.abs(ecand[ids][both] - eref[ids][both]) / S}
        if c.any():
            ir = ids[r][np.argmin(eref[ids][r])]
            if a.ref_first:
                ir = ref_first[int(k)]
            ic = ids[c][np.argmin(ecand[ids][c])]
            row["same"] = bool(ir == ic)
            row["shift"] = shift(ic, ir)
            if a.first:
                row["same_first"] = bool(first[int(k)] == ir)
                row["shift_first"] = shift(first[int(k)], ir)
        rows.append(row)

    edges = [e for e in (1000, 10000, 100000) if e > a.above]
    bins = [(a.above, edges[0])] if edges else []
    bins += list(zip(edges, edges[1:])) + [((edges[-1] if edges else a.above), 10 ** 12)]
    if len(bins) > 1:
        bins.append((a.above, 10 ** 12))
    print()
    print("particles in group | groups | bound (reference) | count: mean, median |.|, 99% |.|, "
          "largest |.| (%) | members: median, 99%, largest (%) | energies |dE|/S: median, 99% | "
          "lowest energy: same particle (%), shift median, 99%, largest (softening lengths)"
          + (" | reported most bound: same particle (%), shift median, 99%, largest"
             if a.first else ""))
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
        line = (f"{name} | {len(sel)} | {sum(r['n'] for r in sel):,} | "
                f"{cnt.mean():+.3f}, {pct(np.abs(cnt), 50):.3f}, {pct(np.abs(cnt), 99):.3f}, "
                f"{np.abs(cnt).max():.3f} | {pct(mem, 50):.3f}, {pct(mem, 99):.3f}, "
                f"{mem.max():.3f} | {pct(de, 50):.2e}, {pct(de, 99):.2e}")
        for key in ("", "_first") if a.first else ("",):
            same = [r["same" + key] for r in sel if "same" + key in r]
            sh = np.array([r["shift" + key] for r in sel if "shift" + key in r])
            line += (f" | {100.0 * sum(same) / max(1, len(same)):.1f}, {pct(sh, 50):.2f}, "
                     f"{pct(sh, 99):.2f}, {sh.max():.2f}")
        print(line)


if __name__ == "__main__":
    main()
