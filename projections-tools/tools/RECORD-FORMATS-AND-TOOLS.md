# Tools from the stall campaign

Twelve tools, kept because they are not specific to the application that
produced them. Everything here is Python 3 with no third-party dependencies, or
one C file. Paths to traces are positional arguments.

## The Projections layer

**`relay18_state.py`** — THE ONE CORRECT PER-PE STATE MACHINE. Start here;
import it rather than writing another. It encodes two mistakes that each cost a
false result (see SUMMARY.md, trap 1): records must be read in FILE ORDER
because `BEGIN_IDLE`/`END_IDLE` share a microsecond, and pack/unpack nest
inside an entry method rather than being state transitions. Yields maximal
busy/idle/overhead intervals. Every other script here builds on it.

**`relay18-band.py`** — counts intervals in a duration band across all PEs, and
EXCLUDES the start-of-trace artifact (trap 2). If you write your own band
count, copy the `open_entry == -1` exclusion from here.

**`relay32-dump.py`** — human-readable dump of a time window across all PEs,
every record type named, every entry method resolved from the `.sts`. The first
thing to run when you do not yet know what a window contains.

**`relay38-pe.py`** — the same for ONE PE, unfiltered, including idle records
and user events. What you use when a single PE is behaving differently from its
neighbours.

**`relay40-wakeups.py`** — bins idle-exit records to reveal protocols that emit
NO trace events at all. This is how quiescence detection was found: its
messages are Converse handlers and Projections never sees them, but every
message that lands makes a PE leave the idle loop. Generalises to anything
built on `CcdRaiseCondition`.

**`relay45-ramp.py`** — measures how a broadcast actually spreads, using the
per-process re-multicast records rather than trying to match executions by
(source, event id), which COLLIDES. Revealed that a Charm broadcast is N
point-to-point sends in process-id order, ~2.3 µs apart, not a tree.

**`relay46-schedgap.py`** — census of user events and bracketed user-event
pairs (record type 100) in a run. Reads what `charm-instrumentation.diff`
writes.

**`relay39-qd.py`** — every quiescence-detection episode in a run: post, fire,
drain, settle, and machine-wide busy time inside it. Needs no instrumentation —
it works off the `waitQD`/`onQD` entry methods, which Projections does record.

## Below Projections

**`monitor-threads.py`** — samples every thread of every local rank: name, CPU,
affinity mask, and the kernel's CUMULATIVE `/proc/<tid>/schedstat` counters.
The `run_delay` column is the one that matters — time spent on the runqueue
waiting for a CPU — because it cannot be missed by sampling, which is the
objection every `ps`-based approach deserves. Run it in the background from the
batch script; the batch script shares a node with application ranks.
KNOWN LIMIT: it caches `Cpus_allowed_list` at first sight, so a thread first
seen before the runtime applies its pinning shows the pre-pinning mask. `psr`
is sampled every time and is the ground truth for placement.

**`relay43-threadmap.py`** — reads that output and finds CPUs hosting more than
one thread. This is what showed 8 of 8 doubled cores matching 8 of 8 victim PEs
from the trace.

**`interpose-pthread.c`** — an `LD_PRELOAD` shim on `pthread_create` that logs,
for every call, the `.so` owning the start routine, the caller's backtrace
resolved frame by frame with `dladdr`, and the CREATING thread's affinity mask.
Named the culprit library and the exact application call site in one run.

    gcc -shared -fPIC -o libpcreate.so interpose-pthread.c -ldl
    PTHREAD_TRACE_DIR=<dir> LD_PRELOAD=<path>/libpcreate.so <binary> ...

**`relay44-pcreate.py`** — reads those logs, groups by owning library, and
singles out creations whose creator was PINNED to one CPU. That last column is
the whole diagnosis.

## Record layouts, verified against the binaries

    1   CREATION           mIdx eIdx time event pe msglen recvtime
    2   BEGIN_PROCESSING   mIdx eIdx time event SRCpe msglen recvtime id0..3 cputime
    3   END_PROCESSING     mIdx eIdx time event pe ...
    13  USER_EVENT         eventid time seq pe
    14/15  BEGIN/END_IDLE  type time pe
    16..19 BEGIN/END_PACK, BEGIN/END_UNPACK
    20  CREATION_MULTICAST mIdx eIdx time event pe msglen ? nPEs
    100 USER_EVENT_PAIR    eventid time seq ...   (two records per bracket,
                                                   matched by (eventid, seq))

Cross-PE timestamp comparison relies on Charm's clock synchronisation: safe at
the millisecond scale, not at the microsecond scale.
