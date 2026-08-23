# uploads/ — 2026-08-23 03:05

**START HERE: `relay92.txt`** (305 lines). All three jobs are now in — 5329100
and 5329432 at 128 nodes, and 5329722 with the 8/16/64 points. Every arm EXACT.
Nothing is outstanding.

## The gating answer: time still binds at 128 nodes

    -D 2   RSS 4113.5 MB   cached_particle 10896 MB
    -D 3   RSS 4180.3 MB   cached_particle 11346 MB

The cache saving survives at 63.6 MB/process but that is **1.6% of a 4.2 GB
resident set**, and the particle volume that differed 2.3× at 16 nodes differs
by **4%** here — at 1024 processes the canopy dominates what gets cached. So
**D1+aggregation is not indicated and I have not built it.**

## THE HEADLINE: loadCache is O(P²) in the SHIP, and the fix is an unset knob

    nodes  procs   pack_n     MB    sort  inst_max   total  residual   res%
        8     64    2,393   0.31  0.0006    0.0019   0.004    0.0015  37.5%
       16    128    4,629   0.59  0.0013    0.0056   0.011    0.0041  37.3%
       64    512   17,694   2.26  0.0045    0.0305   0.101    0.0660  65.3%
      128   1024   34,835   4.46  0.0095    0.0668   0.357    0.2807  78.6%

    over 16x processes:  pack_n P^0.97   install P^1.28   RESIDUAL P^1.89

**The residual is the ship, and the exponent proves it.** The payload grows
linearly with P (canopies are per tree piece) and goes to P processes, so bytes
out of the origin go as P × pack_n ~ P^1.97 — measured P^1.89. A barrier would
be P^1.0 or log P and does not fit. Consistent with the recorded finding that
the runtime broadcast is a serial ramp from one origin, not a tree.

