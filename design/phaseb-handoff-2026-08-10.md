# phaseB work: where it stands, and what a new design should keep

Written 2026-08-10 as the closing note for the `phaseb-steal` branch.
Kale intends to resume with a fresh design. This is the single entry
point: read this, then `phaseb-offload.md` sections 13 to 18 if you want
the detail behind any line here. Nothing below assumes you took part in
the work.

## The one-paragraph summary

phaseB is the stage that finds links between subtree pairs belonging to
different threads of the same process. At 2 billion particles on 16 Anvil
nodes it sat near 3.2 seconds from 8 nodes to 128 and would not scale.
Most of a month of effort went into moving that work between processes so
idle processes could help the overloaded one. That work is correct and
measured, and it buys about a third. Then a much simpler change — writing
the frozen fragment ids into the tree nodes so a whole subtree can be
tested for uniformity in constant time — cut phaseB from 3.36 s to 1.58 s
and, in doing so, removed most of the reason to move work at all. A new
design should start from the annotation, not from the movement.

## The state of the repository

```
main                            untouched, still the default branch
main-through-steal-2026-08-09   tag: permanent bookmark at 840aa7a, so no
                                commit is ever lost to a rename
main-without-steal              candidate main with the steal work taken
                                out and every non-steal change replayed.
                                Builds; 1M full verification and 8M pass.
                                Awaiting Kale + Ritvik testing, then a
                                default-branch swap (NOT a force push)
phaseb-steal                    this branch. Everything described here.
                                Tip 06263d4
```

Anvil holds a SEPARATE working tree at
`$PROJECT/x-lkale/software/clusterfinding/paratreet2`, kept in step by
rsync and carrying no commits of its own. Pull the branch there to
reconcile it. Staged binaries in `campaign-bin/`: `FoF3.unif` is the
current one; `FoF3.steal`, `.refine`, `.exec` are older stages of the same
line. `traced-bin/FoF3.steal.proj` and `.sumd` are the traced builds.
Traces from the 2B runs are on the laptop at `traces/steal-2b-0808/`.

## What is worth keeping

**The node uniformity annotation (section 18).** The largest result by a
wide margin, and the most likely foundation for whatever comes next.

phase 1 never wrote anything into `node->data`. What phaseA learned about
a node lived in `cert_rep`, a per-processor hash map keyed by node
ADDRESS, filled only for nodes its walk happened to touch, cleared at the
end of the phase, and impossible to ship anywhere. `FragData`'s
`min_frag`/`max_frag` live in the node itself, but they were computed only
by `Subtree::upwardPass`, which runs AFTER relabel — too late for phaseB
and holding build-time garbage before that.

Writing the frozen tips into that annotation at the end of phaseA, while
tips are stable, gives phaseB `uniform()` (`min_frag == max_frag`) as a
constant-time test that travels with the node. Measured at 2B, three
repetitions, component count exact:

```
                                    off            on
phaseB                            3.36 s         1.58 s
walk, own work                  269 core-s     144 core-s
emissions                          7.79e9         1.13e9
particles walked by star emits  254,237,928         646
certificates answered by node             0   775,672,539
leaf pairs stopped at first edge          0   727,141,542
```

Three uses, all in `FoFPhase1.h`: a certificate on a uniform subtree
returns its tip immediately rather than walking the subtree to emit a
star that contains no edges; a leaf pair of one fragment is skipped; a
leaf pair of two fragments stops at the first edge in range instead of
re-emitting that edge for every particle pair. `FOF_TIP_ANNOTATE=0`
restores the old behaviour for A/B.

Correctness argument: uniformity is monotone across the merge, so a node
uniform at phaseA time is still uniform after relabel, and `upwardPass`
recomputes the annotation for phase 3 regardless.

**The within-process dynamic pool.** Predates all of this (commit
9b6bf65, 2026-07-26) and is on main already. Any new design should assume
it: threads of a process claim subtree pairs from one shared structure
rather than being statically assigned.

**The instrumentation.** `FOF3STAT execacct` reports, per process and
split by whether the unit was the process's own or borrowed: units, walk
time, edges kept, duplicates suppressed, certificate hits and misses,
particles walked on misses, and each short-circuit count. It is what
turned this from argument into measurement. One caution learned the hard
way: those counters are per PROCESSOR and folded into the process record
at the end of the stage. An earlier version used shared atomics on the
emit path, which runs 8.5 billion times in a 2B run, and took phaseB from
2.4 s to 13-58 s — the instrument cost more than the thing it measured.

## What the measurements settled

**phaseB is mostly waiting, not working.** Before the annotation the whole
stage did about 280 core-seconds over 1,920 processors: 0.15 s if
perfectly balanced, against a 2.3 s wall. After the annotation the work is
about 145 core-seconds. The wall is imbalance across processes, not
volume of work.

**Moving work between processes does not pay once the work is halved.**
With the annotation on, 1.58 s without movement against 1.72 s with it.
The machinery now costs more than it returns.

**Fewer, larger processes beat moving work.** Same 16 nodes: two processes
of 63 threads with no movement averaged 1.86 s, against 2.20 s for the
standing eight processes of 15 threads with movement. Threads inside a
process share the queue and ship nothing, so a bigger process removes the
problem instead of solving it. This was measured on phaseB alone; cache
duplication, decomposition and phase 3 all shift with the layout, so the
whole iteration has to be the judge before the standing configuration
changes.

