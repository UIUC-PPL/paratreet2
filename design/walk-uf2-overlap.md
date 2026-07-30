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
