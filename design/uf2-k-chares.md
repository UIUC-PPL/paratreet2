# k UF_2 chares per process: examined, viable, currently unmotivated

**STATUS: DESIGN EXAMINATION (Kale's proposal, 2026-07-30) — architecture
verified clean, ~30-line change, but the 2B measurement says the cost it
would attack is not there. Parked with the design recorded; revisit if a
workload makes lib-chare CPU time large.**

## The proposal

uf2 runs one UnionFindLib chare per process; at larger ppn each chare
owns proportionally more vertices, and the ppn-31 A/B (job 19571540)
showed uf2 growing 1.06/1.20 s -> 1.49/1.97 s when processes halved.
Kale's idea: split each process's tips over k chares (k=2: even tips ->
chare 0, odd -> chare 1) so the per-chare serial work halves and the two
chares run on different PEs. Noted subtlety: an edge can connect an even
and an odd tip (including via a union arriving from another process), so
find_boss routing must reach the correct local chare from the parity,
beyond the owner encoding.

## Architecture: the idea is clean

Verified against unionFindLib.C (fof_with_aggregation) and paratreet2's
encoding:

- **Routing is 100% locator-driven.** Every send site — union_request,
  find_boss1/2, anchor, path compression — computes its destination chare
  with the registered `getLocationFromID(vid) -> (chare, arrIdx)`; every
  locality test compares `thisIndex` (chare), never a process rank. There
  is NO protocol change: with a parity-aware locator, all traffic —
  including Kale's remote-union-to-odd-tip case — routes to the right
  chare in one hop, because the parity is IN the vid: paratreet2's
  encoding is `(process << 43) | tip`, and tip (a global particle order)
  carries its parity in bit 0. Locator becomes
  `{ (vid >> 43) * k + (vid & (k-1)), vid & mask }` (k a power of 2);
  tip parity is uniform, so the halves balance.
- **Change surface (~30 lines):** an init variant with chare count
  `k * CkNumNodes()`; UFNodeMap generalized to
  `procNum(i) = CkNodeFirst(i/k) + (i%k) * CkNodeSize(i/k)/k` (spread the
  k chares over the process's PEs so their work actually parallelizes);
  the paratreet2 locator above. The bound Prefix array and boss counting
  size off the chare count and generalize untouched.
- **Costs are negligible.** The local_union fast path (both vids on the
  submitting chare) loses the ~50% of process-local unions that cross
  parity; they become one-hop process-local messages. At 2B the WHOLE
  run has 737k-970k edges (~10k/process), so this is milliseconds.
  Component NUMBERING may shift (prefix over k x chares) but the
  components count/histogram — our determinism observable — is
  label-value-invariant.

## Measurement: the motivation is not there (2026-07-30)

From the 2B sum-detail traces (job 19559585, per-PE per-EP, 1 ms bins),
summing all uf2 EPs (union_request(s), find_boss1/2, find_components,
boss counting, set/prune component, Prefix):

- Total uf2 EP CPU time machine-wide: **4.1 s across 1920 PEs**.
- Concentrated exactly where the one-chare design predicts: 108 PEs with
  >10 ms, ALL of them rank-0 (lib-chare) PEs of their process.
- But the busiest lib-chare PE works only **0.117 s**.

So the 1-2 s uf2 WALL is >90% waiting, not chare-serialized compute: the
phase has 2+ QD rounds (each ~90-130 ms settle at this scale on
reconverse — charm-notes/reconverse-qd-latency.md), the prefix
reduction, and large run-to-run variance (ppn15 reps: 1.06/1.20 s; 80M
reps: 0.05-0.33 s). Splitting chares cannot shorten a wait. The earlier
"boss scan over ALL local vertices" cost that would have motivated this
died with sparse-uf2's lazy vertices: scans now touch only
~2 x edges/process vertices (~20k at 2B).

The ppn-31 uf2 regression (+0.5-0.8 s) therefore is NOT a
serial-scan term either; it is barrier/latency structure at fewer,
larger processes (plus noise). The lever for uf2 wall is the QD/latency
side: replacing library QD rounds with counted completion where counts
are knowable (the standing recommendation in the QD note), not more
chares.

## Verdict

Do not build now. The design above is ready if a future workload
(denser edge sets, much larger components, a data-parallel client
pushing millions of unions per process) makes lib-chare CPU a real
term; the trigger to revisit is lib-chare EP time per PE approaching
the uf2 wall in a sum-detail trace, i.e. the opposite of today's
0.12 s vs 1-2 s.
