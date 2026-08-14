# Frontier instructions: parallelize the helper-side rebuild (s3Shipment)

2026-08-14. Context: design/phaseab-balancing.md sections 31-33 and the
timeline images (timelinesPodwire08-13: helper P751 runs one ~150 ms
s3Shipment block while 13 siblings idle; the drainForeign wall starts
only after it ends). Kale's directive: move tree construction inside
the parallel segments on all PEs.

## Prerequisite (do first, tiny): trustworthy helper timers

Helper-side counters print at the helper's OWN merge, which can precede
its helping — only ~23 of ~300 shipments counted (relay1 items 20/16).
Move the helper-side s3 stat print to phase3Stats. Without this the
experiment cannot be read.

## The change

1. s3Shipment becomes THIN: take ownership of the deserialized
   StealShipment (move its vectors onto a heap-owned rec —
   const_cast-and-move from the marshalled param is acceptable since
   the generated wrapper destroys it after return; a raw/message-type
   entry is the cleaner alternative if you prefer), create the
   S3ForeignShip with a per-tree atomic state array
   (raw/building/ready) and per-tree arena slots, then wake every PE.
   BUILD NOTHING in the entry.
2. drainForeign gains a PARALLEL BUILD PRE-PASS: an atomic tree-build
   cursor; each arriving PE claims trees and runs the linear arena
   placement-new pass per tree, until the cursor exhausts; then spins
   briefly on built_count == n_trees (the last tree finishes ~ms later)
   and proceeds to the existing unit drain. Rationale: build total is
   ~100-150 ms today, /14 PEs ~ 10 ms each — the pre-pass barrier
   costs at most one tree's build time. FALLBACK if the barrier shows
   in traces: lazy per-unit build (claim unit -> CAS-build its two
   trees; on 'building' by another PE, move to the next unit) — more
   complex, only marginally better.
3. Keep: per-unit CAS ownership, the owned-units + returns termination
   ledger, the completion counter firing s3FinishForeign from the last
   drainer (now also the arena/rec lifetime holder), forced mode,
   loopback. The one-outstanding-shipment-per-helper protocol is OUT
   OF SCOPE here (section 26 lever 3 is a separate change) — but note
   that shrinking s3Shipment already shortens the turnaround that
   serialization multiplies.

## Gates (per your own vacuous-validation lesson — state what each
## scale executes)

- 10k forced 2-node (ships whole pools THROUGH the new path) — quick
  smoke, exercises parallel build at small tree counts.
- 80M forced (real tree sizes through the path) + 80M natural.
- 2B best-cell natural pair + one forced. All exact, all runs.
- Loopback validates the WIRE only, not this change — run it once to
  confirm no wire regression, and say so in the report.

## Measurement (the point)

Best cell (PARTS=16, defaults now GRANT_M2=1e11 in code), A/B against
the unmodified tip in the SAME allocation, 2 reps each + one
projections subset job (+traceprocessors on the straggler block + one
helper block, as before):
- s3Shipment entry duration: expect ~150 ms -> ~10-15 ms; the
  timeline should show the green block collapse and the drain wall
  start immediately.
- Helper idle gap (own-phaseB-end -> drain-wall-start) and grants per
  helper: turnaround = rebuild + drain + return, so both should move.
- phaseB_s max / Pre-traversal / Iteration 0 vs the POD-wire baseline
  (1.229-1.322 / 3.596-3.744 / 6.278-6.416).
- IF the improvement is real, rerun the packing knobs
  (FOF_S3_DENSITY=0.25, SPAN_PARTS=4, and the composed cell) — relay1
  section 14/15: those verdicts hold only at the old per-grant cost.

## Logistics

- FIRST: git checkout -- fof/ (your uncommitted patch 0005 state) &&
  git pull — land on c65f735 or later. The vetted upstream version of
  your patch DIFFERS from your local copy: the laptop's static_assert
  caught OrientedBox's inherited virtual dtor putting a vptr inside
  FragData, so WireSpatial now mirrors the pupped fields (box corners
  + three longs) instead of embedding Data. Rebuild + re-gate before
  layering this change.
- Relay: patch (git diff against the pulled tip) + report to
  ~/software/reports/, and include scripts/projlog_tool.py in the
  relay batch so the laptop can add it to the sumdetail-analysis skill.