**Shipping is cheap; the grant rate is what starves movement.**
Serialization is 2.15 ms per coarse unit and 0.025 to 0.062 ms per
divided unit. In one run 585,694 requests produced 146 grants and 12,023
refusals for an empty queue, and only 4 to 6 percent of the work ever
moved. A busy process is not unwilling to share: when a request arrives
its queue is already below the reserve of one unit per thread, because
its threads consume units as fast as refinement publishes them.

**Shipped units are not handled worse.** Per particle pair examined,
borrowed work is marginally CHEAPER than own work (0.027 against 0.034
us) and its certificate memo hits more often (96.3 against 93.1 percent).
The annotation helps both sides by the same factor. Whatever limits
movement, it is not that a unit executes badly away from home.

## Dead ends — do not repeat these

```
geometric split size (-Z, FOF_POOL_SPLIT_SIZE)
    24x more units at 2B with the largest unit unchanged: a small box in
    a dense core holds enormous work. Superseded by a particle-count
    threshold. Still in the tree, defaults off, wants deleting.

seed refinement, cost ordering, division on request
    All three implemented, all three cost time (section 15, 17). Division
    on request DID raise the grant rate 120-fold once the reserve was
    removed, but the units it manufactures are nearly empty: 24,464 of
    them carried under a microsecond of work each, because the answer
    takes whichever child was pushed last rather than the costliest.
    Default off. If revisited, hand over the costliest product.

the probe machinery (steal v4 to v7)
    probeRemaining, remainingReport, the buddy-tier monitor. Removed from
    the flow by d584de8 but the entry points are still on main, dead.

batching claims
    Held units in a processor's hand where no stealer could see them.
    Removed twice, at two different granularities.

same-fragment leaf skip in phaseB
    Fired 0 times out of 727 million. phaseB pairs are cross-thread by
    construction, so the two sides never share a fragment. The case is
    real in phase 3, not here.
```

## Open questions for a new design

1. **The remaining wall is cross-process imbalance.** After the
   annotation, phaseB is 1.58 s against roughly 0.08 s of perfectly
   balanced work. Nothing tried so far addresses this: movement is
   throttled by what is in the queue when a request arrives, and the
   queue is empty because threads consume as fast as refinement
   publishes. A design that decides placement BEFORE the stage — rather
   than repairing it during — is the obvious unexplored direction.
2. **Fuse the annotation into the freeze pass.** It currently re-reads
   every particle; phaseA already has a pass over every particle, the one
   component counting rides on. Fusing removes most of the 0.13 s
   measured at 8M, leaving only the O(nodes) accumulation. Not done
   because that path is hot and validated and the measurement came first.
3. **Whether the annotation belongs on `main-without-steal`.** It is
   independent of stealing and helps the plain within-process path most,
   so it probably does. Clean cherry-pick.
4. **The process-layout change**, judged on whole-iteration time rather
   than phaseB alone.

## Operational traps worth carrying forward

```
srun flags        Overriding -N and --cpus-per-task on the srun line, when
                  the allocation already sets them, inflated phaseA from
                  2.4 s to 20-47 s in EVERY arm while leaving phaseB alone
                  - a CPU binding effect hitting the memory-heavy first
                  pass. Use the allocation-level settings and a bare
                  "srun --mpi=pmi2 -n <procs> --export=ALL". Set the
                  layout per job, not per srun.

+pe not +ppn      On this reconverse build, +pe is the total across
                  processes. (+ppn is documented and has worked, but the
                  runs that used it were also the ones with the binding
                  problem above, so prefer +pe.)

header deps       The Makefiles have no header dependency tracking. After
                  any header change, "make clean" in examples/fof3.

worktrees         A fresh git worktree needs "git submodule update --init
                  utility" and must sit BESIDE ../unionfind and ../htram,
                  or the build cannot find them. /tmp will not do.

Anvil tree        Synced by rsync, not git. It has no commits. Rebuild
                  there after every sync; stage binaries under distinct
                  names so a pending job never has its binary swapped
                  underneath it.

destructive git   A "git reset --hard" in a cleanup path destroyed hours
                  of uncommitted work in the live checkout. Probe branches
                  belong in a throwaway worktree, and work belongs
                  committed before anything destructive runs nearby.
```

## Validation commands

Laptop, from `examples/fof3`, with
`CHARM_HOME=~/software/clusterFinding/charm/netlrts-darwin-arm8-smp`:

```
./charmrun ++local ./FoF3 -f ../../inputs/1m.tipsy -d oct -b 0.2 \
    -c full +p4 ++ppn 2                      -> 333889, TEST PASSED
./charmrun ++local ./FoF3 -f ../../inputs/8m-uniform.tipsy -d oct -b 0.2 \
    -c stats +p4 ++ppn 2                     -> 7865983
```

Add `FOF_POOL_2Q=1 FOF_STEAL=1` for the movement path, and
`FOF_STEAL_TEST=1` to force every unit of odd-numbered processes through
the ship, rebuild and return path. `FOF_STEAL_SELFTEST=1` compares each
unit's direct walk against the full serialize-rebuild-walk replica and
aborts on the first difference; it passed throughout and is the reason
the transport was ruled out early.

2B reference component count: 424,897,832.
