#!/usr/bin/env python3
"""Check a kernel's centre refinement against the definition's
(design/subhalo-checks.md, "Centre refinement").

  centrecheck.py <reference.particles.bin> <reference.centre.bin> <dump.hdf5>
                 --tau T [--single-against <double dump.hdf5>] [--list N]

  <reference.particles.bin>  unbindspec --cap C --refine K: every bound
                             particle (24-byte records: int64 particle id,
                             int64 group id, float64 energy); gives S, the
                             median |E| of each group's bound particles
  <reference.centre.bin>     the .centre.bin file of the same run: each
                             refined group's K particles with their refined
                             energies, most bound first
  <dump.hdf5>                unbind --cap C --refine K --dump: ParticleIDs,
                             GroupIDs, Bound, Energy, MostBound,
                             RefinedEnergy (NaN where not refined)

Both must use the same cap and K. The rule, per group refined in either
result:
  1. the group is refined in both;
  2. the kernel's most bound particle is the reference's, or it is one of
     the reference's refined particles and its reference energy is within
     T*S of the reference's lowest (a tie within the tolerance);
  3. every particle refined in both has its refined energy within T*S of
     the reference's.
And over every group of the dump, as a check of the dump itself: a group
with a sub-halo has exactly one MostBound particle, bound, which in a refined
group has the lowest RefinedEnergy and otherwise the lowest Energy (ties by
particle id); a group without one has none.

Groups whose bound sets differ (the amended pass rule allows that) are
judged by the same rule and counted.

--single-against gives a single-precision kernel the allowance unbindcheck
gives it ("Tolerance for single-precision kernels"): n is the 99.9th
percentile of |E_single - E_double| / S over the particles refined in both
this dump and the same kernel's double-precision dump, and T becomes
min(T + 10 n, 10 T) in rules 2 and 3. The double-precision form must pass
at T by itself.

Exit status 0 = pass, 1 = fail, 2 = input error. numpy and HDF5's h5dump
(H5DUMP, default `h5dump`).
"""
import argparse
import os
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
        r = subprocess.run([H5DUMP, "-d", dataset, "-b", "LE", "-o", tmp, path],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != 0:
            sys.exit(f"centrecheck: cannot read {dataset} from {path} (a dump written "
                     f"without --refine has no MostBound or RefinedEnergy)")
        return np.fromfile(tmp, dtype=dtype)
    finally:
        os.unlink(tmp)


def groups_of(g):
    """Order, unique values, starts and counts of the groups in g."""
    o = np.argsort(g, kind="stable")
    u, s, c = np.unique(g[o], return_index=True, return_counts=True)
    return o, u, s, c


def lowest(ids, e):
    """Index of the lowest e, ties by id."""
    return int(np.lexsort((ids, e))[0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("centre")
    ap.add_argument("dump")
    ap.add_argument("--tau", type=float, required=True)
    ap.add_argument("--single-against")
    ap.add_argument("--list", type=int, default=20)
    a = ap.parse_args()
    T = a.tau

    ref = np.fromfile(a.reference, dtype=REC)
    cen = np.fromfile(a.centre, dtype=REC)
    pid = raw(a.dump, "/ParticleIDs", "<i8")
    gid = raw(a.dump, "/GroupIDs", "<i8")
    bnd = raw(a.dump, "/Bound", "u1").astype(bool)
    en = raw(a.dump, "/Energy", "<f8")
    most = raw(a.dump, "/MostBound", "u1").astype(bool)
    ren = raw(a.dump, "/RefinedEnergy", "<f8")
    if not (len(pid) == len(gid) == len(bnd) == len(en) == len(most) == len(ren)):
        sys.exit("centrecheck: the dump's datasets differ in length")
    nid = int(max(pid.max(), ref["id"].max() if len(ref) else 0)) + 1

    # S per group, as unbindcheck takes it: the element at n/2 of the sorted |E|.
    S = {}
    o, u, s, c = groups_of(ref["g"])
    ae = np.abs(ref["e"][o])
    for g, st, n in zip(u.tolist(), s.tolist(), c.tolist()):
        S[g] = float(np.partition(ae[st:st + n], n // 2)[n // 2])

    # Bound sets: particles bound in exactly one result, per group of the dump.
    rb = np.zeros(nid, dtype=bool)
    rb[ref["id"]] = True
    db = np.zeros(nid, dtype=bool)
    db[pid[bnd]] = True
    differ_by_group = {}
    diff = rb[pid] != db[pid]
    for g in np.unique(gid[diff]).tolist():
        differ_by_group[g] = int((diff & (gid == g)).sum())

    allowance = ""
    if a.single_against:
        dp = raw(a.single_against, "/ParticleIDs", "<i8")
        dg = raw(a.single_against, "/GroupIDs", "<i8")
        dr = raw(a.single_against, "/RefinedEnergy", "<f8")
        e_single = np.full(nid, np.nan)
        e_double = np.full(nid, np.nan)
        e_single[pid] = ren
        e_double[dp] = dr
        g_of = np.zeros(nid, dtype=np.int64)
        g_of[dp] = dg
        both = np.flatnonzero(~np.isnan(e_single) & ~np.isnan(e_double))
        Sb = np.array([S.get(g, np.nan) for g in g_of[both].tolist()])
        rel = np.abs(e_single[both] - e_double[both]) / Sb
        rel = rel[np.isfinite(rel)]
        n = float(np.percentile(rel, 99.9)) if len(rel) else 0.0
        T = min(a.tau + 10 * n, 10 * a.tau)
        allowance = (f"; single-precision allowance against "
                     f"{os.path.basename(a.single_against)}: n = {n:.3g} over {len(rel)} "
                     f"refined particles, tolerance = {T:.6g} S")

    failures = []

    # The dump's own MostBound flags.
    od, ud, sd, cd = groups_of(gid)
    kernel_centre, kernel_refined = {}, {}
    for g, st, n in zip(ud.tolist(), sd.tolist(), cd.tolist()):
        k = od[st:st + n]
        b = k[bnd[k]]
        flagged = k[most[k]]
        rk = k[~np.isnan(ren[k])]
        if len(b) == 0:
            if len(flagged):
                failures.append(f"group {g}: no sub-halo but {len(flagged)} MostBound flags")
            if len(rk):
                failures.append(f"group {g}: no sub-halo but refined particles")
            continue
        if len(flagged) != 1 or not bnd[flagged[0]]:
            failures.append(f"group {g}: {len(flagged)} MostBound flags (bound: "
                            f"{bnd[flagged].tolist()})")
            continue
        if len(rk):
            if not bnd[rk].all():
                failures.append(f"group {g}: a refined particle is not bound")
            want = rk[lowest(pid[rk], ren[rk])]
            kernel_refined[g] = dict(zip(pid[rk].tolist(), ren[rk].tolist()))
        else:
            want = b[lowest(pid[b], en[b])]
        if want != flagged[0]:
            failures.append(f"group {g}: MostBound is particle {pid[flagged[0]]}, the lowest "
                            f"{'refined ' if len(rk) else ''}energy is particle {pid[want]}")
        kernel_centre[g] = int(pid[flagged[0]])

    # The reference's refined groups.
    ref_refined, ref_centre = {}, {}
    oc, uc, sc, cc = groups_of(cen["g"])
    for g, st, n in zip(uc.tolist(), sc.tolist(), cc.tolist()):
        k = np.sort(oc[st:st + n])     # file order: most bound first
        ref_refined[g] = dict(zip(cen["id"][k].tolist(), cen["e"][k].tolist()))
        ref_centre[g] = int(cen["id"][k[0]])

    groups = sorted(set(ref_refined) | set(kernel_refined))
    same = ties = differ_sets = 0
    worst = 0.0
    overlap = []
    rows = []
    for g in groups:
        if g not in ref_refined or g not in kernel_refined:
            failures.append(f"group {g}: refined only in the "
                            f"{'reference' if g in ref_refined else 'kernel'}")
            continue
        Sg = S.get(g, 0.0)
        if not Sg > 0:
            failures.append(f"group {g}: no S (the reference has no bound particle there)")
            continue
        differ_sets += g in differ_by_group
        rr, kr = ref_refined[g], kernel_refined[g]
        shared = set(rr) & set(kr)
        overlap.append(len(shared) / max(len(rr), len(kr)))
        de = max((abs(kr[i] - rr[i]) / Sg for i in shared), default=0.0)
        worst = max(worst, de)
        if de > T:
            failures.append(f"group {g}: a refined energy differs by {de:.3g} S")
        kc, rc = kernel_centre.get(g), ref_centre[g]
        if kc == rc:
            same += 1
        elif kc in rr and rr[kc] - rr[rc] <= T * Sg:
            ties += 1
            rows.append(f"  group {g}: kernel {kc}, reference {rc}, "
                        f"{(rr[kc] - rr[rc]) / Sg:.3g} S apart (a tie)")
        else:
            failures.append(f"group {g}: most bound particle {kc} in the kernel, {rc} in the "
                            f"reference" + (f", {(rr[kc] - rr[rc]) / Sg:.3g} S above it"
                                            if kc in rr else ", not among its refined particles"))

    ov = np.array(overlap) if overlap else np.zeros(1)
    print(f"reference {os.path.basename(a.reference)} + {os.path.basename(a.centre)}; "
          f"kernel {os.path.basename(a.dump)}; tau {a.tau}{allowance}")
    print(f"refined groups: {len(ref_refined)} in the reference, {len(kernel_refined)} in "
          f"the kernel; with bound sets that differ: {differ_sets}")
    print(f"most bound particle: the same in {same}, a tie within tau*S in {ties}")
    print(f"refined energies of particles refined in both: largest |dE|/S {worst:.3g}")
    print(f"refined particles shared, per group (fraction of K): median {np.median(ov):.4f}, "
          f"smallest {ov.min():.4f}")
    for r in rows[:a.list]:
        print(r)
    for f in failures[:a.list]:
        print("FAIL " + f)
    if len(failures) > a.list:
        print(f"... {len(failures) - a.list} more failures")
    print("CENTRECHECK " + ("PASS" if not failures else f"FAIL ({len(failures)})"))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
