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
- The AUTO PE-set default (sets = PEs/process, 2026-08-20) at shapes
  other than ppn 14 — measured only at s=14/ppn 14; one sweep arm per
  new shape confirms or retunes it. Also the provisional
  FOF_PHASEB_SLICE_MS=2 default (Kale unsure, kept for now).
- The 08-14 GPU node sweep rerun at ppn 7 if the advantage holds.

## E. Execution record (2026-08-20)

Branch `gpu-phase1-rebase`, eleven rebased commits plus three new ones, on
top of main `7fa3531`. Worktrees and job logs under
`/lustre/orion/csc710/scratch/rrao/gpu-merge/`.

**A1 (LCI fork) — DONE.** The fork is deleted; the stack runs autofetched
upstream `uiuc-hpc/lci` at `ca88ce2c`. The three fork commits survive on
GitHub and in `~/lci-fork-archive/`, to be reopened as upstream PRs if
Frontier ever needs them (A3's tail).

**A2, A3 — NOT DONE.** Runtime-layer items, untouched here: the reconverse
lock A/B still wants its one 2B job, and neither charm nor reconverse has
been sent upstream.

**B1 — DONE.** The scripts were already committed at `f7375a3` (which also
carries `run_fof3_nosmt_8p_nd7.sbatch`, added 08-19). The four IN-REPO
runbook defects from relay16 section 6 are fixed: `KOKKOS_DIR` now defaults
to `$(HOME)/kokkos` in both makefiles, and the README pins the Kokkos clone
to 4.7.04, says to pass `-DCMAKE_PREFIX_PATH=/opt/rocm-6.2.4` because
`module load rocm/6.2.4` is a non-interactive no-op, and makes the `-amd`
suffix on `CHARM_HOME` unmissable. Defect 5 (the clone list) was already
covered by README's Prerequisites.

**B2 — DONE, and cheaper than expected.** `git rebase --onto main f3d9bf2`
hit exactly ONE textual conflict: `examples/fof3/Makefile`'s `DATA` list,
where both sides had independently added `FoFPhase1.h` (kept the branch's,
which carries the comment explaining the stale-instantiation trap it
fixed). Nothing had to be dropped — the branch never touched the S3,
shedding or reservation code the cleanup removed, so the fork point's age
cost nothing. `fof/FoFPhase1.h` auto-merged despite +849 on one side and
+1094 on the other: the two sets of edits are in disjoint regions.

Auto-merging is not compiling, and the tree did not compile. Two braced
initializations that main wrote in the `FOF_STEALA` claim path stopped
resolving, because gpu-phase1 gave `TreePieceRef` default member
initializers and an NSDMI disqualifies a class from being an aggregate
under C++11 — which is what charmc compiles (`-std=gnu++11`). `libfof.a`
still built; the chares are templates, so only the application instantiates
them. Repaired at `69a1be2`, which also carries the device tree through a
claim: both rewritten structures had been dropping `dnodes`, so
`FOF_STEALA=1` plus any device arm would have ended in deviceLaunch's "no
flat device tree" abort.

**B3 — DONE; one contract gap found and closed.** Sections 1, 2, 4 and 5
hold as written. Section 4 holds structurally: the `dev_done_cb_` reduction
that releases the driver's `CkCallbackResumeThread` is contributed by every
PE only after `deviceAdopt`, so labels are complete and nothing is in
flight. Section 5 holds because the keep-alive ring is `CcdCallFnAfter` —
raw Converse, invisible to QD.

Section 3 held only by omission, which is the gap. `pieceSetTable()` is
written by the CPU chain's pool build, and Replace mode skips that chain
entirely; with `FOF_PE_SETS` unset `peSetKeepLocalPair` returns before it
ever reads the table, so the standing configuration is fine by accident.
Configure the split on a GPU process and the table stays EMPTY, the
"don't know -> walk it" fallback vetoes the ownership prune for every local
pair, and phase 3 silently loses its prune while still producing the RIGHT
count. `21e9aaa` refuses that combination in `deviceStage0` (a broadcast,
so the check is per-process) and names `FOF_PE_SETS_NODES` as the fix,
which keeps the contract's mixed case well-formed.

**An unrelated main-side bug rides in with the rebase.** `TreePiece::reset()`
on main clears `particles` unconditionally, but `buildTree()` takes its
next input by swapping in `incoming_particles`, which nothing refills for an
app that does not move particles. Reproduced on main at `7fa3531`, fof3
10k `-c stats -i 3`: iteration 0 reports 3549 components, iterations 1 and 2
report **0**, in 0.8 ms, with no error. The same command on the rebased tree
reports 3549 three times. Every multi-iteration measurement taken on main
since the fork was measuring an empty tree — including, had it been run, B4's
own "-i 2 with iteration-1 readout" rule.

**B4 — laptop and single-node rows DONE.**

- CPU matrix, reconverse, job 5314421 (25 s): all 16 runs `FOF3 TEST
  PASSED`, plus 100k `-c full` (33933 components, O(n^2) crosscheck agreeing)
  and split-active 10k `FOF_PE_SETS=2` at exactly the 3549 gold with the
  split engaging (piece_pairs_dropped 2112/2130). Zero nonzero exits. Run
  under `srun --network=single_node_vni` rather than `charmrun ++local`:
  the reconverse/LCI transport needs a VNI even for two processes on one
  node.
- Device arm, 1 node x 8 proc x 7 PE, job 5314453 (32 s): `FOF_GPU_VERIFY`
  exit 0 — and verify mode aborts on any per-particle disagreement, so that
  is an exact match against the CPU labeling, not a matching count.
  `FOF_GPU_PHASE1` gave 33933 / max_size 26042 / identical log2 histogram,
  three-way exact with the verify arm and with the CPU-only gate. The
  section-3 guard aborts as designed on `FOF_PE_SETS=2`.
- GPU=0 is NOT byte-equivalent to main, deliberately: the `reset()` fix
  above changes `-i > 1` behaviour, and the branch also hardens
  `Makefile.common` so `PROJECTIONS=0` means off rather than on.
- 2B / 16 nodes, device arm, job 5314471 (33 s wall): **424,897,832
  components, max_size 185,317,566** — the gold pair, reproduced on top of
  main's PE-set split and cleanup, which this code had never seen before
  the rebase. `phase1_devinit init 0.000`, `phase1_device wall 0.582 s`,
  Iteration 0 4290.5 ms (5307458 was 4303.9, 5310026 was 4239.2 — inside
  the spread of the two pre-rebase jobs at this shape). exit 0.
- NOT DONE: the classic-charm arm (no classic build on this machine), the
  mixed CPU/GPU job, and `-i 2` iteration-1 timing readouts.

**B5 — DONE.** `60e67ea`, `--no-ff` into main, first-parent. The merged
tree differs from the gated branch tip by this file alone, so what is on
main is what the gates above ran against. Merged with two of the plan's own
gates unrun (classic-charm arm, mixed job), which the merge commit says in
as many words rather than leaving to be rediscovered. Not pushed:
`origin/main` is 16 behind.

**C (launch shape).** The plan's recommended GPU default — ppn 7 / ndev 7 /
poll 1 — was measured at 16 nodes on 08-19, job 5310026 against 5307458
(ndev 4 + poll stride 2), same shape otherwise: iteration 0 4239.2 vs
4303.9 ms untraced, tree traversal 2180 vs 2261 ms, `phase1_device` flat to
the millisecond, components bit-identical. Every pair moves the opposite way
on the traced arm, which is the noise signature. So the two configurations
are interchangeable here, and ndev 7 is the one that is valid under any
threading contract — but note it is only reachable with the poll stride
UNSET: `+ppn 7 +lci_ndevices 7 +backend_poll_thread 2` is one thread per
device with the odd ranks silenced, which is the silent 8-minute init hang
of job 5302111. The "ask him why ppn 7" item is still open.
