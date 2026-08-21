#!/usr/bin/env python3
"""Charm++ sum-detail (.sumd) trace analyzer. Stdlib only.

Format (ver 7.1, reverse-engineered 2026-08-13 against sumd2b_slice_v3
and verified against Projections' own Time Profile percentages):
  <prefix>.sts        ENTRY CHARE <epid> "<name>" ... maps EP id->name
  <prefix>.<pe>.sumd  3 lines:
    line 1: ver:7.1 cpu:PE/NPES numIntervals:M numEPs:E intervalSize:1.0e-03
    line 2: ExeTimePerEPperInterval <RLE>   busy per-mil of each interval
    line 3: EPCallTimePerInterval  <RLE>    calls begun in each interval
  RLE token "a+b" = value a repeated b times; "a" alone = once. The
  stream is EP-major: EP0's M intervals, then EP1's M, ... E*M values.
  A value v in line 2 means the PE spent v/1000 of that interval
  (intervalSize seconds) inside that EP.

Usage (run from the trace directory, or pass --dir):
  sumd_tool.py totals   --ep 'regex' [--ep 'regex2' ...]
  sumd_tool.py straggler --ep 'regex' [--top N] [--ppn P]
  sumd_tool.py fanout   --ep 'regex' --ppn P
  sumd_tool.py first    --ep 'regex' [--ppn P]
  sumd_tool.py profile  --ep 'regex' [--bin MS]   (utilization vs time)
EP regexes match .sts entry names case-insensitively; every matching
EP id is aggregated under the regex. --ppn = worker PEs per process
(trace PEs are workers only: Frontier 2B = 14, Anvil 2B = 15).
"""
import argparse, glob, os, re, sys
from collections import defaultdict


def find_sts(d):
    sts = glob.glob(os.path.join(d, "*.sts"))
    if not sts:
        sys.exit("no .sts file in " + d)
    return sts[0]


def ep_ids(sts_path, pattern):
    ids = {}
    rx = re.compile(pattern, re.I)
    with open(sts_path) as f:
        for line in f:
            if line.startswith("ENTRY"):
                m = re.match(r'ENTRY\s+\S+\s+(\d+)\s+"([^"]*)"', line)
                if m and rx.search(m.group(2)):
                    ids[int(m.group(1))] = m.group(2)
    return ids


def rle(tokens):
    for t in tokens:
        if '+' in t:
            v, r = t.split('+')
            yield int(v), int(r)
        else:
            yield int(t), 1


def parse_pe(path, want_ids, calls=False, series=False):
    """Per EP id: [busy_ms, first_interval, last_interval, calls,
    optional per-interval list]."""
    with open(path) as f:
        hdr = f.readline().split()
        n_int = int([x for x in hdr if x.startswith("numIntervals")][0].split(':')[1])
        exe = f.readline().split()[1:]
        cal = f.readline().split()[1:] if calls else None
    out = {e: [0.0, None, -1, 0, ([0.0] * n_int if series else None)]
           for e in want_ids}

    def scan(tokens, idx):
        pos = 0
        hi_all = (max(want_ids) + 1) * n_int
        for v, r in rle(tokens):
            if pos >= hi_all:
                break
            start, end = pos, pos + r
            pos = end
            if v == 0:
                continue
            for e in want_ids:
                lo, hi = e * n_int, (e + 1) * n_int
                if end <= lo or start >= hi:
                    continue
                s, t = max(start, lo), min(end, hi)
                a = out[e]
                if idx == 0:
                    a[0] += v * (t - s) / 1000.0
                    if a[1] is None:
                        a[1] = s - lo
                    a[2] = t - 1 - lo
                    if a[4] is not None:
                        for k in range(s - lo, t - lo):
                            a[4][k] += v / 1000.0
                else:
                    a[3] += v * (t - s)
    scan(exe, 0)
    if calls:
        scan(cal, 1)
    return out, n_int


