#!/usr/bin/env python3
"""FoF3 -U against HBT-HERONS on the same groups, per group and by group
size (design/subhalo-checks.md, "cosmo25: HBT-HERONS on a sub-box").

  sharecmp.py <FoF3 out= prefix> <hbtref prefix>.fof3.particles.bin

FoF3's files are <prefix>.<process>.ubd and <prefix>.<process>.s.ubd: one
40-byte record per particle of every group it unbound (order, label,
energy, refined energy, bound, most bound, refined group, pad). hbtref's
.fof3.particles.bin has one 24-byte record per particle HBT-HERONS kept
bound (order, FoF3 label, energy). Both are native byte order.

Prints per log2 bin of the group's size: groups; the share of groups with
a sub-halo in FoF3 and in HBT-HERONS; the share of their particles bound in
each; the share of groups where the two agree on whether there is a
sub-halo; the share with the same bound count; and the mean of
|bound count difference| / group size. Then the same over all groups, and
the groups where only one of the two finds a sub-halo, by bin.
"""
import glob
import sys

import numpy as np

UBD = np.dtype([("order", "<i8"), ("label", "<i8"), ("energy", "<f8"), ("refined", "<f8"),
                ("bound", "u1"), ("most", "u1"), ("rg", "u1"), ("pad", "u1", 5)])
REF = np.dtype([("order", "<i8"), ("label", "<i8"), ("energy", "<f8")])


def main():
    prefix, ref = sys.argv[1], sys.argv[2]
    files = sorted(glob.glob(prefix + ".*.ubd"))
    if not files:
        sys.exit("sharecmp: no files %s.*.ubd" % prefix)
    lab, bnd = [], []
    for f in files:
        r = np.fromfile(f, dtype=UBD)
        lab.append(r["label"])
        bnd.append(r["bound"].astype(np.int64))
    lab = np.concatenate(lab)
    bnd = np.concatenate(bnd)
    labels, inv, n = np.unique(lab, return_inverse=True, return_counts=True)
    nb_f = np.bincount(inv, weights=bnd).astype(np.int64)
    h = np.fromfile(ref, dtype=REF)
    hl, hc = np.unique(h["label"], return_counts=True)
    pos = np.searchsorted(labels, hl)
    ok = (pos < len(labels)) & (labels[np.minimum(pos, len(labels) - 1)] == hl)
    if not ok.all():
        print("sharecmp: %d HBT-HERONS labels are not FoF3 groups" % int((~ok).sum()))
    nb_h = np.zeros(len(labels), dtype=np.int64)
    nb_h[pos[ok]] = hc[ok]
    print("sharecmp: FoF3 %d files, %d particles in %d groups; HBT-HERONS %d bound particles "
          "in %d groups" % (len(files), len(lab), len(labels), len(h), len(hl)))

    b = np.floor(np.log2(n)).astype(int)
    hf, hh = nb_f > 0, nb_h > 0
    print("sharecmp: bin (particles) groups | with a sub-halo: FoF3 HBT | bound share: FoF3 HBT "
          "| agree on a sub-halo | same bound count | mean |dN|/n | only FoF3 only HBT")

    def line(tag, m):
        g = int(m.sum())
        if g == 0:
            return
        print("sharecmp: %-14s %9d | %.3f %.3f | %.3f %.3f | %.4f | %.4f | %.5f | %d %d" % (
            tag, g, hf[m].mean(), hh[m].mean(), nb_f[m].sum() / n[m].sum(),
            nb_h[m].sum() / n[m].sum(), (hf[m] == hh[m]).mean(), (nb_f[m] == nb_h[m]).mean(),
            (np.abs(nb_f[m] - nb_h[m]) / n[m]).mean(), int((hf[m] & ~hh[m]).sum()),
            int((hh[m] & ~hf[m]).sum())))

    for k in range(b.min(), b.max() + 1):
        line("%d (%d-%d)" % (k, 2 ** k, 2 ** (k + 1) - 1), b == k)
    line("all", np.ones(len(n), dtype=bool))


if __name__ == "__main__":
    main()
