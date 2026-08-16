# migrateMe bisecting test (2026-08-15)

Written to answer "when does app-directed migration work and when does it
not", after forced shedding segfaulted in paratreet2 with the stack in
`CkArrayBroadcaster::attemptDelivery`.

Charm++ already ships one migration test — `tests/charm++/anytime_migration`
— but it only ever migrates from a POINT-TO-POINT entry method, which is
exactly the case that was not in doubt.

This one runs four modes over the same array (`./mig <nElements> <mode>`):

| mode | migration is issued from | result on 4 PEs |
|---|---|---|
| 0 | a point-to-point entry method (what the charm test does) | PASS |
| 1 | **inside a broadcast entry method** | **PASS** |
| 2 | a broadcast that defers via a self-send | PASS |
| 3 | broadcast decides -> reduction -> migrate from the reduction target | PASS |

Each mode migrates every element, waits for quiescence, then broadcasts a
liveness check and counts answers by reduction; PASS means all N answered.

## What this rules out

**Migrating from inside a broadcast is NOT inherently unsafe in Charm++.**
That was my hypothesis for the paratreet2 crash and it is wrong. All four
shapes work on a plain array.

So the paratreet2 failure is specific to `TreePiece`, not to the
mechanism. Candidates, in the order I would test them:
1. **The custom array map.** TreePieces are created with the
   decomposition's `setArrayOpts` map; this test uses the default. A map
   consulted during delivery after an element has moved is the closest
   structural difference.
2. **The partial pup.** `TreePiece::pup` ships `incoming_particles`,
   proxies, `tp_key` and `load`, leaving `local_root`, `leaves`,
   `particles`, `traverser` and `r_local` default-constructed on arrival.
   The AtSync path survives this, but it migrates at a different point in
   the lifecycle.
3. **`usesAtSync` is set in the normal constructor and NOT in the
   `CkMigrateMessage` constructor**, so a migrated TreePiece has it false.
   Not an obvious segfault, but it is a real inconsistency.

Build: `charmc mig.ci && charmc -c mig.C && charmc -language charm++ -o mig mig.o`

## usesAtSync / the CkMigrateMessage constructor: NOT a bug (2026-08-16)

Candidate 3 above — "`usesAtSync` is set in the normal constructor and NOT
in the `CkMigrateMessage` constructor, so a migrated element has it
false" — is WRONG, and worth writing down because it looks like a bug and
would have cost a test program and an upstream issue.

The flag is restored by the pup, and the framework reaches that pup
automatically:
- `CkMigratable::pup` pups it (`cklocation.C:1746`: `p | usesAtSync;`,
  alongside `thisIndexMax`, `can_reset`, `usesAutoMeasure`).
- Migration calls `elt->virtual_pup(p)` (`cklocation.C:1650/1673/2652/
  2897`), which is `recursive_pup<TreePiece<Data>>(this, p)`.
- `recursive_pup_impl<T,true>` (`charm++.h:430`) does, in order:
      obj->parent_pup(p);   // walks UP the inheritance chain
      obj->_sdag_pup(p);
      obj->T::pup(p);       // the user's pup, last
  `parent_pup` chains through `ArrayElementT` to `CkMigratable::pup`.

So base state is pupped by the framework BEFORE the user's pup, and the
constructor leaving `usesAtSync` unset is harmless — it is overwritten
microseconds later. charm++.h:451 even documents the design: "CBaseX::pup
must be an empty override, so that the recursive PUPing doesn't call an
implementation multiple times up the inheritance hierarchy" — which is
why `CBase_TreePiece::pup` is `{ }` and why a user class must NOT call
its parent's pup.

This also disposes of candidate 2 (the partial pup), for the same reason
Kale expected: the members `TreePiece::pup` omits are either framework
state (handled above) or app state that `buildTree` rebuilds from
`incoming_particles` anyway.

**So the crash points squarely at candidate 1: the custom `CkArrayMap`.**
That is the one structural difference left between paratreet2 and the
four-mode test, which uses the default map and passes all four modes.

## Bisect log (2026-08-16) — what is RULED OUT

Kale's correction retired my leading hypothesis before it cost a run:
`procNum` is the STATIC answer, used to populate the array and to find an
element's HOME (`CkArrayMap::homePe` defaults to it, cklocation.h:143/505;
`CKARRAYMAP_POPULATE_INITIAL` at cklocation.C:277). The DYNAMIC location is
always the location manager's. So a custom map cannot hand the broadcaster
a stale local element, and swapping paratreet2 to the default map would
have rearranged every piece — destroying locality and changing the whole
run — while isolating nothing.

Ruled out by direct experiment in this test (all four modes PASS in every
configuration below):

| difference from paratreet2 | tested | result |
|---|---|---|
| migration issued from inside a broadcast | modes 1-3 | PASS |
| SMP (2 processes x 4 PEs, matching the failing run) | `+p8 ++ppn 4` | PASS |
| element sets `usesAtSync` | `MIG_ATSYNC=1` | PASS |

Ruled out by reading the runtime, not by experiment:
- the `CkMigrateMessage` constructor not setting `usesAtSync` (recursive_pup
  restores it — see the section above);
- the partial `TreePiece::pup` (framework state is pupped by `parent_pup`;
  the omitted app state is rebuilt by `buildTree`).

STILL UNTESTED, and now the only structural difference left:
1. a CUSTOM ARRAY MAP on the array being migrated — cheap to add to this
   test, and the natural next step;
2. paratreet2's other in-flight state at that moment: the TreeCanopy
   registration each TreePiece performs in its constructor
   (`buildCanopy` -> a `[createhere]` entry on another array), and the
   Reader group's outstanding flush acks.

The symptom to reproduce: segfault on a PE of the SOURCE process inside
`CkArrayBroadcaster::attemptDelivery` <- `CkArray::recvBroadcast`, with
k=1 (a single migration), on a PE OTHER than the one whose element moved.
