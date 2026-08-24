# FoF optimization campaign, August 2026 — what was established and what was refuted

**What this is, and whether you need it.** This is the record of an
optimization campaign, not documentation for the application. **If you want to
RUN FoF3, ignore this file** — the README carries the recommended
configuration and every flag, and it is kept current. **If you are thinking
about an OPTIMIZATION, check here first.** Most of what follows is negative
results: ideas that were designed, implemented, measured, and found not to
work. They are recorded because the reasoning behind each was sound enough
that someone will have it again, and re-deriving a refutation costs a machine
allocation and a week.

Scale for everything below: 2 billion particles, Frontier, 16–128 nodes,
CPU and GPU arms. Reports are in `design/campaign-archive/` (relay1–110);
each claim here names the report that carries the data.

---

## 1. What shipped

| change | effect | where |
|---|---|---|
| PE-set split (`FOF_PE_SETS` AUTO, MODE 1) | −14.5% to −16% at 2B/16 | §36/§38 |
| GPU helper-thread affinity fix | −23% at 16 nodes, 2471→914 ms at 128 | relay45/108 |
| leaf size 32 default (CPU) | −11.5% against the old 12 | relay61–64 |
| sort+scan histogram path | −283.6 ms on the CPU iteration | relay74 |
| `FOF_UF_SIZES=0` | ~−2%, exact; union-find drain −18–27% | relay79/81/82 |
| `-s` canopy cap | breaks the 64→128 scaling stall; −41% on GPU | relay93/96/106 |
| canopy collect gate | 69,670 → 1,170 driver messages; sort 97× smaller | relay97/98 |
| prefix removal (self-naming) | wall-neutral, but a prerequisite | relay74 |

## 2. What was refuted — the substance of this document

**The compression wave.** Re-run the labeling cascade mid-stream as global
path compression. Implemented in three trigger forms: once at the
`fireUF2Edges` barrier, the same at `-E 0`, and periodically mid-cascade
(QD-safe, gated on structural unions). **All three rewrite ~260 parents.** The
benefit is capped by the forest's actual depth, not by the trigger, while each
pass costs a global sweep: 25,637 PE-ms to save nothing on a 4,165 PE-ms
union-find. Periodic firing cost +71% at 25 ms and +176% at 10 ms.
*Do not build a v3 for FoF; a better trigger cannot fix a capped benefit.*
The refutation is workload-bound — FoF forests are shallow by construction —
so the code is retained compile-gated (`CONCURRENT_COMPRESSION_WAVE`,
unionfind Makefile.common; zero footprint when off) for explicit-graph
studies where chains can run deep. (relay74/76/77)

**Chain shortening in general.** Three independent mechanisms shortened
union-find chains and none bought time: the wave, the backward short-circuit
(remote climb hops −32%, wall unchanged), and disabling local path compression
(hops +26%, tail 250× longer, wall unchanged). **Chain length is not the
constraint.** (relay77/78/83)

**Chain concurrency.** The drain's 128 active PEs are the one-element-per-
process home PEs, so tail concurrency is structurally capped — but they are
*keeping up*: 2 µs median executions, 99% of continuous busy runs ending
within 152 µs, 3.7% busy in the drain. There is no backlog for a nodegroup or
element sharding to relieve. (relay84)

**Root concentration.** 125 of 128 chares root straddling components; the
busiest holds 3.7%. No hotspot to break up. A dense-bits comparator would also
be a correctness break — dense indices restart at 0 per process, so ties make
two roots point at each other. (relay85)

**Element sharding.** Would lift the tail cap, but on CPU the dominant cost is
losing the same-process fast path (~790k of 1.24M edges resolve free today)
against only ~151k added hop messages — 5:1 the wrong way. (relay83)

**Contract-then-gather (`-u gather`).** Correct at 2B *bitwise*, and it repairs
80% of `-u serial`'s +33% penalty by retiring the 739k same-process edges the
split manufactures. Still loses to `dist` by 6.6%: it forfeits ~132 ms of
streaming's walk-concurrent cascade and pays a serial finisher that grows with
process count. The root-pair collapse it was partly premised on buys **1.6%**.
Kept as a validated instrument, not a candidate. (relay86)

