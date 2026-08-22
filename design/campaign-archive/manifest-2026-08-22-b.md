# uploads/ — 2026-08-22 11:00

**START HERE: `relay79.txt` — add_size is dead work and removing it is worth
-1.9%, verified by the exactness gate itself.** Then `relay78.txt` (your drain
question answered by counting, plus the backward short-circuit). `relay77.txt`
and `relay75-addendum.txt` are still here unpulled.

## relay79.txt — the dead-field result

    A  FOF_UF_SIZES=1   4629.9 ms [4625.2..4634.7]   walk 0.499 s
    B  FOF_UF_SIZES=0   4541.0 ms [4538.2..4543.7]   walk 0.420 s
    B - A  -88.9 ms  -1.92%   ranges separated by 81 ms;  walk -15.8%

**Both B reps are EXACT**, and that is the test rather than a formality: the
gate checks components *and* max_size, so if `size` were read anywhere on the
path to either number, B would have come out DIFFERS.

The knob gate has an exact identity — in both B reps `addsize_SKIPPED` equals
`fb2_UNION` to the unit, so it is one skip per union, no more and no fewer. And
the union protocol is unchanged: fb1_root, fb2_UNION, fb2_SAMEROOT_discard and
fb2_flip all move by less than run-to-run variation across the four arms. Same
forest, no size bookkeeping.

I had hedged that add_size is a side flow and might not shorten a latency-bound
drain. It did anyway, and the −79 ms in the *walk* bracket says why: Charm entry
methods are non-preemptive, so every add_size was delaying whatever find message
sat behind it. This is not pushable as-is — the library is shared and
`local_union` still does its direct `size +=`, so the field is inconsistent
rather than absent under the knob. It wants a documented opt-in no-sizes mode.

## Correction on "on-chare" — you were right (relay78 §11)

I meant *within a process*: UnionFindLib is one chare per process
(`unionFindInitOnePerNode`), so chare == process here. And your PE-set point is
right — with AUTO the split resolves to PEs-per-process, and `common.h:89-91`
says "sets == ppn defers ALL cross-PE pairs to the phase-3 walk". Those edges do
go through the walk.

What I should have said: the 794,192 local_unions are not edges that skipped the
walk. They are walk-generated uf2 edges whose endpoints land on the same chare,
so `union_request`'s fast path resolves them locally with zero messages.

    uf2 edges submitted        ~1,247,000
      same-process, free          794,192   64%
      cross-process, find_boss    453,183   36%

**And that is the mechanism behind the +28% serial note.** The split manufactures
those 794k same-process edges; dist absorbs them for nothing, serial cannot,
because `flushPhase3Edges` concatenates every edge to PE 0. Which makes
contract-then-gather not a general nicety but specifically the repair for what
makes serial lose under the split.

## relay78.txt — what is actually happening in the drain

Counted in the library, not inferred: every branch point in the find_boss
protocol, summed over the array (`[UFSTAT]`). Job 5326279, all arms EXACT.

**Your three candidates, over the whole run:**

    UNION (parent pointer changes)     438,068    80.7%
    FLIP  (restart, id order wrong)     89,865    16.5%
    SAME-ROOT DISCARD                   15,116     2.8%

**It is not discards — four out of five chains end in a real merge.** And the
chains are already about one hop: 552,412 climb hops over 543,048 chains,
1.02 each. Two internal checks come out exact: union+flip+discard equals the
number of boss1 roots found, and addsize_root equals the union count.

**There is a fourth outcome you did not list, 5.9x bigger than discards.** The
FLIP: find_boss2 reaches a root whose id is larger than the incoming one,
refuses it on the min-heap rule, and re-issues `union_request` from scratch —
the whole two-phase find thrown away. `union_request` already orders the two
vertex ids at submission; it cannot order the two ROOT ids, not knowing them.

**About 30% of every union in the run happens inside the drain** — roughly
132,600 real merges in 265 ms at 0.67% utilization. The drain is a third of
the actual merging work, serialized by dependence.

## Your backward short-circuit — already in the library, and measured

It was written and commented out at both call sites (`unionFindLib.C:346` and
`:445`); `senderID` is already carried in every message. Enabled behind
`FOF_UF_SHORTCIRCUIT=1`.

Your race needs no epoch. Parent ids strictly decrease toward the root, so
"closer to the root" is just "smaller", and accepting only a strictly smaller
id can never move a pointer the wrong way whatever happened since the send.

    arm                     Iter0                      remote climb hops
    A  short-circuit off   4612.4 ms [4582.6..4642.1]      337,525
    B  short-circuit ON    4604.0 ms [4595.6..4612.5]      228,305   -32.4%

    sc_sent 56,493   sc_applied 3,401 (6.0%)   sc_rejected 53,092 (94.0%)

**It works and buys nothing.** Remote climb hops fall by a third, ranges
separated, and the wall does not move. Two independent mechanisms have now
shortened chains without buying time — the wave and this — which is the
cleanest evidence that chain length is not the constraint. 94% of the backward
offers arrive already matched or beaten, largely by the local path compression
find_boss1/find_boss2 already do.

One latent bug in the commented code, fixed: it used `std::pair<int,int>` for
a `std::pair<int,uint64_t>` return, truncating the local index.

