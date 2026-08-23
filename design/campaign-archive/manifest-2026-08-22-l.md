# uploads/ — 2026-08-22 23:05

**START HERE: `relay91.txt`.** Items 1 and 3 done from the relay90 traces, no
new job. Item 2 (−D at 128 nodes) is job **5329100, queued**.

## 1. Aggregation: the idea dies here

`requestNodes` costs **2.7 µs + 0.85 µs per node returned**. Four independent
fits across a 12× range of reply size give the same two constants:

    -D   nodes/reply   a (fixed µs)   b (µs/node)   R²      mean dur
     1      1.48          2.585          1.098     0.075      4.21
     2      4.32          3.074          0.891     0.452      6.92
     3     14.13          2.655          0.847     0.558     14.62
     4     18.44          2.872          0.839     0.894     18.34

The D=3 fit predicts D=4's mean at 18.27 µs against 18.34 measured, and D=1's
at 3.91 against 4.21. The model is right and your 14.9 µs decomposes cleanly.

At the shipping depth the fixed term is **18.2% of the call, the pack 81.8%**.
So aggregation addresses 18.2% of 18.2% of walk wall — 7.95 ms per PE, less
after a 4-item buffer keeps a quarter of it, and PEs are only 53% busy so it
does not convert 1:1 to wall. **Single-digit milliseconds on the iteration.**
Your spread numbers made it *viable*; this makes it *pointless* at −D 3.

Also: **100% of requests created a reply, at every depth.** There are no
pending-request joins to contrast — every request does a full lookup-and-pack,
so there is no de-facto coalescing today either.

**One combination worth flagging, as arithmetic not result.** The fixed term's
share is not constant — 61.5% at −D 1, 15.7% at −D 4. Request-serving time per
PE: 45.0 / 37.9 / 43.1 / 75.0 ms at D=1..4. At −D 1 aggregation would remove
~20.8 ms per PE, taking it to ~24 ms, below every other depth — and −D 1 fetches
10.2M nodes not 40.7M, wastes 5.5% not 76.5%, holds 628 MB/process not 838.
**Aggregation is worthless at the depth we ship and worth most at the depth we
rejected.** The risk is the same buffer: −D 1 already has the longest walk
(0.580 s vs 0.448) from round trips, and the buffer adds ~0.65 ms of flush
latency per channel. Unmeasured, and the only reason I would not just recommend
the pair.

## 3. The −D 4 anomaly: it ships interior structure

                      -D 3           -D 4        ratio
    interior nodes  11,438,781    35,073,778     x3.07
    cached leaves   29,295,354    28,355,621     x0.97   (flat)
    particles      263,143,034   319,046,426     x1.21
    requests         2,644,471     3,665,431     x1.39

Your hypothesis was that subtrees bottom out at leaves and the extra depth ships
particles. The content split says otherwise: **interior nodes triple while the
leaf population is flat**, so the subtrees are getting deeper in branch
structure — and branch structure is exactly what nobody examines (`never` goes
76.5% → 84.7% while `used` stays at ~9.6M).

The frontier half of your hypothesis is supported and explains the
non-monotonicity: a depth-D slice of an oct tree has a frontier growing with the
branch factor per level, while useful coverage does not grow at all (working set
fixed at 9.6M). So 3→4 multiplies the frontier faster than the coverage it buys,
and requests rise 39% even though each reply is 12% bigger. Leaf *mix* also
shifts toward fuller leaves — 8.98 → 11.25 particles per cached leaf — which is
why particles rise 21% on a flat leaf count.

**And the field that would settle it directly is the one the labelling bug
hid**: the placeholder population is exactly `used_nodes`, which `FoF3.C:783`
packed over. The label is fixed at 6b2b623; reporting the *real* `used_nodes`
alongside would make this measurable instead of inferred.

## 2. Queued

Job 5329100, −D 2 vs −D 3 at 128 nodes / 1024 processes / 7168 PEs, 2 reps each
plus a shakedown. Pending on Resources — 128-node allocations queue longer than
anything this campaign has run. Results separately.

## Files

    relay91.txt                 the above in full
    relay91-reqcost.py          the cost regression (pairs within one log)
    relay91-dsweep-128n.sbatch  the queued 128-node job
