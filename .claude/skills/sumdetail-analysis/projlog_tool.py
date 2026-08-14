#!/usr/bin/env python3
"""
Read Charm++ Projections FULL event traces (.log.gz + .sts) headlessly.

Companion to .claude/skills/sumdetail-analysis/sumd_tool.py, which reads
.sumd summary traces and explicitly does NOT read .log.gz. This one exists
for the question sum-detail cannot answer: per-CALL duration and MESSAGE
LENGTH, so an entry method's cost can be regressed against bytes.

Format (verified empirically against Projections VERSION 11.0 traces from
FoF3 on Frontier, 2026-08-13):

  .sts   ENTRY CHARE <id> "<name>" ...        # id -> name, PER BINARY
  .log.gz  line 1: PROJECTIONS-RECORD <n>
           BEGIN_PROCESSING: 2 mtype entry time event pe msglen recvTime [idx..]
           END_PROCESSING:   3 mtype entry time event pe msglen time2
           (times are microseconds; entry ids differ between binaries --
            always resolve through the .sts of THAT run)

Entry methods do not nest on a PE, so BEGIN/END pair by simple stack per file.

  totals   --ep RE           busy s, calls, ms/call, bytes/call  (like sumd totals)
  calls    --ep RE           per-call duration + msglen distribution (percentiles)
  regress  --ep RE           duration vs msglen: slope, r, and binned means
                             -- THE bytes-vs-nodes discriminator across two runs
  entries                    list entries by call count

Usage:
  python3 projlog_tool.py totals  --dir DIR --ep 's3Shipment'
  python3 projlog_tool.py regress --dir DIR --ep 's3Shipment'
"""
import argparse, glob, gzip, os, re, sys


def load_sts(d):
    stss = glob.glob(os.path.join(d, "*.sts"))
    if not stss:
        sys.exit("no .sts in %s (entry ids cannot be resolved without it)" % d)
    names = {}
    pat = re.compile(r'^ENTRY\s+\w+\s+(\d+)\s+"(.*?)"')
    with open(stss[0]) as f:
        for line in f:
            m = pat.match(line)
            if m:
                names[int(m.group(1))] = m.group(2)
    return names, stss[0]


def select(names, regexes):
    """regex -> set of entry ids, case-insensitive, matched against names."""
    out = {}
    for rx in regexes:
        c = re.compile(rx, re.I)
        ids = {i for i, n in names.items() if c.search(n)}
        if not ids:
            print("# WARNING '%s' matched no entry in the .sts" % rx)
        else:
            for i in sorted(ids):
                print("# '%s' -> %d:%s" % (rx, i, names[i]))
        out[rx] = ids
    return out


PE_RE = re.compile(r"\.(\d+)\.log\.gz$")


def file_pe(path):
    """Executing PE is the FILE, not a record field."""
    m = PE_RE.search(path)
    return int(m.group(1)) if m else -1


def walk(d, want_ids):
    """Yield (entry, pe, start_us, dur_us, msglen, src_pe, event) per call.

    NOTE on the format: on a BEGIN_PROCESSING record, field 6 is the SOURCE
    pe (who created the message) -- the EXECUTING pe is the file the record
    lives in. Getting this backwards inflates any "how many PEs" count into
    "how many senders". want_ids=None collects everything (for `entries`)."""
    for path in sorted(glob.glob(os.path.join(d, "*.log.gz"))):
        pe = file_pe(path)
        open_begin = {}          # entry -> (time, src, msglen, event)
        try:
            with gzip.open(path, "rt") as f:
                for line in f:
                    if not (line[0] == "2" or line[0] == "3") or line[1] != " ":
                        continue
                    p = line.split()
                    try:
                        entry = int(p[2]); t = int(p[3])
                        ev = int(p[4]); src = int(p[5])
                    except (IndexError, ValueError):
                        continue
                    if want_ids is not None and entry not in want_ids:
                        continue
                    if p[0] == "2":
                        try:
                            msglen = int(p[6])
                        except (IndexError, ValueError):
                            msglen = -1
                        open_begin[entry] = (t, src, msglen, ev)
                    else:
                        b = open_begin.pop(entry, None)
                        if b is not None and t >= b[0]:
                            yield entry, pe, b[0], t - b[0], b[2], b[1], b[3]
        except (OSError, EOFError) as e:
            print("# WARNING unreadable %s: %s" % (os.path.basename(path), e))