## What it says about collect-and-finish

Your premise splits: not long chains (confirmed twice), but not discards
either. Still points your way, for a different reason — the volume is small
(132,600 unions in the drain, 4,310 PE-ms of protocol over the whole run), it
would kill the 16.5% flips for free since a collector holds both roots, and it
removes round-trips, which is the only thing that can touch a 280x
wall-to-work ratio. **The open risk is the trigger**: "collect what is left"
needs a moment when the cascade is nearly done, and relay76 showed the
fireUF2Edges barrier is not it. That is what killed the wave, so it deserves
an answer before code.

## Your three follow-ups — relay78.txt sections 6, 7, 8

**You are right about the partial order.** Id order is total, ancestry is
partial, and my argument silently assumed the offered id and the current parent
both lie on the vertex's current root path — which fails exactly under path
compression, when the pointer has already jumped over the offered vertex. So
"never moves the pointer away from the root" is not a theorem. The guard is
still safe, for two simpler reasons: a smaller id can never be a descendant (so
no cycle), and trees only merge (so the component is still right). What the
comparison buys is a strictly decreasing id per accepted write — progress, not
distance. I have also softened the 94% claim to what it actually says: those
offers did not carry a strictly smaller id.

**add_size** adds a delta to the root reachable from a vertex, forwarding
upward if that vertex is no longer a root. It is called only at a union, just
before the parent write. It must forward because the target may itself have
been merged since it was observed as a root; a plain write would strand the
count. It is exactly-once because the size transfer and the parent write happen
in one non-preemptive entry-method execution, so any other add_size aimed at
that vertex is processed strictly before (carried along) or strictly after
(forwarded up). The invariant: a size field is meaningful only while the vertex
is a root. `max_size` is our EXACT gate, so every arm being EXACT tests this.
Measured: 8.6% of add_size calls had to forward — the "root moved" race is real
at about one in twelve.

**ACIC is the right shape and one number reshapes the payload.** Walk
completion is locally decidable, which is exactly what the cascade never was —
that is why it solves what killed the wave. But the edges are already in:
73,365 edge batches during the walk against 4,000 after, about 5%. So a
reduction collecting "edges not yet submitted" collects almost nothing; what
remains in the drain is the cascade of already-submitted edges.

That splits the design. **Option 1 — do not stream the boundary edges, gather
them** — makes the in-flight question disappear rather than solving it, and the
volume is small: ~453,000 distinct cross-chare edges touching ≤906,000
vertices, about 7 MB, sequential union-find in milliseconds. It costs the 104 ms
of streaming overlap (relay76) and returns the 265 ms drain plus the 16.5% of
flips, since a central finisher holds both roots and orders them once. On paper
net 150–200 ms; that is arithmetic, not a run. **Option 2 — keep streaming and
mop up** — needs termination detection, which your ACIC rounds give for free
via a sent/received counter pair per PE (Mattern's four-counter method, no
runtime quiescence); the hard part is that the library does not track which
edges are still unresolved, so mid-cascade collection needs new per-chare
bookkeeping that option 1 does not.

## Two corrections of mine

My first attempt to answer from traces was wrong — executions minus creations
measures window skew, not terminations, since sends equal receives over a run.
And relay75/77 folded the labeling scatter into the union-find group; split
out, the drain tightens from 313 ms to 265 ms. Every earlier conclusion
survives; the number moves by 48 ms.

## The patch — two files, one per repository

`APPLY.md` has the exact commands. In short: the change spans **two repos**, so
it is two diffs, applied from each repository's root on top of the upstream
HEADs `paratreet2 57f2395` and `unionfind d72ee66`. A single combined diff
cannot be applied by `git apply` from either root, which is why the earlier
single-file form is gone. Both were verified by reverse-applying them against
the Frontier working tree.

Everything in it defaults to today's behaviour: the `[UFSTAT]` census (always
on, cost not detectable), `FOF_UF_SHORTCIRCUIT=1` (your backward short-circuit,
off by default), and `FOF_UF_SIZES=0` (the −1.92% result, off by default and
**not safe to ship as a default** — `local_union` still does a direct
`size +=`, so the field becomes inconsistent rather than absent).

## Files

    APPLY.md                    how to apply the patch pair, and to what
    relay79.txt                 the dead-field result
    relay79-unionfind.diff      patch, apply in unionfind/   (NOT pushed)
    relay79-paratreet2.diff     patch, apply in paratreet2/  (NOT pushed)
    relay79-nosizes-16n.sbatch  the relay79 job
    relay78.txt                 the drain census and the short-circuit
    relay78-census-16n.sbatch   the relay78 job
    relay78-drainanatomy.py     per-window protocol volumes
    relay78-groups.py           labeling split out of union-find
    relay77-waveattrib.py       wave-aware attribution
    relay77.txt                 the periodic wave (unpulled)
    relay75-addendum.txt        the "stable" correction (unpulled)

Binary `merged/FoF3.cpu-prod-cen` md5 `1775a0218343a85ee2213abb720ceb05`.
Nothing pushed; both trees at 57f2395 / d72ee66 with the patch uncommitted.
