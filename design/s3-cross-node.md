# S3 cross-node helpers (Kale, 2026-08-15) — design, with the data that
# makes it the strongest remaining lever

## The finding that justifies it: the straggler's BLOCK cannot balance itself

S3 steals within a physical-node block (FOF_PROCS_PER_PNODE, 8 on both
machines). From the Frontier 128-process load_model data:

| block | total phaseB (8 procs) | members, sorted |
|---|---|---|
| **6** | **31.19 s** | **15.1  9.6**  1.5 1.2 1.1 1.1 1.0 0.5 |
| 10 | 29.90 s | 8.9 7.1 6.8 2.4 2.0 1.8 0.8 0.1 |
| 0 | 20.21 s | 4.5 3.8 3.7 3.4 2.2 1.8 0.7 0.2 |
| ... | | |
| 9 | 7.00 s | 1.9 1.8 1.0 0.8 0.6 0.5 0.3 0.2 |

The straggler (node 55, 15.14 s) and the SECOND-worst process (node 54,
9.63 s) are **in the same block**. Block 10 holds three of the top five.
That is not coincidence: the decomposition is spatially contiguous, so a
dense region lands on adjacent processes, which are adjacent ranks, which
share a physical node.

**Therefore within-node stealing is capped by the block mean.** Even with
PERFECT intra-block balance, block 6 settles at 31.19/8 = **3.9 s per
process** — about 2x the global mean (2.03 s) and ~15x the 0.25 s
granularity floor. No amount of local stealing gets under that. Meanwhile
the lightest blocks sit near 1 s per process, a ~4x spread ACROSS blocks,
and the 16 least-loaded processes in the machine are at 0.12-0.47 s.

Spare capacity below the median: **3.21 PE-s inside the straggler's own
block, 54.2 PE-s globally** — a 17x difference. The capacity the straggler
needs is not where it is allowed to look.

## Why it should be nearly free to the donor

relay1 §22 settled where donor time goes: the SEND is ~1 ms and sits at
99.2% through the s3ShipOrder block; the cost is charm's sizer+pack
(~49 ms/grant after the POD wire). Pack cost does not depend on where the
grant is going. **So a cross-node grant costs the donor essentially what
an intra-node grant costs** — distance is the fabric's problem, and the
fabric is not on the donor's critical path. That is the fact that makes a
conservative version low-risk.

## Design (conservative, as specified)

**Helper selection — measured, not predicted.** A global coordinator
(node 0) to which each process reports when it DRAINS its own pool. The
first arrivals are, by definition, idle; no model is consulted. This
matters because relay5 showed our metric ranks the extreme well (worst at
#1, 3/3) but the middle and bottom poorly — exactly the region we would
have to trust to "predict the 2 least-loaded". Drain-reporting reuses the
event the block protocol already generates (s3Drained).

**Scope — two helpers.** Only the globally worst donor gets remote help,
and at most 2 remote helpers, so the added grant traffic is bounded and
attributable. Everything else keeps stealing within its block, unchanged.

**Trigger — when local stealing is already underway.** Arm the remote
path at the moment the block coordinator issues its first order for that
donor, so cross-node help is strictly additive to a mechanism already
running, never a replacement for it.

**Direction — donor-initiated**, matching the existing shape: the global
coordinator matches an idle remote process to the worst donor and tells
the donor to ship; the donor packs and sends exactly as it does today.
No new data plane — s3Shipment/drainForeign/s3Return are unchanged.

## The risk to watch, and why it may have expired

relay1 §19 found GRANT COUNT, not work moved, predicts the straggler
(r=+0.919): each grant costs its donor a roughly fixed collection time,
and past some count the marginal grant is net negative. More helpers means
more grants. BUT that correlation was measured at 117.8 ms/grant; the POD
wire cut it to 49.4 ms. The break-even count therefore moved, and by how
much is unmeasured. Two helpers is the right hedge: enough to see the
effect, small enough that if the old correlation still governs, the damage
is bounded and visible.

## What the A/B must report

phaseB_s max (the prize), grant count and units/grant per donor, donor
s3_time, AND phase3/merge/relabel — a remote helper's returned edges cross
a node boundary, so the phase-3 term is where an unexpected cost would
appear. Arms: baseline, +1 remote helper, +2 remote helpers, same cell.

## Standing caveat

The measured ceiling for fixing node 55 alone is -33% of the phaseB max
(levelling #1 to #2) — and #2 here is node 54, IN THE SAME BLOCK. So
cross-node help for 55 alone leaves 54 setting the phase. Two helpers
aimed at the two worst processes of block 6 is the configuration that
actually moves the max, which is an argument for the "2 helpers" scope
being about right for reasons beyond conservatism.
