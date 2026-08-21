#!/usr/bin/env python3
"""relay18: ONE correct per-PE state machine over a Projections .log.gz.

Two mistakes cost me a false result each before this was written down, so the
model is stated explicitly:

  * BEGIN_IDLE(14)/END_IDLE(15) arrive in the SAME microsecond very often --
    reconverse's scheduler leaves the idle condition to poll and re-enters it.
    Records must be read in FILE ORDER.  Sorting by (time, kind) inverts those
    pairs and turns the genuine idle period that follows into a phantom
    overhead gap.  (State records are time-monotonic in file order; only
    CREATION records interleave, and they carry a SEND time, not a state.)

  * BEGIN/END_PACK(16/17) and BEGIN/END_UNPACK(18/19) nest INSIDE an entry
    method.  They are sub-intervals, not state transitions.  Returning to
    OVERHEAD when a pack ends reclassifies ordinary execution as overhead.

So the base state is decided by two independent flags and nothing else:
    in_entry  (BEGIN_PROCESSING .. END_PROCESSING)   -> BUSY
    in_idle   (BEGIN_IDLE       .. END_IDLE)         -> IDLE
    neither                                          -> OVERHEAD
Pack/unpack time is accumulated separately, as a sub-category of whatever the
base state is, purely for reporting.
"""
import gzip, re
BUSY, IDLE, OVER = 0, 1, 2
STATE_NAME = {BUSY: 'busy', IDLE: 'idle', OVER: 'overhead'}
PE_RE = re.compile(r"\.(\d+)\.log\.gz$")


def pe_of(path):
    return int(PE_RE.search(path).group(1))


def events(path):
    """Yield (t_us, kind, entry, src) in FILE ORDER.
    kind: 'B' begin_processing, 'E' end_processing, 'I' begin_idle,
          'J' end_idle, 'P' begin_pack/unpack, 'Q' end_pack/unpack,
          'S' begin_computation, 'T' end_computation."""
    with gzip.open(path, 'rt') as f:
        f.readline()
        for line in f:
            sp = line.split()
            ty = sp[0]
            if ty == '2':    yield int(sp[3]), 'B', int(sp[2]), int(sp[5])
            elif ty == '3':  yield int(sp[3]), 'E', int(sp[2]), -1
            elif ty == '14': yield int(sp[1]), 'I', -1, -1
            elif ty == '15': yield int(sp[1]), 'J', -1, -1
            elif ty in ('16', '18'): yield int(sp[1]), 'P', -1, -1
            elif ty in ('17', '19'): yield int(sp[1]), 'Q', -1, -1
            elif ty == '6':  yield int(sp[1]), 'S', -1, -1
            elif ty == '7':  yield int(sp[1]), 'T', -1, -1


def intervals(path):
    """Yield (t_start, t_end, state, open_entry, close_entry, close_src,
             close_kind) for every maximal base-state interval.

    open_entry  = entry id of the END_PROCESSING that opened an OVERHEAD run
                  (-2 if it opened at END_IDLE, -1 otherwise)
    close_entry = entry id of the BEGIN_PROCESSING that closed it (-1 if the
                  interval ended by going idle instead)
    """
    in_entry = in_idle = False
    last = None
    cur = OVER
    open_e = -1
    for t, k, e, s in events(path):
        if k == 'S':
            last = t
            continue
        if k in ('P', 'Q'):
            continue                      # sub-interval: never changes state
        if k == 'T':
            if last is not None:
                yield last, t, cur, open_e, -1, -1, 'T'
            return
        new_entry, new_idle = in_entry, in_idle
        if k == 'B':   new_entry = True
        elif k == 'E': new_entry = False
        elif k == 'I': new_idle = True
        elif k == 'J': new_idle = False
        ns = BUSY if new_entry else (IDLE if new_idle else OVER)
        if ns != cur:
            if last is not None:
                yield last, t, cur, open_e, (e if k == 'B' else -1), \
                      (s if k == 'B' else -1), k
            open_e = (e if k == 'E' else (-2 if k == 'J' else -1)) if ns == OVER else -1
            last, cur = t, ns
        in_entry, in_idle = new_entry, new_idle
