# Notes for a future session: collectives for the canopy exchange

2026-08-23. Kale asked to defer this to a fresh session ("designing allgather
for Charm++"). These are the notes to start from — the measurements are done,
the problem is characterized, the design is not.

## The motivating measurement (relay92, jobs 5329100 / 5329432 / 5329722)

`loadCache` is the reason paratreet2 stops scaling. 64 -> 128 nodes buys
NOTHING (1729 -> 1771 ms, 0.98x) after 8->16 gave 1.95x and 16->64 gave 2.66x,
and loadCache alone accounts for the stall: it adds 0.256 s between those two
points, and without that increase 128 nodes would land near 1515 ms.

Decomposed at 128 nodes / 1024 processes:

    pack build (sort)      0.0095 s    2.7%
    per-process install    0.0668 s   18.7%  (worst process; mean 0.0443)
    ship + barrier         0.2807 s   78.6%  <- the residual

Scaling of each term over a 16x range of P (8/16/64/128 nodes):

    pack_n   P^0.97      install  P^1.28      RESIDUAL  P^1.89

The residual is the ship. The model `cost ~ P x pack_n x 128 B / B` has TWO
factors of P (P destinations, and pack_n itself proportional to P), and it is
NOT fitted: residual/P/pack_bytes comes out 13-18 GB/s across the whole range,
and the model predicts 285.4 ms at 128 nodes against 280.7 ms measured. A
barrier would be P^1.0 or log P and does not fit.

## The two collectives hiding in this, and neither is one today

**(a) The collect is a hand-rolled gather onto a singleton.** `TreeCanopy.h:66`
has every canopy element send its own point-to-point `recvTC` to `Driver`, a
SINGLETON chare, which appends to a vector. At 128 nodes that is 69,670
individual entry invocations serialized on one PE (34,835 canopies x 2 rounds).
Estimated ~0.14 s of driver-PE time at ~2 us per invocation — the same order as
loadCache itself, and invisible in every phase row we print. NOT yet confirmed
by a trace; it should appear as one hot PE at 128 nodes.

*The fix is half-built already*: the line right after that `recvTC` is
`thisProxy[thisIndex / branch_factor].recvData(...)` — the canopy tree is
ALREADY aggregating upward in parallel while every node also messages the
driver separately. A concat reduction over the TreeCanopy array (or simply
taking what the canopy root already holds) replaces 69,670 serialized messages
with one aggregation, and delivers the pack pre-deduped — which also removes
the sort (2.7%) and the 2x duplication.

**(b) The ship is a serial ramp, not a tree.** The origin sends one message per
destination. This matches the campaign's earlier finding (relay45 machinery:
"how a broadcast actually spreads") that the runtime broadcast ramps from one
origin rather than fanning out as a spanning tree. Capping the payload (see
`-s` below) removes ONE factor of P and leaves O(P): ~285 ms uncapped at 128
nodes, ~20 ms capped, but still ~157 ms at 1024 nodes. **The knob is the cheap
fix; a real spanning-tree broadcast is the actual one.**

## What it actually is: an allgather

Every process contributes its own canopies and every process needs the union.
That is an allgather, for which the standard algorithms (recursive doubling,
ring, Bruck) are O(log P) messages or O(P) with full bandwidth utilization —
against the current O(P) messages from ONE origin plus O(P) gather messages
INTO one chare. Charm++ exposes reductions and broadcasts but no allgather
primitive, so this would be built from sections/nodegroups or from the tree
that already exists in TreeCanopy.

## Questions for the design session

1. Is Charm's broadcast to a nodegroup genuinely a serial ramp on this runtime
   (classic netlrts vs reconverse vs the Frontier build), or is our evidence
   specific to one path? Measure before designing.
2. Can the TreeCanopy tree serve as the allgather network in both directions —
   aggregate up, then push down — avoiding a second structure entirely?
3. What is the right payload cap policy (`-s`), and should it be a depth cut
   rather than a count cut? Keys are prefix-coded so a count cut on sorted keys
   is already approximately a depth cut, but a depth cut is what one would want
   to state in a paper.
4. Does the closing reduction/barrier of `recvStarterPack` matter separately
   from the ship, once the ship is fixed?
5. Is this worth upstreaming to Charm++ as a general collective, or does it
   stay a paratreet2-level structure? (Kale's call; relevant to the
   charm-notes/reconverse work.)

## The knob that exists today, unset since forever

    Driver.h:476-481   if (num_share_nodes > 0 && num_share_nodes < send_size)
                         send_size = num_share_nodes;
                       else CkPrintf("Broadcasting every tree canopy because
                                      num_share_nodes is unset")
    Configuration.h:170  register_field("nShareNodes", "s", num_share_nodes)
    Main.C:49            conf.num_share_nodes = 0;   // = unset = ship everything

Every run in this campaign printed that line and nobody read it.

LOCAL VERIFICATION (2026-08-23, 1m/4 procs, laptop): `-s` engages below the
DEDUPED canopy count (62 raw / 31 deduped here — confirming raw is exactly 2x
deduped), and **every cap is EXACT, including `-s 1`**: components 333889 /
max_size 259128 identical to the uncapped run. So correctness does not depend
on the starter pack; a process that lacks a canopy entry fetches it during the
walk. The trade is purely ship cost against walk fetches, which is what the
knob is for, and both sides are now instrumented.

Because keys are prefix-coded (root = 1, deeper nodes have larger keys) and
`sortStorage()` sorts by key, capping ships the SHALLOWEST N canopy nodes —
the top of the tree, which is the right prefix. That is not documented
anywhere and should be, since it is what makes the knob usable as-is.
