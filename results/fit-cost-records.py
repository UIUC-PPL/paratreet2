#!/usr/bin/env python3
"""Refit the subtree-pair cost records produced by FOF_COST_PROBE=1.

Usage: fit-cost-records.py <records.csv> [<records2.csv> ...]

The run already fits a linear model in place, per process, and prints it as
FOF3COST lines in the job log; that is the primary result. This script is
for the second question: whether a DIFFERENT model form fits better, which
needs the raw records process 0 writes.

Columns: kind, t_us, m1_npairs, m2_expected, m3_nsum
  kind 0 = phaseA self pair, 1 = phaseA cross pair, 2 = phaseB pair
  m1 = n_a * n_b                    what unitCost uses today
  m2 = rho_a * rho_b * V_int * Vb   expected pairs within the linking length
  m3 = n_a + n_b                    descent size

Reports, per phase: how much of the total time each feature explains alone,
the joint linear fit, and a log-log fit (which answers "is the cost a power
law in this feature", the useful form if a single feature dominates).
No dependencies beyond the standard library.
"""
import csv, math, sys
from collections import defaultdict

KIND = {0: "A_self", 1: "A_cross", 2: "B"}
FEATURES = ["m1_npairs", "m2_expected", "m3_nsum"]


def solve(a, b):
    """Gaussian elimination with partial pivoting; a is n x n, b is n."""
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        if abs(m[p][c]) < 1e-30:
            return None
        m[c], m[p] = m[p], m[c]
        for r in range(n):
            if r == c:
                continue
            f = m[r][c] / m[c][c]
            for j in range(c, n + 1):
                m[r][j] -= f * m[c][j]
    return [m[i][n] / m[i][i] for i in range(n)]


def fit(rows, cols):
    """Least squares of t_us on [1] + cols. Returns (coefficients, R^2)."""
    k = len(cols) + 1
    xtx = [[0.0] * k for _ in range(k)]
    xty = [0.0] * k
    sy = syy = 0.0
    for r in rows:
        x = [1.0] + [r[c] for c in cols]
        y = r["t_us"]
        sy += y
        syy += y * y
        for i in range(k):
            xty[i] += x[i] * y
            for j in range(k):
                xtx[i][j] += x[i] * x[j]
    n = len(rows)
    coef = solve(xtx, xty)
    if coef is None:
        return None, 0.0
    mean = sy / n
    ss_tot = syy - n * mean * mean
    ss_res = syy - sum(coef[i] * xty[i] for i in range(k))
    return coef, (1 - ss_res / ss_tot) if ss_tot > 0 else 0.0


def loglog(rows, col):
    """Fit log t = a + b log x over rows with both positive. Returns (b, R^2, n)."""
    pts = [(math.log(r[col]), math.log(r["t_us"]))
           for r in rows if r[col] > 0 and r["t_us"] > 0]
    if len(pts) < 8:
        return None, 0.0, len(pts)
    n = len(pts)
    sx = sum(p[0] for p in pts); sy = sum(p[1] for p in pts)
    sxx = sum(p[0] * p[0] for p in pts); sxy = sum(p[0] * p[1] for p in pts)
    syy = sum(p[1] * p[1] for p in pts)
    d = n * sxx - sx * sx
    if abs(d) < 1e-30:
        return None, 0.0, n
    b = (n * sxy - sx * sy) / d
    a = (sy - b * sx) / n
    ss_tot = syy - sy * sy / n
    ss_res = syy - 2 * (a * sy + b * sxy) + n * a * a + 2 * a * b * sx + b * b * sxx
    return b, (1 - ss_res / ss_tot) if ss_tot > 0 else 0.0, n


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    rows_by_kind = defaultdict(list)
    for path in sys.argv[1:]:
        with open(path) as fh:
            for row in csv.DictReader(fh):
                rows_by_kind[int(row["kind"])].append(
                    {k: float(v) for k, v in row.items() if k != "kind"})

    for kind in sorted(rows_by_kind):
        rows = rows_by_kind[kind]
        total = sum(r["t_us"] for r in rows)
        print(f"\n=== {KIND.get(kind, kind)}  pairs {len(rows)}  "
              f"total {total/1e6:.3f} s  mean {total/len(rows):.2f} us  "
              f"max {max(r['t_us'] for r in rows):.1f} us")

        print("  each feature alone:")
        for c in FEATURES:
            _, r2 = fit(rows, [c])
            slope, lr2, n = loglog(rows, c)
            extra = (f"   log-log slope {slope:.2f} R2 {lr2:.3f} on {n} pairs"
                     if slope is not None else "")
            print(f"    {c:<14} linear R2 {r2:.4f}{extra}")

        coef, r2 = fit(rows, FEATURES)
        if coef:
            print(f"  all three:      R2 {r2:.4f}   "
                  f"c0 {coef[0]:.4g}  " +
                  "  ".join(f"{c} {coef[i+1]:.4g}" for i, c in enumerate(FEATURES)))

        # Where the time actually is: the tail usually decides the wall.
        s = sorted(rows, key=lambda r: -r["t_us"])
        for frac in (0.01, 0.1):
            k = max(1, int(len(s) * frac))
            print(f"  top {frac*100:>4.0f}% of pairs hold "
                  f"{sum(r['t_us'] for r in s[:k])/total*100:5.1f}% of the time")


main()
