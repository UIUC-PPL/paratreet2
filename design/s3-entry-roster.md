# S3 entry-method roster

The complete set of entry methods behind S3 phaseB stealing
(design/phaseab-balancing.md sections 19-31), grouped by role. Written
2026-08-13 for trace reading (Projections EP names match these).
Declarations: fof/fof.ci lines 58-67 and 90; handlers in
fof/FoFPhase1.h.

Topology reminder: one COORDINATOR per physical-node block (the
lowest-ranked process, s3Coord()); every process is a MEMBER; a member
is a DONOR when work is taken from it and a HELPER when it executes
someone else's work. FOF_PROCS_PER_PNODE sets the block width.

## Control plane (cheap, coordinator-centered)

| entry | runs on | what it does |
|---|---|---|
| `s3Publish(fromNode, total, part_costs)` | coordinator | member announces its armed stage-0 pool at phaseB start: total m2 and per-partition costs. Feeds the coordinator's donor table. |
| `s3Poll()` | member | self-repeating 10 ms tick (FOF_S3_POLL_MS) while armed; sends s3Report. |
| `s3Report(fromNode, remaining)` | coordinator | member's CURRENT remaining m2 (one atomic subtract per claim keeps it live). Lets orders chase the actual straggler, not initial cost. Also where the (now default-off) section-27 RESERVE trigger fires. |
| `s3Drained(fromNode)` | coordinator | helper finished its shipment — "next grant, please". Sent by the last drainer (s3FinishForeign). One shipment in flight per helper (v1/v2 protocol); breaking this serialization is section 26 lever 3. |
| `s3Declined(donorNode, helperNode, partIdx)` | coordinator | donor's reply when the ordered partition had nothing unclaimed. The coordinator retires or re-orders per its in-flight bookkeeping. |

## Data plane (where the time goes; section 31 numbers from the
5250364 sum-detail, GRANT=32 generation)

| entry | runs on | what it does | measured |
|---|---|---|---|
| `s3ShipOrder(helperNode, partIdx)` | DONOR | The collect-and-send: scan the partition range (reserve window first when active), CAS-claim unclaimed units, FLATTEN the deduplicated subtrees (the serialization), send one s3Shipment to the helper. Runs ON the donor — critical-path time on the hottest process. | 29.9 s total; 4.87 s on straggler proc 55 (20% of its exec); ~65 ms per shipped grant (6.5 ms per order call; 4,569 order calls served 473 ships) |
| `s3Shipment(StealShipment)` | HELPER (one PE) | Receive the grant: rebuild each shipped tree, enqueue units to the process-wide foreign queue, wake every PE (drainForeign self-sends). Was per-node malloc + recursion — 59 ms mean / 423 ms max serial head, dominating shipment makespan; replaced at b797e73 by offset wire + one-arena linear rebuild (Kale's design). | 27.8 s / 473 calls (pre-b797e73) |
| `drainForeign()` | HELPER (all PEs) | Execute stolen units from the foreign queue (atomic cursor, sliced loop) into per-origin edge buffers; last finisher runs s3FinishForeign (dedup edges, send s3Return, send s3Drained). Fans out well: 13.4/14 PEs active. | 55.9 s total (16% of phaseB exec) |
| `s3Return(fromNode, edges)` | DONOR | Accept the results home: deduped edge list re-injected through submitEdges (merge is idempotent to duplicates); decrement ships-outstanding for the owned-units + returns termination ledger. Tiny — invisible in any overview. | 0.17 s total (~0.4 ms/shipment) |

## Dormant

| entry | runs on | what it does |
|---|---|---|
| `s3Reserve()` | donor | Section-27 ship-only window over the costliest-first pool prefix. Default OFF since 1996ebd (section 30: composition inversion); FOF_S3_RESERVE=1 re-enables, FACTOR=0 = every member reserves (correctness-gate config). |

Not an entry method but part of the plane: `s3FinishForeign` /
`s3TryFinishB` (plain calls) close out a shipment and the phase;
`selftestShippedUnit` (FOF_S3_LOOPBACK=1) replays every local unit
through flatten->pup->rebuild->walk against the direct walk — the
transport's strongest gate.
