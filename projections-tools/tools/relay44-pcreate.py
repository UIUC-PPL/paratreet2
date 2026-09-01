#!/usr/bin/env python3
"""relay44: read the pthread_create interposer logs and name the owner.

Input: a directory of pcreate.<pid>.txt files written by
tools/interpose-pthread.c.  Each record names the .so that owns the START
ROUTINE, the creating thread's affinity, and the caller's backtrace.

The record that matters is one whose CREATOR WAS PINNED to a single CPU: that
is a helper inheriting a Charm PE's affinity mask, which is the whole bug.
"""
import glob, os, re, sys, collections

def records(d):
    for f in sorted(glob.glob(os.path.join(d, "pcreate.*.txt"))):
        txt = open(f, errors="replace").read()
        for b in txt.split("\n[")[1:]:
            m = re.search(r"START_ROUTINE in (\S+)", b)
            if m:
                yield f, m.group(1), ("PINNED to cpu" in b), "[" + b

def main(d, label):
    if not os.path.isdir(d):
        print("  %s: no logs" % label); return
    n = len(glob.glob(os.path.join(d, "pcreate.*.txt")))
    all_ = collections.Counter()
    pinned = collections.Counter()
    example = {}
    for f, lib, isp, body in records(d):
        all_[lib] += 1
        if isp:
            pinned[lib] += 1
            example.setdefault(lib, body)
    print("  --- %s : %d process logs, %d creations ---"
          % (label, n, sum(all_.values())))
    print("  creations by the library owning the START ROUTINE:")
    for lib, c in all_.most_common():
        print("     %6d  %s" % (c, lib))
    print("  of those, creations whose CREATOR WAS PINNED to one cpu:")
    if not pinned:
        print("     none")
    for lib, c in pinned.most_common():
        print("     %6d  %s   <-- inherits a PE's mask" % (c, lib))
    for lib in list(pinned)[:4]:
        print("  ==== full record, %s ====" % lib)
        for line in example[lib].splitlines()[:18]:
            print("     " + line)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
