# Frontier task: 16-node sanity checks on the post-campaign cleanup (2026-08-24)

**What changed.** The campaign is closed (`design/campaign-report-2026-08.md`).
A hygiene pass followed — read `design/debris-audit-2026-08.md` for the full
dispositions. Code-relevant summary:

- unionfind `7794336` (master): compression wave compile-gated under
  `CONCURRENT_COMPRESSION_WAVE` (default OFF — `unionFindVertex` shrinks
  120 → 80 B); UFSTAT branch census and climb-hop histogram DELETED.
  NOTE: this sits ON TOP of Ritvik's 08-23 width/iterator fixes
  (00397f3..5e94077 — the >2^31 `arrIdx` truncation behind the 24B
  under-merge, and the lazy_store mutation-during-iteration skips), so
  these sanity runs cover his fixes and the cleanup together. His
  `wave_pass` iterator fix is preserved inside the wave gate.
- paratreet2 `2b55923` (main): dead dense tip-enumeration path removed
  (`computeTipEncoding`/`encode_map`/`uf2_vertices`), `verifyTips`/
  `phaseBWalker` removed, wave glue gated, debug-only 2^20-process
  `CkAssert` added in fof3 Main.C.
- Rollback tag `pre-cleanup-2026-08` in both repos.

None of this should change any computed value or any default-build timing,
except: (a) 40 B/vertex less union-find memory, (b) removal of the
always-on census/histogram increments from the find_boss hot path. That is
what these runs check.

**Operational notes for your harness:**
- `FOF_WAVE` / `FOF_WAVE_MS` are now INERT unless both libs are built with
  `-DCONCURRENT_COMPRESSION_WAVE` — do not schedule wave arms.
- `[UFSTAT]` lines no longer print; retire any parsing that expects them.
- `FOF_UF_SIZES`, `FOF_UF_LOCALCOMP`, `FOF_UF_SHORTCIRCUIT`, `-u gather`,
  `-s`, `-D`, `-E`, `FOF_PE_SETS` all unchanged.

## Runs (2B, 16 nodes, one job if walltime allows)

Pull both repos (paratreet2 main, unionfind master), clean-rebuild the full
stack with your established build, print binary md5s as usual.

1. **CPU exactness + wall band** — recommended defaults (README config:
   `-u dist`, leaf 32, `FOF_PE_SETS` AUTO, `FOF_UF_SIZES=0`), 2+ reps.
   Gate: exact gold — **424,897,832 components / max_size 185,317,566** —
   and wall within your most recent 16-node within-allocation band (quote
   the reference job id; cross-allocation spread is 2–4%, so treat wall as
   advisory unless it moves well outside that).
2. **GPU arm** — same scale, your standard GPU configuration (affinity fix
   on, `-s` per relay106 practice). Same exactness gate; wall advisory.
   This is the arm Ritvik's 24B/56B runs ride on, so a pass here clears
   the cleanup for his campaign.
3. **Compile check only (no run): wave-on build** — rebuild unionfind with
   `WAVE=-DCONCURRENT_COMPRESSION_WAVE` and paratreet2 fof with the same
   define, confirm the stack links on Frontier's toolchain, then rebuild
   default-off. Keeps the explicit-graph study path from rotting.

Report per protocol (`~/software/reports/sanity-cleanup.md`): commit SHAs,
job ids, exact srun lines, per-arm components/max_size, walls with the
within-job reference. Nothing here needs 128 nodes; if anything is off,
STOP and report — the rollback comparison point is tag `pre-cleanup-2026-08`.
