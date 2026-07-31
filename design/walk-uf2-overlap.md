# The dual-walk -> distributed-UF_2 phase: overhead, overlap, and a plan

**STATUS: ANALYSIS + PLAN (2026-07-30, from Kale's framing: tiny-message
overhead vs aggregation latency; local walk as low-priority filler;
batched edge submission to overlap uf2). QD latency is assumed fixed
separately (charm-notes/reconverse-qd-latency.md); nothing below waits
on it, and item 1 reduces exposure to it anyway.**

## 1. Measured cost structure (80M, 4 nodes, 480 PEs, pool3-noagg traces)

Walk window 474 ms (first goDown to last walk-EP event), aggregated over
480 PEs:

| bucket | share | meaning |
|---|---|---|
| entry methods | 43.3% | startDual 16.1, Resumer::process 13.6, addCache 7.0, goDown(S) 3.2, requestNodes 3.2 |
| idle | 27.4% | fetch-latency stalls + end-of-window tail |
| black (untraced) | 29.4% | scheduler/message overhead |

Messages created inside the window:

| EP | count | per PE |
|---|---|---|
| Resumer::process | 7,793,025 | 16,235 |
| addCache (replies) | 519,535 | 1,082 |
| requestNodes (pair+subtree) | 519,535 | 1,083 |
| goDown | 54,650 | 114 |

Two conclusions that reshape the intuition:

1. **The tiny-message flood is NOT the node-request traffic.** Requests
   plus replies are ~1M messages; **Resumer::process is 7.8M — 87% of
   all messages in the window**. Every subtree install fans out one
   process(key) message per waiting PE per key. At ~8 us entry time
   plus ~7.5 us untraced per-message cost (black / message count), the
   resumption fan-out accounts for roughly HALF the window's
   entry+black budget. The request path Kale's aggregation idea targets
   is ~3% of window x PEs.
2. **Overhead (black) and latency (idle) are the same size.** Overlap
   (priorities, streaming) can hide idle but cannot hide black — that
   is CPU burned per message. Cutting message COUNT attacks black;
   reordering work attacks idle. Both are needed; neither alone
   suffices.

## 2. The plan, ordered by measured payoff over risk

### (1) Stream edge batches into UF_2 during the walk  [low risk, do first]

Today: walk -> QD -> per-process gather -> union_requests -> QD -> boss
counting. The uf2 machine time is tiny (4.1 s CPU across 1920 PEs at
2B) — its wall (1.0-2.0 s at 2B) is latency chains and TWO extra
quiescence boundaries.

Change: when a PE's edge_buf3 reaches K (start K=4096; edges are
deduplicated per-PE by seen3 exactly as today), submit the batch to the
lib immediately and keep walking; final flush submits the remainder.
Union-find is a semilattice — unions are order-independent and the lib
accepts them any time before find_components — so correctness is
unaffected; the FOF3STAT components line stays byte-identical.
Completion: union traffic joins the WALK's QD (messages in flight keep
QD unconverged), so the separate uf2-injection QD boundary disappears —
one fewer ~100 ms settle even before the QD fix, and find_boss latency
chains overlap the walk's remaining minutes of work and fetch stalls.
Volume is negligible: 970k edges TOTAL at 2B (~10k/process) vs 86M
cached nodes; contention with fetch traffic is not a concern.
Expected win at 2B: most of the current 1.0-1.2 s uf2 wall (ppn15)
hides under the 3.3 s walk; at ppn31 it also erases most of that
config's uf2 regression.

**MEASURED (step 1). Implemented 3b71f9e (-E flag, default 4096; -E 0
= classic oracle). Laptop: 1M/2M/LAMBS-1M exact incl. forced batch
sizes 8-64 (hundreds of mid-walk flushes), classic + reconverse
runtimes, multi-process. 80M A/B (job 19579881, 4 nodes, 3 interleaved
reps): all 6 runs exact (23,707,197; identical 49,312 edges both
arms); performance a WASH as predicted at this scale — walk+uf2
E0 0.56/0.87/0.83 s vs E4096 0.78/0.92/0.67 s, inside the noise band,
because 80M's uf2 wall is essentially one QD settle either way and the
overlap-able find_boss chains are milliseconds at 49k edges.
2B A/B (job 19582087, 16 nodes, 2 interleaved reps): all 4 runs exact
(424,897,832; identical 969,878 edges both arms); performance NEUTRAL —
walk+uf2 E0 4.65/4.52 s vs E4096 4.67/4.67 s.

VERDICT (documented per the campaign rule, including null results):
correctness-clean at every scale, performance-neutral at both 80M and
2B. The mechanism did what it was designed to do — union cascades
demonstrably ride the walk's QD — but the find_boss cascades were
never the uf2 wall: the earlier sum-detail analysis showed ALL uf2
compute is 4.1 s machine-wide with the busiest lib-chare PE at 0.117 s.
The 1.0-1.4 s uf2 wall at 2B sits in what runs AFTER all unions —
find_components' own serial latency structure (boss-count prefix chain
over 128 elements, set_component broadcasts, label collection, plus
QD settles), which no amount of union overlap can touch by design.
That is the sparse-message-latency disease again (reconverse-qd-latency
note), not an application-schedulable cost. KEPT with default -E 4096:
zero measured cost, bounds peak_edge_buf to the batch size, removes
edge injection from the post-walk critical path, and the overlap
benefit becomes real if edge volumes ever grow to where cascades
matter. -E 0 remains the oracle.

### (2) Batch the Resumer resumption fan-out  [the measured hotspot]

