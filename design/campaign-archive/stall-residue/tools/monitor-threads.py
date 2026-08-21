#!/usr/bin/env python3
"""relay40: sample every thread of every local rank -- name, core, and the
kernel's own cumulative scheduling accounting.

WHY NOT PLAIN ps.  Kale's worry is right: an instantaneous snapshot can miss a
thread that is only occasionally runnable.  So this records CUMULATIVE
counters, which cannot be missed by sampling:

  /proc/<tid>/schedstat  ->  sum_exec_runtime_ns  run_delay_ns  pcount
      run_delay is the time this thread spent ON THE RUNQUEUE WAITING for a
      CPU.  That is the exact quantity we are chasing -- descheduled but
      runnable -- and the kernel accumulates it for us whether or not we
      sample at the right moment.
  /proc/<tid>/stat field 39 -> the CPU this thread last ran on.
  /proc/<tid>/comm         -> the thread's name, which is how we identify
      whose thread it is (ROCm, libfabric, userfaultfd, ...).

Snapshots are still taken at ~10 Hz so we can see placement stability, but the
verdict comes from the deltas of the cumulative counters between the first and
last snapshot.
"""
import os, sys, time, glob

def threads_of(pid):
    try:
        return os.listdir("/proc/%d/task" % pid)
    except OSError:
        return []

def read(p):
    try:
        with open(p) as f: return f.read()
    except OSError:
        return None

CACHE = {}

def static_info(pid, tid):
    """Read once per thread: name and CPU AFFINITY MASK.  The mask is the
    cheapest discriminator there is -- a Charm PE thread pinned by +pemap has
    a single-CPU Cpus_allowed_list, an unpinned intruder inherits the whole
    job cpuset."""
    k = (pid, tid)
    if k in CACHE: return CACHE[k]
    comm = (read("/proc/%d/task/%s/comm" % (pid, tid)) or "?").strip()
    allowed = "?"
    st = read("/proc/%d/task/%s/status" % (pid, tid))
    if st:
        for line in st.splitlines():
            if line.startswith("Cpus_allowed_list:"):
                allowed = line.split(None, 1)[1].strip(); break
    CACHE[k] = (comm, allowed)
    return CACHE[k]


def sample(pids):
    out = []
    for pid in pids:
        for tid in threads_of(pid):
            st = read("/proc/%d/task/%s/stat" % (pid, tid))
            if st is None: continue
            # comm may contain spaces and parens: split after the LAST ')'
            i = st.rfind(')')
            fields = st[i+2:].split()
            # after comm and state, field index 0 is state; processor is
            # field 39 of the whole record = index 36 here
            psr = fields[36] if len(fields) > 36 else "-1"
            comm, allowed = static_info(pid, tid)
            ss = (read("/proc/%d/task/%s/schedstat" % (pid, tid)) or "0 0 0").split()
            out.append((pid, tid, comm.replace(" ", "_"), psr, ss[0], ss[1],
                        ss[2] if len(ss) > 2 else "0",
                        allowed.replace(" ", "")))
    return out

def find_pids(pattern):
    pids = []
    for d in glob.glob("/proc/[0-9]*"):
        c = read(d + "/comm")
        if c and pattern in c.strip():
            try: pids.append(int(d.split('/')[-1]))
            except ValueError: pass
    return sorted(pids)

def main(outpath, pattern, seconds, hz=10.0):
    end = time.time() + seconds
    fh = open(outpath, "w")
    fh.write("# t_s pid tid comm psr sum_exec_ns run_delay_ns pcount cpus_allowed\n")
    fh.write("# node %s  pattern %r\n" % (os.uname().nodename, pattern))
    t0 = time.time()
    seen = False
    n = 0
    while time.time() < end:
        pids = find_pids(pattern)
        if pids:
            seen = True
            if n == 0:
                fh.write("# batch-context Cpus_allowed_list: %s\n"
                         % (dict(l.split(":",1) for l in
                            (read("/proc/self/status") or "").splitlines()
                            if l.startswith("Cpus_allowed_list"))
                            .get("Cpus_allowed_list","?").strip()))
                for pid in pids:
                    st = read("/proc/%d/status" % pid) or ""
                    al = [l for l in st.splitlines()
                          if l.startswith("Cpus_allowed_list")]
                    fh.write("# rank pid %d process-wide %s ; %d threads\n"
                             % (pid, al[0].split(None,1)[1].strip() if al else "?",
                                len(threads_of(pid))))
            for row in sample(pids):
                fh.write("%.3f %d %s %s %s %s %s %s %s\n" % ((time.time()-t0,) + row))
            n += 1
            if n % 50 == 0: fh.flush()
        elif seen:
            break              # the ranks exited; stop
        time.sleep(1.0/hz)
    fh.close()

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], float(sys.argv[3]))