def pe_files(d):
    fs = glob.glob(os.path.join(d, "*.sumd"))
    key = lambda p: int(re.search(r"\.(\d+)\.sumd$", p).group(1))
    return sorted(((key(p), p) for p in fs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["totals", "straggler", "fanout", "first", "profile"])
    ap.add_argument("--dir", default=".")
    ap.add_argument("--ep", action="append", required=True,
                    help="regex over .sts entry names; repeatable")
    ap.add_argument("--ppn", type=int, default=14)
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--bin", type=int, default=100, help="profile bin, ms")
    args = ap.parse_args()

    sts = find_sts(args.dir)
    groups = {}   # pattern -> {id: name}
    for pat in args.ep:
        ids = ep_ids(sts, pat)
        if not ids:
            sys.exit(f"no EP matches '{pat}' in {sts}")
        groups[pat] = ids
        print(f"# '{pat}' -> " + ", ".join(f"{i}:{n.split('(')[0]}"
                                           for i, n in sorted(ids.items())))
    want = sorted({i for g in groups.values() for i in g})
    need_calls = args.cmd == "totals"
    need_series = args.cmd == "profile"

    per_pe = {}
    for pe, path in pe_files(args.dir):
        per_pe[pe], n_int = parse_pe(path, want, need_calls, need_series)
    npe = max(per_pe) + 1

    def group_val(pe, pat, idx):
        return sum(per_pe[pe][e][idx] for e in groups[pat])

    if args.cmd == "totals":
        print(f"{'pattern':30s} {'busy_s':>10s} {'calls':>8s} {'ms/call':>8s} {'PEs>1ms':>8s}")
        for pat in groups:
            t = sum(group_val(pe, pat, 0) for pe in per_pe)
            c = sum(group_val(pe, pat, 3) for pe in per_pe)
            n = sum(1 for pe in per_pe if group_val(pe, pat, 0) > 1)
            print(f"{pat:30s} {t/1000:10.2f} {c:8d} "
                  f"{(t/c if c else 0):8.1f} {n:8d}")

    elif args.cmd == "straggler":
        for pat in groups:
            rows = sorted(((group_val(pe, pat, 0), pe) for pe in per_pe), reverse=True)
            print(f"\n== {pat}: top {args.top} PEs (ms, pe, proc, last-active-ms)")
            for ms, pe in rows[:args.top]:
                last = max((per_pe[pe][e][2] for e in groups[pat]), default=-1)
                print(f"  pe {pe:5d} proc {pe//args.ppn:4d} {ms:9.0f} ms  last {last}")
            proc = defaultdict(float)
            for ms, pe in rows:
                proc[pe // args.ppn] += ms
            ps = sorted(proc.items(), key=lambda kv: -kv[1])
            med = sorted(proc.values())[len(proc)//2]
            print(f"  top processes: " + ", ".join(f"{p}:{v/1000:.2f}s" for p, v in ps[:6]))
            print(f"  median process {med/1000:.2f}s; top/median "
                  f"{ps[0][1]/med if med else 0:.1f}x")

    elif args.cmd == "fanout":
        for pat in groups:
            print(f"\n== {pat}: per-process spread (ppn {args.ppn})")
            acts, shares = [], []
            for pr in range(0, (npe + args.ppn - 1)//args.ppn):
                vals = [group_val(pe, pat, 0)
                        for pe in range(pr*args.ppn, min((pr+1)*args.ppn, npe))
                        if pe in per_pe]
                s = sum(vals)
                if s > 1:
                    acts.append(sum(1 for v in vals if v > 1))
                    shares.append(max(vals)/s)
            if not acts:
                print("  (no process has activity)")
                continue
            print(f"  active processes {len(acts)}; active PEs/process mean "
                  f"{sum(acts)/len(acts):.1f} min {min(acts)} max {max(acts)}; "
                  f"top-PE share mean {sum(shares)/len(shares):.2f} "
                  f"(1.0 = serialized on one PE, 1/ppn = perfect spread)")

    elif args.cmd == "first":
        for pat in groups:
            rows = [(min((per_pe[pe][e][1] for e in groups[pat]
                          if per_pe[pe][e][1] is not None), default=None), pe)
                    for pe in per_pe]
            rows = [(f, pe) for f, pe in rows if f is not None]
            rows.sort()
            print(f"\n== {pat}: first activity (ms since trace start)")
            print(f"  earliest {rows[0][0]} (pe {rows[0][1]}), "
                  f"median {rows[len(rows)//2][0]}, latest {rows[-1][0]} "
                  f"(pe {rows[-1][1]}); active PEs {len(rows)}")

    elif args.cmd == "profile":
        b = args.bin
        for pat in groups:
            series = [0.0] * n_int
            for pe in per_pe:
                for e in groups[pat]:
                    sv = per_pe[pe][e][4]
                    if sv:
                        # PEs can record more intervals than the first file
                        # promised (they stop tracing at slightly different
                        # times); grow rather than crash.
                        if len(sv) > len(series):
                            series.extend([0.0] * (len(sv) - len(series)))
                        for k, v in enumerate(sv):
                            series[k] += v
            print(f"\n== {pat}: busy-PE count per {b} ms bin (sum busy_ms/bin/{b})")
            for k in range(0, n_int, b):
                v = sum(series[k:k+b]) / b
                if v >= 0.5:
                    print(f"  {k:6d} ms  {v:8.1f}  {'#' * min(80, int(v))}")

if __name__ == "__main__":
    main()
