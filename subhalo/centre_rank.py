#!/usr/bin/env python3
"""Where the exact result's most bound particle ranks in a capped result's
energy order, per sampled group (rank 0 = the capped result's lowest energy).

  centre_rank.py <exact.particles.bin> <capped.particles.bin> <cap>

Both files have 24-byte records (int64 particle id, int64 group id, float64
energy). A group counts as sampled if its bound count in the capped file is
above the cap. Used for "HBT-HERONS on the Mac" in design/subhalo-checks.md:
it says how many of the lowest capped energies a centre refinement would have
to look at. numpy only.
"""
import sys

import numpy as np

REC = np.dtype([("id", "<i8"), ("g", "<i8"), ("e", "<f8")])
ex = np.fromfile(sys.argv[1], dtype=REC)
cp = np.fromfile(sys.argv[2], dtype=REC)
cap = int(sys.argv[3])
ex = ex[np.lexsort((ex["e"], ex["g"]))]
cp = cp[np.lexsort((cp["e"], cp["g"]))]
eg, es = np.unique(ex["g"], return_index=True)
cg, cs, cn = np.unique(cp["g"], return_index=True, return_counts=True)
best = dict(zip(eg.tolist(), ex["id"][es].tolist()))
ranks = []
for g, s, n in zip(cg.tolist(), cs.tolist(), cn.tolist()):
    if n <= cap or g not in best:
        continue
    hit = np.flatnonzero(cp["id"][s:s + n] == best[g])
    ranks.append(int(hit[0]) if len(hit) else -1)
r = np.array(ranks)
ok = r[r >= 0]
print(f"{sys.argv[2].split('/')[-1]}: {len(r)} sampled groups; exact most bound particle not "
      f"bound in the capped result: {int((r < 0).sum())}; rank 0: {100 * (ok == 0).mean():.1f}%, "
      f"below 10: {100 * (ok < 10).mean():.1f}%, below 100: {100 * (ok < 100).mean():.1f}%; "
      f"median {int(np.median(ok))}, 99th percentile {int(np.percentile(ok, 99))}, "
      f"largest {int(ok.max())}")
