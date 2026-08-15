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
