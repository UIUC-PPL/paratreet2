# Merging gpu-phase1: sequence and open decisions (2026-08-19)

State at writing: main at `241ddb4` (campaign merged `75ae684`, cleanup
done, engine contract `9faa067`, keep-alive gap meter `241ddb4`).
Ritvik's `gpu-phase1` at `a491d27`, forked from main at `f3d9bf2`
(2026-08-13) — after the fof/ extraction and the walk consolidation,
before the PE-set split and the cleanup. His runtime stack: charm +
reconverse `gpu-merge-candidate`, LCI fork `frontier-changes`
(retirement recommended, relay17/18). The stall investigation that
delayed this is closed: `design/campaign-archive/gpu-stalls-relay{16,17,18}.md`.

## A. Runtime layers first (they gate what the app is tested on)

A1. **LCI: retire the fork.** Upstream `b8069dd` runs the application
    5/5 exact at 2B/16 with indistinguishable timing (relay17 job
    5307495) and reproduces no defect the fork fixes. The fork's
    FI_THREAD_DOMAIN downgrade is a live contract violation
    (reproducer dies 8/8 in two-threads-per-domain shapes) masked only
    by reconverse-side locks. Owner: Ritvik. Effort: config change in
    his build scripts.

A2. **Reconverse: decide the fate of the two lock commits**
    (`7737b74` ofi_lock_mode = TRYLOCK_SEND|RECV|POLL, `cdec537`
    m_progress_locks + reentrancy guard) — the branch's only
    message-path delta (+67 lines, relay17 item 11). With upstream LCI
    (FI_THREAD_SAFE, provider-side thread safety) they are belt over
    braces: not the stall cause (relay18 headline 3), not measurably
    costly, possibly protective, never A/B'd. One 2B job — reconverse
    gpu-merge-candidate with and without the two commits, on upstream
    LCI — settles whether they go upstream or get dropped. Owner:
    Frontier session (job) + Kale/Ritvik (decision).

A3. **Charm/reconverse branches -> upstream.** charm
    `gpu-merge-candidate` is production + 53 commits with NO
    message-path changes (relay17 item 10: HAPI/GPU layer, LB
    framework, tests/docs); reconverse's is main + 22 (tests,
    device-RDMA, build, plus A2's locks). Normal PR path to
    charmplusplus once A2 is decided. Owner: Ritvik, Kale reviewing.

## B. App-side sequence (paratreet2)

B1. **Ritvik commits the untracked scripts** (`run_fof3_nosmt_8p.sbatch`,
    `run_fof3_scale_gpu.sbatch`) and the runbook fixes (KOKKOS_DIR
    hardcoded home x2, rocm module no-op -> CMAKE_PREFIX_PATH, -AMD
    build-dir note, Kokkos pinned to 4.x, clone list missing
    utility/unionfind/htram). These are merge content, not hygiene:
    reproducibility of the GPU arm is part of what main accepts.

B2. **Rebase `gpu-phase1` onto main.** The fork point predates the
    cleanup: any hunk conflicting with the deleted S3/shedding/
    reservation code is DROPPED, not restored (tag
    `campaign-2026-08-stealing` is the recovery point if ever needed).
    Expect fof/FoFPhase1.h to be the conflict surface; his device code
    is well-separated (fof/gpu/ is hipcc-only) and src/ is nearly
    untouched, so the rebase is tractable.

B3. **Engine-contract compliance** (`design/phase1-engine-contract.md`,
    the review standard for this merge). The GPU arm must, at
    runFoFPhase1 return:
    - leave `pieceSetTable()` in the sets=1 state on GPU processes
      (it covers ALL intra-process pairs; contract section 3 — the
      split machinery arrives with the rebase, his code has never seen
      it);
    - have labels COMPLETE — no device work or hapi callbacks in
      flight (contract section 4: cache copies are taken at ship time);
    - leave no Charm-level messages in flight (contract section 5, the
      QD clause — the natural GPU hazard).
    Owner: laptop session review of the rebased diff, then Frontier
    validation.

B4. **Gates.**
    - Laptop: full `make test` matrix + 1m -c full + split-active 10k
      on the CPU arm (GPU=0 build must be byte-equivalent behaviour to
      main), classic AND reconverse.
    - Frontier: 2B/16 GPU arm exact (424,897,832 / max_size
      185,317,566), devinit 0.000, device wall ~0.58 s, on the A1/A2
      runtime; plus one MIXED job if feasible (CPU processes with
      FOF_PE_SETS=14, GPU processes sets=1) — the contract's mixed
      case has never run at scale.
    - The keepalive_gaps line rides every arm free; compare within-job
      only.
    - -i 2 with iteration-1 readout for any device-arm timing claim
      (his stage-4 lesson: iteration 0 carries Kokkos/HIP init).

B5. **Merge** with --no-ff (same rationale as `75ae684`: one
    first-parent commit naming the outcome). GPU stays a build variant
    (GPU=1 + FOF_GPU_PHASE1 runtime arm), CPU default untouched.

## C. Launch-shape questions (asked by Kale, 2026-08-19)

**ppn 7 vs ppn 14, and SMT.** Facts first: Frontier low-noise mode
removes core 0 of each 8-core L3 group, leaving 56 usable physical
cores of 64. BOTH shapes use all 56 cores — ppn 7 x 8 processes = one
PE per physical core (no SMT), ppn 14 x 8 = two PEs per core (SMT2).
So "half the PEs", not "half the cores": no silicon idles either way.

- GPU arm, measured (relay17 item 9, one node count, 2v3 reps): ppn 7
  beats ppn 14 by 12.8% on Iteration 1 — uf2 -40%, phase-3 walk -20%,
  while phase1_device goes slightly the other way (0.599 -> 0.653 s).
  Coherent story: with phase 1 on the device, SMT's throughput benefit
  to the CPU chain evaporates, while its cost (two PEs sharing one
  core's issue width) still lands on the latency-sensitive uf2/walk.
  A hypothesis fitting one datapoint — check at 64/128 nodes before
  declaring (relay17 item 17).
- CPU arm: the entire campaign record (ppn 14, SMT) has NO direct ppn-7
  A/B. Do not change the CPU default on the GPU arm's evidence; if it
  matters, it is one cheap 2-arm job.
- Ritvik's stated reason for ppn 7: NOT recorded anywhere we have — the
  runbook just labels it "nosmt", and his 08-14 sweep used ppn 14, so
  he switched between 08-14 and 08-18. ASK HIM (it may encode a
  constraint we cannot see, e.g. host-thread staging contention with
  the GCD, or simply the same empirical finding).
- Mapping rule at any shape: every device needs a poller AND
  ndevices x processes/node <= ~56 (112 fails libfabric memory
  registration, relay17 arm D). Poller-count side effects are
  cosmetic only (relay18: gap rate per poller constant; all shapes
  equal speed).

**Recommended defaults today:** GPU arm ppn 7 / ndev 7 / poll 1 on
upstream LCI (one thread per domain — valid under ANY threading
contract, fewest projections artifacts among the fast shapes); CPU arm
unchanged (ppn 14 / ndev 7 / poll 2).

## D. Non-blocking follow-ups

- relay19 (in flight): whether the machine globally idles during the
  10-60 ms poller gaps — reconverse scheduler question either way,
  candidate upstream issue.
- ppn-7 GPU advantage at more node counts; CPU ppn A/B if wanted.
- The 08-14 GPU node sweep rerun at ppn 7 if the advantage holds.