The new finding. On subtree install, CacheManager sends process(key)
per (waiting PE, key). Batch per destination: accumulate the keys
resolved by one install burst and send ONE process(vector<Key>) per
waiting PE (or bounded chunks). 7.8M messages become <=0.5M; the
resumption WORK is unchanged, only the per-message tax (~15 us summed
entry-dispatch+overhead) is paid ~16x less often. This is also
precisely the seam the SMP-cache extraction defines (install RETURNS
parked opaques; the client schedules them) — implementing batching now
is a down payment on that interface, not throwaway.
Expected win: order of 10% of the walk window at 80M; the resumer
share grows with miss rate, so likely more at 2B.

**MEASURED (step 2). The flood turned out to be a BUG, not a design
cost: process() filtered its per-PE fanout with `(bit | mask)` —
always true — broadcasting to all 15 PEs per install (519,535 installs
x 15 = 7,793,025, the exact measured count). The one-character `&` fix
alone UNDERCOUNTED components: the broadcast had been masking a second
bug — waiters set requested bits on the PLACEHOLDER, which swapIn
discards, so the installed node's mask was empty and filtered fanout
dropped resumptions (walk QD converged with lost work; 334,753 vs
333,889 at 1M). Complete fix (3350d4a): `&` plus swapIn hands the
displaced placeholder's waiter bitmask to the installed node
(NULL-guarded initial root); late parkers self-process via the
substituted-placeholder re-check. This is precisely the park-vs-install
race the smp-cache-extraction design says must close against install's
publication. Validated: 10k/1M/2M/LAMBS/8M multi-process exact (8M ==
its single-process ground truth), annotate sfc+oct PASS, reconverse
2-proc exact. 80M A/B (job 19584083, pre-fix vs fix binaries, 3
interleaved reps): all exact; **walk wall 0.606/0.571/0.630 ->
0.511/0.493/0.512 s = -16%**, every fix rep faster than every pre rep.
2B A/B (job 19584493, 2 interleaved reps): all 4 exact; walk
3.308/3.618 -> 3.311/3.308 s, ~-0.15 s (-4%). The smaller relative win
at 2B is expected: installs per PE are similar to 80M, so the absolute
message tax removed (~0.1-0.15 s of window) is similar, but the 2B walk
carries ~6x more compute per PE. (One watch item: the fix reps showed
phaseB_s max 4.15/3.22 vs the usual 2.74-2.82 band — phase 1 is
untouched by this change, so this reads as run-to-run system noise on
the heavy process, but re-check in the next 2B runs.)**

### (3) Local pairs as low-priority filler; remote frontier first
[Kale's overlap proposal — attacks the 27% idle]

Order the walk so work that TRIGGERS remote fetches runs first (issue
requests early, park cheaply), and purely-local pair walks run as
LOW-priority messages that fill the stall windows. Charm queue
priorities on the goDown/continuation path make the scheduler do the
interleaving; no new machinery. Two implementation notes:
- The dual walk's per-chare pair loop must not be one long entry
  (nothing can interleave); split local-local pair walks into
  self-messages carrying low priority bits.
- Fetch replies (addCache) and resumptions keep HIGH priority so the
  remote pipeline never starves behind filler.
Ceiling = the idle share minus true tail imbalance; even reclaiming
half the stall portion is ~10% of the window. Synergy with (4): once
local filler hides latency, added request latency from aggregation
becomes nearly free.

**MEASURED (step 3). Implemented dc646ec (-R chunk budget, 0=off
default; DualTraverser pause/stash/low-priority continuation; DFS
order untouched; reconverse's scheduler polls FIFO before the
prioritized queue, so the continuation is strict filler there by
construction). Laptop + reconverse: exact at all configs, chunk
budgets 100-5000. 80M A/B (job 19586178, R0/R2000/R500 x3
interleaved): all 9 exact, but chunking REGRESSES the walk wall at
this scale — means R0 0.487 s, R500 0.549 s (+13%), R2000 0.583 s
(+20%). Reading: post-fanout-fix, the 80M walk's natural
entry-granularity interleaving (~8 chares/PE, resumptions running
between chare drains) already covers the fetch stalls; slicing adds
stash/restore and scheduling overhead with nothing left to overlap.
DECISION PENDING: one -R arm piggybacked on the next 2B job settles
whether the bigger fetch-stall structure there flips the sign; if 2B
is also negative, REVERT the machinery (~70 lines) rather than carry
an off-by-default feature that measured negative — complexity rule.**

### (4) Short-buffer PP aggregation for node requests  [do LAST, gated]

Kale's tradeoff is real but the measured base is small: requests+replies
are ~3% of window x PEs at 80M. If (1)-(3) land, re-measure; only if
the request path's share has GROWN (2B, higher miss rates, more procs)
is this worth building. The right shape when built:
- PP (per-destination) buffers, NOT WP: no indirection hop, batching
  only same-destination requests. Buffer 4-8 requests.
- Flush on CcdPROCESSOR_BEGIN_IDLE rather than a timer: zero added
  latency when trickling (the PE has nothing else to do), bounded
  batching during bursts — and no new periodic timers (the QD
  investigation shows what timers in quiet paths cost).
- CHEAPER ALTERNATIVE FIRST: cache_share_depth + 1 halves the request
  COUNT with a config flag we already have (cost: bigger replies,
  more cached memory — the slim CachedParticle makes that cheaper than
  it used to be). Run that A/B before writing any aggregation code.

## 3. Measurement protocol

Each step: laptop correctness (components byte-identical, 1M/2M, both
runtimes) then Anvil 80M 4-node interleaved A/B (the 10% noise rule),
then 2B when the 80M signal is clear. The observables per step:
(1) uf2 + edge_gather walls -> ~0; walk wall unchanged;
(2) black share and Resumer::process count in the walk window (rerun
    the trace decomposition — script in the session scratchpad, worth
    committing to design/tools/ if reused);
(3) idle share in the walk window;
(4) requestNodes count and walk wall.
