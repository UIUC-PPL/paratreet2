# The three open issues, explained
# 2026-08-16, after job 5287653 (top-6 PE-set split: phase1 −19.2%, Iter0 −8.4%)

## 1. "The union-find is now the binding cost"

**What happens.** Splitting a process's PEs into sets means a FoF component
that straddles a set boundary is no longer merged during phase 1. It ends
phase 1 as two or more separate FRAGMENTS, each with its own tip. Phase 3 then
has to discover an edge joining them and run a union-find over every fragment
in the machine to relabel them.

So the split trades phase-1 time for fragments, and fragments are what `uf2`
(the union-find stage) costs money on:

| arm | fragments (`relabel_map` entries) | unique edges | uf2 |
|---|---|---|---|
| base | 745,540 | 485,013 | 0.448 s |
| top6 s=14 | ~+5% | 512,837 | 0.476 s (+28 ms) |
| all s=2 | — | 1,260,794 | 1.461 s (+1.01 s) |
| all s=14 | 2,449,538 | 1,647,442 | 2.025 s (+1.58 s) |

**Why it scales with HOW MANY processes are split, not how finely.** Each split
process contributes its own extra fragments. Splitting 6 processes fourteen
ways adds fragments from 6 processes; splitting 128 processes two ways adds
fragments from 128 processes — roughly twenty times more contributors, so far
more extra fragments, even though each contributes fewer. That is why
`top6 s=14` costs 28 ms and `all s=2` costs 1.01 s.

**Why it is "binding".** The phase-1 saving grows as you treat more processes
(top1 −327 ms → top2 −471 → top6 −534 of Iteration 0). The uf2 cost grows too,
and past about six processes it overtakes: `all s=14` wins 1157 ms in phase 1
and loses 1249 ms overall. **The union-find is the thing that stops us from
simply applying this everywhere**, which is what makes Kale's point about
serial-mode edges going to the root, and communication dominating in dist mode,
the exact thread to pull. Job 5288750 is testing precisely that.

## 2. "The victim list is still hand-supplied"

Today the recommended configuration is literally
`FOF_PE_SETS_NODES=55,54,87,83,80,107` — six process ranks I read off a
baseline run's measured `pb_sum_s`. That is fine for an A/B and useless in
production: a real run has no baseline to read, and the split has to be decided
BEFORE phase 1, from the tree alone.

**The predictor already exists and is already validated.** relay5 measured the
`m2_cross` model against the truth over three repeats: it ranks the true worst
process **#1 in 3/3 repeats** and gets **4 of the top 5**, with rank correlation
+0.948. The design doc's trigger is a robust z (median/MAD) above 7, which
separates the outliers on both machines tested.

So the work is: compute `m2_cross` per process before the pool build (it is
already computed for the `load_model:` instrument), take the top N by robust z,
and set the split on those — no env var, no hand list. It was not worth wiring
up when the payoff was shedding's −49 ms. At −534 ms it is.

## 3. "phaseA's 19-process plateau is untouched"

**phase 1 = phaseA + phaseB.** phaseB is pairs between pieces on DIFFERENT PEs
of a process; phaseA is same-PE work — a piece against itself, plus pairs
between pieces sharing a PE. The PE-set split can only remove phaseB work,
because a set boundary is a boundary between PEs. No set assignment can
separate two pieces that sit on the same PE.

The numbers, from `phase1-idle-structure.md`:

- phaseA total work is 2061 PE-s = **1.150 s** if spread perfectly over 1792
  PEs, but the stage takes **2.016 s**. That 0.87 s gap is phaseA imbalance.
- Its shape is a PLATEAU, not an outlier: top-10 processes are
  2.02, 2.01, 1.92, 1.90, 1.87, 1.82, 1.79, 1.78, 1.76, 1.76 against a median
  of 1.20, and 19 processes exceed 1.5 s. **Removing the single worst gains
  10 ms.** Targeted anything is useless against that shape.

**Why it now dominates.** The top-6 arm took phaseB down to 0.50 s, so phase 1
is 2.639 s of which roughly 2.1 s is phaseA. And the extreme arm proves the
point: `all s=14` drives phaseB to **0.001 s** — every cross-PE pair pushed to
phase 3 — and phase 1 still takes **2.110 s**. That residue is phaseA, and it
is about 0.96 s above its own perfect-balance floor.

**What could ever touch it.** Machine-wide, `m2_self` is 3.708e11 of 7.543e11
total pair work — **49% of all phase-1 pair work is self-pairs inside a single
piece**, which cannot be moved anywhere without moving the piece. The
addressable part is `m2_intra` (31%), pairs between different pieces on one PE.
Moving those needs either migration (measured: 59 ms + 142 ms per million
particles, and capped by the union-find the same way) or a finer decomposition
so that fewer pairs are same-PE in the first place. Neither is a small change,
and neither is on the table yet.

## How these three interact with the 64/128-node plan

- Issue 1 gets WORSE with more nodes in serial mode: four times the processes
  means four times the fragment contributors for a machine-wide split, so the
  gather-to-root union-find is exactly what should break first. That makes the
  serial-vs-dist crossover a scaling question, not a 16-node one.
- Issue 3 gets BETTER: at fixed problem size, more nodes means fewer particles
  per process, so phaseA per process falls. Whether the PLATEAU shape survives
  is an open question and worth reading off the same scaling runs.
- Issue 2 is scale-independent, and becomes more valuable at scale because
  hand-listing victims across 512 processes is not workable.