**And the knob already exists, unset since forever:**

    Driver.h:479  if (num_share_nodes > 0 && num_share_nodes < send_size) ...
                  else CkPrintf("Broadcasting every tree canopy because
                                 num_share_nodes is unset")
    Configuration.h:170  register_field("nShareNodes", "s", num_share_nodes)
    Main.C:49            conf.num_share_nodes = 0;

Every run in this campaign printed that line and nobody read it. `-s N` caps the
pack, removing one factor of P and turning the ship from O(P²) into O(P).

**This corrects my own §5.** I said a bounded canopy "would address 18.7% at
best" because it only touches the install. Wrong — the dominant term is
P × pack_n, so bounding the canopy attacks the 78.6%, not the 18.7%.

Not free, which is what the knob is for: a process without the whole canopy
fetches more during the walk. The experiment is a `-s` sweep against walk time,
and both sides are now instrumented.

## And scaling has already stopped — 128 nodes is not faster than 64

     8 ->  16 (2x):  8977 -> 4598 ms  = 1.95x   near-perfect
    16 ->  64 (4x):  4598 -> 1729 ms  = 2.66x   fair
    64 -> 128 (2x):  1729 -> 1771 ms  = 0.98x   NOTHING

**loadCache alone accounts for it** — it adds 0.256 s between those points, and
without that increase 128 nodes would land near 1515 ms against 64's 1729. So
loadCache is not merely a growing term; it is what stalled the curve.

## Two clarifications (§11)

**How capping gives O(P).** The residual behaves exactly like "the origin ships
the whole pack once per destination". The test: if so, residual/P/pack_bytes is
a *bandwidth* — a machine constant, not a fitted parameter.

    nodes     P     pack_B  residual   per-dest   implied B/W
        8    64     310000    0.0015     23.4us      13.2 GB/s
       16   128     590000    0.0041     32.0us      18.4 GB/s
       64   512    2260000    0.0660    128.9us      17.5 GB/s
      128  1024    4460000    0.2807    274.1us      16.3 GB/s

13–18 GB/s across a 16× range of P and a 14× range of payload. The model is not
fitted — it predicts 285.4 ms at 128 nodes against 280.7 measured.

So cost ≈ P × pack_n × 128 B / B, with **two** factors of P: P destinations, and
pack_n itself ∝ P. Capping pack_n removes the second and leaves **O(P), not
O(1) and not O(log P)** — the origin still sends one message per destination.
At 128 nodes: 4.46 MB → 285 ms uncapped, 0.31 MB → 20 ms capped. A 14× cut that
buys three or four more doublings before the remaining O(P) blocks again
(~157 ms at 1024 nodes). Removing the last factor needs a spanning-tree
broadcast — which is precisely what "broadcast is a serial ramp, not a tree"
says we do not have. **The knob is the cheap fix; the tree is the real one.**

**Is the canopy collected by a concat reduction? No — there is no reduction at
all.** `TreeCanopy.h:66` has every canopy element send its own point-to-point
`recvTC` to `Driver`, a *singleton chare*, which appends to a vector. At 128
nodes that is **69,670 individual entry invocations on one PE** — 34,835
canopies × 2 rounds (build + upwardPass, which is why raw is exactly twice
deduped).

So the driver is serialised **twice** and I measured only one: collect (69,670
messages in, hidden inside phase1) and ship (P sends out, 78.6% of loadCache).
Prediction, not yet run: at ~2 µs per invocation the collect side is ~0.14 s of
driver-PE time at 128 nodes — same order as loadCache, invisible in the phase
rows, and it should show as one hot PE in a 128-node trace.

**The fix is half-built already.** The line right after that `recvTC` is
`thisProxy[thisIndex / branch_factor].recvData(...)` — the canopy tree is
*already* aggregating upward in parallel while every node also messages the
driver separately. A concat reduction over the TreeCanopy array, or just taking
what the canopy root already has, replaces 69,670 serialised messages with one
aggregation — and delivers the pack pre-deduped, removing the sort and the 2×
duplication too.

## The loadCache prediction is refuted — the collective, not the install

    pack build (sort)      0.0095 s    2.7%
    per-process install    0.0668 s   18.7%   (worst process; mean 0.0443)
    ship + barrier         0.2807 s   78.6%   <- RESIDUAL

I recorded the prediction so it could fail: install tracks P, residual does not
dominate. **It dominates at 78.6%.** The pack is only 34,835 canopies × 128 B =
**4.46 MB**, so a bounded or lazily-installed canopy — the remedy the install
hypothesis pointed at — would reach 18.7% of loadCache at best. Your other
candidate, the collective barrier, is where the time is.

Bandwidth is not it either: 4.46 MB to 1024 processes is 4.6 GB total, 36 MB
per node, against 1.9 GB/s per node. It is broadcast latency or the closing
reduction over 1024 processes.

## Placeholders, measured

    arm   cached_nodes     used_nodes        placeholders   ratio
    D2      95,980,846     813,982,806        718,001,960    7.5x
    D3     131,004,902   1,074,613,934        943,609,032    7.2x

**7.2 placeholders per cached node — 88% of the pool by count and, at ~248 B
per slot, about 88% by bytes.** `pool_MB 266,618` at −D 3 is mostly empty
frontier markers, not data. That is the quantity relay91 §3 wanted and could
not measure.

## Two corrections to my own earlier claims

**The walk is not "flat".** You were right that its work grows with the
decomposition. Edges emitted go 1,234,781 → 2,821,596, ×2.285 for 8× processes
(P^0.40), so the yardstick is 3.5× not 8×: ideal 0.127 s, achieved 0.435 s,
**29.3% efficiency**, not the 12.8% "flat vs 8×" implied. Still a real 3.4×
problem and still the largest term at 128 nodes (25%), but I overstated it by
2.3× and have corrected §2.

**The two 128-node jobs disagree on D3.** D2 spans 39 ms over four reps; D3
spans 275 ms and the jobs order them oppositely. So D2 is *reproducible* and D3
is *erratic* at this scale — a more useful statement than "tied", and one job
could not have shown it.

## A job that failed on a bug of mine

5329433 died in 6 s with no log: `local N=$1 T=$((N*8))` expands the arithmetic
before `N` is assigned, so under `set -u` it is a fatal unbound-variable error
that exits the shell, which `|| true` cannot catch. Reproduced in two lines
before fixing. Resubmitted as **5329722**; I had also hidden the warm-up's
output behind `/dev/null`, which is what concealed it, and that is removed.

With §5 in hand the question 5329722 answers has changed: it is now *how does
the ship+barrier residual grow with P*, not *does the install track P*.

## Files

    relay92.txt                        the above in full
    relay92-paratreet2-loadcache.diff  the loadCache instrumentation (NOT pushed)
    relay92-lc128n.sbatch              the 128-node companion
    relay92-lcscale-64n.sbatch         the 8/16/64 sweep, fixed
    build-v85.sh                       the binary