def walk_creations(d, want_ids):
    """Yield (entry, event, src_pe, time, msglen) from CREATION records (type 1).

    Pairs with walk()'s (entry, event) to time a message in flight."""
    for path in sorted(glob.glob(os.path.join(d, "*.log.gz"))):
        try:
            with gzip.open(path, "rt") as f:
                for line in f:
                    if line[0] != "1" or line[1] != " ":
                        continue
                    p = line.split()
                    try:
                        entry = int(p[2]); t = int(p[3])
                        ev = int(p[4]); src = int(p[5]); ml = int(p[6])
                    except (IndexError, ValueError):
                        continue
                    if want_ids is not None and entry not in want_ids:
                        continue
                    yield entry, ev, src, t, ml
        except (OSError, EOFError):
            pass


def pct(sorted_vals, q):
    if not sorted_vals:
        return 0
    k = min(len(sorted_vals) - 1, max(0, int(round(q * (len(sorted_vals) - 1)))))
    return sorted_vals[k]


def cmd_entries(d, names, _):
    counts, dur = {}, {}
    for entry, pe, st, du, ml, src, ev in walk(d, None):
        counts[entry] = counts.get(entry, 0) + 1
        dur[entry] = dur.get(entry, 0) + du
    print("%9s %12s %9s  %s" % ("calls", "busy_s", "ms/call", "entry"))
    for e in sorted(counts, key=lambda k: -dur.get(k, 0))[:30]:
        print("%9d %12.2f %9.3f  %d:%s"
              % (counts[e], dur[e] / 1e6, dur[e] / 1e3 / counts[e], e,
                 names.get(e, "<unknown>")))


def collect(d, sel):
    """regex -> list of (dur_us, msglen, exec_pe, start_us, src_pe, event)"""
    allids = set().union(*sel.values()) if sel else set()
    per = {rx: [] for rx in sel}
    for entry, pe, st, du, ml, src, ev in walk(d, allids):
        for rx, ids in sel.items():
            if entry in ids:
                per[rx].append((du, ml, pe, st, src, ev))
    return per


