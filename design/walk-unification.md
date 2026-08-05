# Walk unification: local traversals as framework drivers

**STATUS: AGREED DIRECTION (Kale, 2026-08-05), design for the next
structural round. No implementation yet.**

## The principle (Kale)

paratreet2 exists to support tree walks. Local versus across-process is
a smaller variation than the commonality among walks. Therefore the
framework should offer ONE walk abstraction — the visitor contract:
certificates and actions in open()/leaf(), state carried by the visitor
through group-branch pointers, application fields through the Data
payload — crossed with two small axes:

1. WHERE the pair's data lives: chare-local (a tree against itself),
   PE-local (pairs among one PE's trees), process-local (pairs across
   the process's PEs), remote (the global tree through the cache). Only
   the remote scope engages fetch/park/install machinery at all.
2. HOW work is scheduled and terminated: chare-driven traversal closed
   by quiescence detection or by credit counting (design direction
   2026-08-05, serial mode); pool-claimed unit scheduling closed by the
   process-local deposit chain.

## Evidence the abstraction already holds

- Mutation is not a barrier: gravity's leaf() mutates accelerations;
  the phase-3 FoFEdgeVisitor consults AND updates shared search state
  mid-walk (the SEEN table is a mutation-driven prune) inside the
  standard contract.
- Application node fields are not a barrier: everything FoF phase 1
  hangs on nodes went through the Data payload (FragData annotations,
  process-level tips) — the mechanism any application uses.
- Walk policies already live as visitor traits (SplitLargerOnly,
  TargetMustBeLeaf, SkipLocalSource, SkipMirroredPairs) with
  SFINAE-defaulted opt-in, so adding drivers does not disturb existing
  visitors.
- The counter-example that proves the gap: FoF phase 1 re-implements
  descent mechanics (mindist pruning, split ordering, leaf-pair loops)
  in fof/FoFPhase1.h purely because local drivers do not exist in the
  framework.

## What moves into the framework

1. Local drivers, visitor-parameterized, with BOTH sides mutable (both
   are owned; the const-source convention exists for cached copies):
   - self-walk: one tree against itself (CallSelfLeaf semantics);
   - PE-local pair walk: all pairs of the PE's trees;
   - process-local PAIR POOL: geometric enumeration (mindist-gated,
     split-leveled so no unit hides a dense-boundary giant), LPT sort
     by a visitor-supplied or default geometric cost key, atomic claim
     cursor across the process's PEs. Unit = (node*, node*, key) —
     needs only bounding boxes, which the Data concept guarantees.
2. The deposit-chain termination harness: process-local stage chaining
   by shared counters with one closing reduction (the QD-free pattern
   of phase 1's phaseA -> pool build -> phaseB -> merge -> relabel).
3. Later, on the same pool: the steal-based cross-process offload
   (design/phaseb-offload.md) — ship-once/use-many units, remaining-
   work metric from the claim cursor, node-level monitor, buddy nodes.
   Building it against a framework pool means every application
   inherits the balancing.

## What stays in applications

The algorithms, as visitors: FoF's star-union positive certificate,
connectivity-suppression memos, and the union-find itself are
open()/leaf() logic over visitor-carried state — exactly as SEEN works
in phase 3 today.

## Clients

FoF phase 1 (first client, migration gated on bit-identical counts at
every scale with existing baselines); SPH density/force local loops and
collision detection (the generality proof); Barnes-Hut local
interactions if the gravity example grows a same-process fast path.

## Phasing (each step separately gated)

1. Lift the pair pool + deposit chain out of FoFPhase1Node into
   framework services; FoF phase B becomes the first client.
2. Add self-walk and PE-local drivers; phase A becomes a client; the
   phase-1 walk bodies shrink to visitors.
3. Second client (SPH-style local density pass) to prove generality.
4. Steal-based offload on the framework pool (phaseb-offload.md).

Queue position: after the in-flight trace round (2B sumdetail with the
pruned walk) and the serial-mode credit-counter termination; this work
is the on-ramp to the phaseB offload.