**The folded two-phase find.** Withdrawn before implementation. Chains are ~1
hop (0.74 remote hops per edge), so today's serial two-phase find is ~2 remote
hops while a folded version costs max(1,1) + a return to the submitter + a link
≈ 3. It would add a hop to 100% of edges to save one on 16.5%. And a flip is
not "a two-phase find thrown away" — the re-issued `union_request` lands where
r_w is already a root, so exactly one extra remote message follows, which the
census confirms (`fb1_root = UNION + flip + discard`).

**A within-process walk phase.** Pooling the local-local tree pairs to balance
them cannot work: with the split OFF the walk's per-PE imbalance is 4.00×
against 3.89× with it ON. Removing the local-local pairs does not reduce the
skew — it lives in cross-process pairs. The hotspot PEs are the same across
arms *and across jobs*, so the skew is a fixed property of the decomposition.
(relay87)

**Both models for the optimal `-s` cap.** Pre-registered before the run, which
is the only reason the run could settle anything. Fan-in predicted a
P-independent optimum; at 16 nodes uncapped is best and at 64 nodes cap 128 is
the *worst* arm. The chain model predicted s\* ∝ 1/P; the 16-node curve is not
monotone. What survives is a **crossover, not an optimum**. (relay96)

**Request aggregation (htram) for the union-find tail.** The precondition
fails: 8 messages per ms per PE spread over 128 destinations is ~0.065 per ms
per destination, so a 4-item buffer never fills and flush-on-idle degrades every
send to a singleton. For the cache path the precondition *holds* (median PE
talks to 6 destinations, top-4 carry 97%), but `requestNodes` costs
2.7 µs fixed + 0.85 µs per node, so at the shipping depth aggregation can
address 18.2% of 18.2% of walk wall. (relay89/91)

**Cache-fetch reduction by deduplication.** Zero duplicate traffic exists, by
construction: the CacheManager is a nodegroup (one cache per process) and there
is no eviction path within an iteration. And bundling is already at its optimum
— the `-D` sweep is a U with a flat floor at the default, because the walk's
*used* node count is invariant at ~9.6M across a 6.2× range of total fetches.
(relay88/90)

## 3. Two rules this campaign paid for

**Quote within-job deltas; never cross-allocation ones.** Two jobs disagreed on
the *sign* of the same 60 ms effect, each with tight separated within-job
ranges. Between-allocation spread is 2–4% — the size of the effect. A −8.3%
result was published and later retracted when a third measurement of the same
quantity gave −14.8%. *No wall figure in this campaign is resolvable to better
than about 4%.* (relay96/98)

**Report the wall next to the phase timer.** A 30× startup regression
(`--core-spec=0`, ~1100 s per run at 128 nodes) sat entirely outside every
phase timer and was invisible for the whole campaign, because only phase timers
were read. The per-rep seconds column had been printing 27–50 s where 5 was
expected, and nobody asked. (relay105)

Corollaries earned the hard way: check entry-method **signature widths** in the
`.ci` against the `.h` before theorising about races (a 32-bit truncation there
was dormant for years); gate an instrumented arm on a **predicted value**, not
a threshold, or a wrong prediction passes silently; and small-scale
**multi-process** gates catch what both 2-process and full-scale runs miss.

## 4. Open, and deliberately parked

Placeholder representation (a placeholder is a full `FullNode`, including an
8-pointer children array that is never written — ~26% of every slot);
`eBinaryOct` to cut the frontier ~7× (`design/binaryoct-design-issues.md`);
Charm++ allgather for the canopy exchange
(`design/allgather-design-notes.md`); PAPI to name the dilution mechanism
(parked on a charm toolchain blocker); the ppn 6 poller hypothesis; the m2
dump for the walk tie-break; ~330 ms of helper damage outside every timed
phase; and the `-s` default, left unset because the crossover has no
scale-independent answer.