def cmd_transit(d, names, sel):
    """Time a message in flight: CREATION (sender) -> BEGIN_PROCESSING (receiver).

    With CMK_USE_SHMEM undefined, even same-physical-node traffic goes over the
    fabric, so this is what a 40 MB intra-node shipment actually costs to move.
    Both endpoints must be inside the traced PE subset to pair."""
    allids = set().union(*sel.values()) if sel else set()
    created = {}
    for entry, ev, src, t, ml in walk_creations(d, allids):
        created[(entry, ev)] = (t, src, ml)
    for rx, rows in collect(d, sel).items():
        ids = sel[rx]
        matched, unmatched = [], 0
        for du, ml, pe, st, src, ev in rows:
            hit = None
            for i in ids:
                hit = created.get((i, ev))
                if hit:
                    break
            if hit and st >= hit[0]:
                matched.append((st - hit[0], ml, hit[1], pe))
            else:
                unmatched += 1
        if not matched:
            print("\n== %s: no CREATION/BEGIN pairs (sender outside traced subset?) "
                  "unmatched=%d" % (rx, unmatched))
            continue
        ts = sorted(m[0] for m in matched)
        same = [m for m in matched if m[2] // 14 // 8 == m[3] // 14 // 8]
        print("\n== %s: %d matched pairs (%d unmatched)" % (rx, len(matched), unmatched))
        print("   in-flight ms  min %.2f  p50 %.2f  p90 %.2f  max %.2f"
              % (ts[0]/1e3, pct(ts, .5)/1e3, pct(ts, .9)/1e3, ts[-1]/1e3))
        print("   same physical node (block of 8 procs): %d of %d"
              % (len(same), len(matched)))
        if matched:
            bw = [(m[1] / (m[0] / 1e6) / 1e6) for m in matched if m[0] > 0]
            if bw:
                bw.sort()
                print("   implied MB/s  min %.0f  p50 %.0f  max %.0f"
                      % (bw[0], pct(bw, .5), bw[-1]))


def cmd_totals(d, names, sel):
    print("%-34s %9s %8s %9s %11s %10s" %
          ("pattern", "busy_s", "calls", "ms/call", "bytes/call", "PEs"))
    for rx, rows in collect(d, sel).items():
        if not rows:
            print("%-34s %9s" % (rx[:34], "-- none --")); continue
        n = len(rows); tot = sum(r[0] for r in rows)
        mls = [r[1] for r in rows if r[1] >= 0]
        print("%-34s %9.2f %8d %9.1f %11.0f %10d"
              % (rx[:34], tot / 1e6, n, tot / 1e3 / n,
                 (sum(mls) / len(mls)) if mls else -1,
                 len({r[2] for r in rows})))


def cmd_calls(d, names, sel):
    for rx, rows in collect(d, sel).items():
        if not rows:
            print("%s: none" % rx); continue
        ds = sorted(r[0] for r in rows)
        ms = sorted(r[1] for r in rows if r[1] >= 0)
        n = len(ds)
        print("\n== %s: %d calls on %d PEs" % (rx, n, len({r[2] for r in rows})))
        print("   duration ms  min %.3f  p50 %.3f  p90 %.3f  p99 %.3f  max %.3f  total %.2f s"
              % (ds[0]/1e3, pct(ds,.5)/1e3, pct(ds,.9)/1e3, pct(ds,.99)/1e3,
                 ds[-1]/1e3, sum(ds)/1e6))
        if ms:
            print("   msglen  KB  min %.1f  p50 %.1f  p90 %.1f  max %.1f  mean %.1f"
                  % (ms[0]/1024, pct(ms,.5)/1024, pct(ms,.9)/1024,
                     ms[-1]/1024, sum(ms)/len(ms)/1024))


def cmd_regress(d, names, sel):
    """Duration vs message length.

    Across two runs this is the bytes-vs-nodes discriminator: if the cost is
    BYTES, both runs share a slope (us per byte) and the cheaper wire format
    merely sits at lower x. If the cost is NODES, the run with fewer bytes per
    node shows a STEEPER us/byte slope for the same wall time."""
    for rx, rows in collect(d, sel).items():
        rows = [r for r in rows if r[1] > 0]
        if len(rows) < 3:
            print("%s: too few calls with msglen (%d)" % (rx, len(rows))); continue
        xs = [r[1] for r in rows]; ys = [r[0] for r in rows]
        n = len(xs); mx = sum(xs)/n; my = sum(ys)/n
        sxy = sum((x-mx)*(y-my) for x, y in zip(xs, ys))
        sxx = sum((x-mx)**2 for x in xs); syy = sum((y-my)**2 for y in ys)
        slope = sxy/sxx if sxx else 0
        r = sxy/((sxx*syy)**.5) if sxx and syy else 0
        print("\n== %s: %d calls" % (rx, n))
        print("   mean %.1f KB -> %.1f ms" % (mx/1024, my/1e3))
        print("   slope %.4f us/byte  (= %.3f ms per KB)" % (slope, slope*1024/1e3))
        print("   intercept %.3f ms   pearson r %.3f" % ((my-slope*mx)/1e3, r))
        # binned means: shows curvature a single slope would hide
        lo, hi = min(xs), max(xs)
        if hi > lo:
            NB = 6; w = (hi-lo)/NB
            print("   %-18s %7s %10s %10s" % ("msglen bin (KB)", "calls", "mean ms", "us/byte"))
            for b in range(NB):
                a, z = lo+b*w, lo+(b+1)*w
                sel_r = [(x, y) for x, y in zip(xs, ys) if (a <= x < z or (b == NB-1 and x == z))]
                if not sel_r: continue
                bx = sum(p[0] for p in sel_r)/len(sel_r)
                by = sum(p[1] for p in sel_r)/len(sel_r)
                print("   %7.1f-%-9.1f %7d %10.3f %10.4f"
                      % (a/1024, z/1024, len(sel_r), by/1e3, by/bx))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["totals", "calls", "regress", "entries", "transit"])
    ap.add_argument("--dir", default=".")
    ap.add_argument("--ep", action="append", default=[],
                    help="regex over .sts entry names; repeatable")
    a = ap.parse_args()
    names, sts = load_sts(a.dir)
    print("# sts %s (%d entries)" % (os.path.basename(sts), len(names)))
    if a.cmd == "entries":
        cmd_entries(a.dir, names, None); return
    if not a.ep:
        sys.exit("--ep is required for %s" % a.cmd)
    sel = select(names, a.ep)
    {"totals": cmd_totals, "calls": cmd_calls, "regress": cmd_regress,
     "transit": cmd_transit}[a.cmd](a.dir, names, sel)


if __name__ == "__main__":
    main()
