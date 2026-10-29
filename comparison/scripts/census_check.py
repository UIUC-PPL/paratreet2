# Emulate the candidate-set max_size algorithm exactly as implemented in
# cosmo_fof_hacc.cpp and compare against brute force, over many random
# distributions including the adversarial ones (heavy percolation, one
# component spread thin over every rank, all-singleton).
import random
from collections import defaultdict

def emulate(slices_per_rank, nranks):
    # slices_per_rank[r] = {label: count}  (already merged by label, as the
    # C++ does by sorting the per-index counts and merging)
    # (b) threshold
    Lmax = max((max(d.values()) if d else 0) for d in slices_per_rank)
    T = max(1, Lmax // nranks)
    # (c)/(d) candidate slices, hash-partitioned, partial sums per label
    part = defaultdict(int)
    for d in slices_per_rank:
        for lab, c in d.items():
            if c >= T:
                part[lab] += c
    B = max(part.values()) if part else 0
    # (e) shortlist
    floor_p = B - nranks * T
    shortlist = sorted(lab for lab, p in part.items() if p > floor_p)
    # (f) exact sums over ALL slices for the shortlist
    exact = {}
    for lab in shortlist:
        exact[lab] = sum(d.get(lab, 0) for d in slices_per_rank)
    mx = max(exact.values()) if exact else 0
    return mx, Lmax, T, len(part), len(shortlist), B

def brute(slices_per_rank):
    tot = defaultdict(int)
    for d in slices_per_rank:
        for lab, c in d.items():
            tot[lab] += c
    return max(tot.values()) if tot else 0

def make(kind, nranks, rng):
    s = [defaultdict(int) for _ in range(nranks)]
    if kind == "singletons":
        for i in range(2000):
            s[rng.randrange(nranks)][i] += 1
    elif kind == "thin_giant":
        # one component with an equal thin slice on every rank, plus a
        # compact component that owns the global max local slice
        for r in range(nranks):
            s[r][0] += rng.randint(8, 12)
        s[rng.randrange(nranks)][1] += 100
        for i in range(2, 500):
            s[rng.randrange(nranks)][i] += rng.randint(1, 5)
    elif kind == "percolated":
        for r in range(nranks):
            s[r][0] += rng.randint(1000, 5000)
        for i in range(1, 3000):
            for _ in range(rng.randint(1, 4)):
                s[rng.randrange(nranks)][i] += rng.randint(1, 200)
    elif kind == "random":
        for i in range(rng.randint(1, 800)):
            for _ in range(rng.randint(1, nranks)):
                s[rng.randrange(nranks)][i] += rng.randint(1, 300)
    elif kind == "tie":
        # two components of exactly equal total, split differently
        for r in range(nranks):
            s[r][0] += 10
        s[0][1] += 10 * nranks
    return [dict(d) for d in s]

rng = random.Random(12345)
bad = 0
worst_short = 0
for trial in range(4000):
    nranks = rng.choice([1, 2, 3, 8, 16, 128, 1024])
    kind = rng.choice(["singletons", "thin_giant", "percolated", "random", "tie"])
    s = make(kind, nranks, rng)
    if not any(s): continue
    got, Lmax, T, ncand, nshort, B = emulate(s, nranks)
    want = brute(s)
    worst_short = max(worst_short, nshort)
    if got != want:
        bad += 1
        if bad <= 5:
            print(f"MISMATCH kind={kind} nranks={nranks} got={got} want={want} "
                  f"Lmax={Lmax} T={T} cand={ncand} short={nshort} B={B}")
    # the two invariants the C++ also asserts
    assert B <= want, f"B={B} exceeds true max {want}"
    assert got >= B
print(f"trials done, mismatches={bad}, largest shortlist seen={worst_short}")
