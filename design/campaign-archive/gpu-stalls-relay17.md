# relay17 — the LCI-layer swap on Ritvik's stack, and what actually separates
# his runtime from ours

Frontier, 2026-08-19. Job 5307495 (16 nodes, 2B, GPU replace arm).
Stack in `~/software/gpu-stalls/`. Nothing pushed; no file in any of
Ritvik's repositories changed.

---

## 0. The headline, and it came from the login node

**The relay16 gap is closed, and the answer is not in the LCI fork.**

relay16 ended on a contradiction it could not resolve: a pure-LCI
reproducer, relinked against Ritvik's `liblci`, dies 8 times out of 8 in
his thread/device shape — and his application, on the same library in the
same shape, ran 23 times clean. Item 12a said "both results are solid, so
the gap is the finding" and left it open.

The gap is this, and it is one line of his own code:

    // reconverse/src/comm_backend/lci2/comm_backend_lci2.cpp, commit 7737b74
    // ("fixes to run on frontier", Ritvik Rao, 2026-05-05)
    //
    // SMP mode has multiple PE threads per node sharing one device. The CXI
    // provider is not thread-safe without FI_THREAD_SAFE, so enable LCI's
    // built-in per-device trylock for all OFI operations.
    g_attr.ofi_lock_mode = lci::LCI_NET_TRYLOCK_SEND |
                           lci::LCI_NET_TRYLOCK_RECV |
                           lci::LCI_NET_TRYLOCK_POLL;

LCI's default for `ofi_lock_mode` is `0` — no lock at all
(`lci-up-src/src/global/global.cpp:104`). The relay14/16 reproducer never
sets the attribute, so **the reproducer ran unlocked**. The application,
through reconverse, runs with a per-device spinlock taken around every
send, every recv and every poll (`backend_ofi.hpp:37`,
`if (ofi_lock_mode & mode && !lock.try_lock()) return ret;`), plus a
second per-device trylock of reconverse's own around `lci::progress_x()`.

So the two results were never in conflict. They measured two different
things:

| | thread/domain | serialised? | outcome |
|---|---|---|---|
| relay14/16 reproducer | 2 | **no** (`ofi_lock_mode` unset) | fails 8/8 |
| the application | 2 | **yes** (send+recv+poll) | 23/23 clean |

**This changes what relay16 recommends.** The fork's `FI_THREAD_DOMAIN`
downgrade is still a real contract violation, and it is still worth
fixing — but the application is already carrying the serialisation the
contract demands, so `+lci_ndevices 7 +backend_poll_thread 1` is a
belt-and-braces measure, not the fix for a live application bug. Relay16
item 13 said "run with" it; the honest version is "it costs nothing and
closes a gap that is currently closed by a different mechanism."

What the fork still leaves uncovered is memory registration, and the
fork's own next commit (19fe6b8) added a mutex for exactly that. Between
`ofi_lock_mode` and that mutex, essentially every `fi_*` call the
application makes on a shared domain is serialised.

*(Section 3 reports what the swap job measured on top of this.)*

---

## 1. What was built

One layer swapped, everything else held fixed.

| | arm `fork` | arm `up` |
|---|---|---|
| paratreet2 | a491d27 (gpu-phase1) + 241ddb4 as a local edit | same tree, same edit |
| charm | 9af1de4b6 gpu-merge-candidate | same commit |
| reconverse | 397864f gpu-merge-candidate | same commit |
| Kokkos | 4.7.04 | same |
| **LCI** | **ritvikrao/lci 4ee0d15 (frontier-changes)** | **b8069dd — the fork's own merge base** |

`git diff b8069dd 4ee0d15` is **3 files, +103 −20**:
`src/core/progress.cpp`, `src/network/ofi/backend_ofi.cpp`,
`src/network/ofi/backend_ofi.hpp`. That is the entire difference between
the two arms.

Layout: upstream source is a `git worktree` off the same clone (so there
is no chance of a different upstream), installed to
`gpu-stalls/lci-up`; a second charm tree `gpu-stalls/charm-up` at the
identical commits built against it; the application built twice from one
source tree into `FoF3.fork` and `FoF3.up`.

### 1.1 The RUNPATH gate, both directions

relay16 trap 6a was that the first link of the reproducer silently
resolved to the production `liblci` because RUNPATH loses to
`LD_LIBRARY_PATH`. That trap is live here and worse: the login shell puts
`$HOME/software/charm/lib` on `LD_LIBRARY_PATH`, and that directory holds
**both** a `liblci.so` and a `libreconverse.so` from the production
stack. Left alone, every arm of this job would have run production and
"cleared" the fork.

The job therefore strips every `$HOME/software/` entry from
`LD_LIBRARY_PATH` and rebuilds it per arm, then gates on **content, not
path**:

    strings liblci.so | grep -c FI_THREAD_DOMAIN   ->  fork = 2, upstream = 0

An arm whose resolved `liblci` does not have the expected count is
skipped rather than run. Both `libreconverse.so` files also carry a
RUNPATH to their own LCI, so the two trees are self-identifying.

### 1.2 The instrument

main's `241ddb4` is applied to the gpu-stalls checkout as an **uncommitted
local edit**. `fof/FoF.C` applies clean. The `fof/FoFPhase1.h` hunk does
not: main places the call after `printLoadModel` / `printPhaseAStages`,
and the gpu-phase1 branch has neither. It is hand-placed at the head of
`phase3Stats` instead. `FoFPhase1` is a **group**, so `CkMyRank() == 0`
gives one line per process — the same cardinality main gets. The
declaration goes inside `namespace paratreet` (lines 91–369 of that
header; the `FoFPhase1` class itself is outside it, which is why the call
site says `paratreet::`).

Ring period is the 100 ms default on every arm, so the arms stay
comparable and the instrument perturbs all of them equally.

---

## 2. The login-node question: how much of relay13/14 transfers?

relay13/14 were measured on the **production** stack: charm `3d1fdd89f`,
reconverse `a1207a8` (= reconverse main), upstream LCI. Ritvik's stack is
charm `9af1de4b6`, reconverse `397864f`, LCI fork. Both diffs were
classified line by line.

### 2.1 charm: nothing on the message path

Production charm is an **ancestor** of his, so the comparison is exact:
53 non-merge commits, +9664 −1789 over 153 files. By destination:

| lines | bucket | reaches FoF3? |
|---|---|---|
| +2609 | examples, tests, docs | no |
| +2606 | load balancing (`src/ck-ldb`) | framework yes, no strategy on the FoF3 command line |
| +1512 | HAPI / GPU device layer (`src/arch/cuda/hybridAPI`) | **yes** — this is the GPU arm's device layer |
| +979 | other machine layers (`src/arch/{mpi,ucx,netlrts,util,common}`) | **no** — a reconverse build has no `libconverse`; verified, the built tree has no `libconverse*` |
| +795 | checkpoint / shrink-expand / CCS | no |
| +588 | device-RDMA + PUP (`ckrdmadevice`, `conv-rdma*`, `pup*`) | no — the app never calls charm's device-RDMA API |
| +234 | build machinery (CMake, `charmc`, Dockerfile) | build only |
| +341 | remainder in `src/ck-core` | see below |

The `src/ck-core` remainder is `cklocation.C` (+228: GPU load-balancing
instrumentation — `setObjGPUTime`, GPU PUP size — plus a location-cache
epoch field used only on migration), `init.C` (+17: `CMK_CUDA` →
`CMK_CUDA || CMK_HIP` guards, a shrink-expand handler registration, an
`_inrestart` → `get_in_restart()` accessor rename), `ck.C` (+14: the same
HIP guards and commented-out debug prints), and one-line changes in
`ckcallback.C` (whitespace) and `ckreduction.C` (the same accessor
rename).

**No charm change between the two stacks touches the LCI message path.**

### 2.2 reconverse: exactly one message-path change that matters

His reconverse is main + 22 commits, +543 −38 over 20 files, and his
branch is a strict superset of main (0 commits on main are missing from
it). By destination:

| lines | bucket |
|---|---|
| +322 | tests (`rdma_pingpong_device`) — not built into FoF3 |
| +94 | device-RDMA: `deviceRdmaOpInfo`, `memcpyAnyPtr` (a HIP/CUDA-pointer-aware loopback copy) |
| **+67** | **`comm_backend/lci2` — the LCI message path** |
| +32 | build machinery |
| +28 | the rest: commented-out debug prints in `convcore.cpp`, a listener-free leak fix in `threads.cpp`, two `converse.h` defines, an `issueRget` signature refactor (`remote_disp` → `remote_buf`, disp computed inside; behaviour-preserving for host buffers, and it is on the live Rget path) |

The +67 is three things, all from `7737b74` and `cdec537`:

1. **`ofi_lock_mode = TRYLOCK_SEND | TRYLOCK_RECV | TRYLOCK_POLL`** —
   section 0.
2. **`m_progress_locks`**, one atomic per device, so at most one thread
   polls each device CQ at a time; the loser of the race returns without
   progressing.
3. **A reentrancy guard.** `g_in_progress_callback` is set while a thread
   is inside `lci::progress_x()`. `issueAm`/`issueRget` read it and pass
   `allow_retry(false)` when it is set, pushing to LCI's backlog instead
   of spinning — because spinning inside a completion callback would hold
   the progress lock indefinitely and deadlock the TX drain. The commit
   that added it is named "attempt to avoid deadlock".

### 2.3 The transfer verdict

**Relay13's and relay14's conclusions transfer, with exactly one caveat,
and the caveat is in the progress path.**

- relay14 ("the LCI idle-stall does not exist on Slingshot/CXI —
  220,000 round trips, 0 samples over 1 ms") was measured with a pure-LCI
  harness against upstream LCI. The fork does not change polling cadence;
  its third commit adds a null-context guard in `progress_read`. That
  result carries over to his LCI. This job re-tests it directly on both
  LCI arms with the `keepalive_gaps` counters.
- relay13 ("the keep-alive ring is not what holds the stall off") was
  measured on the production **application**. Its runtime differs from his
  in one way that matters: **production reconverse takes no lock at all in
  the progress path; his takes two.** On production every thread polls its
  device freely. On his, a thread inside a send contends with the
  device's poller for one spinlock, and a second thread wanting to poll
  the same device is turned away outright.
- Everything else — HAPI, load balancing, checkpoint, device RDMA, the
  other machine layers — is either not compiled into FoF3, not called by
  it, or on the GPU side rather than the network side.

So the honest one-line answer: **relay13/14 transfer except in the
progress path, where his runtime serialises and ours does not.**

---

## 3. The swap, measured — job 5307495

16 nodes, 2B (`cosmo25cmb.768g2_dm.001024`), GPU replace arm, `-i 2`,
`-c stats -l 128`, his documented sweep line
(design/fof3-2b-scaling.md §7). Started 12:46:47, done 12:58:54, COMPLETED.
Arms interleaved B,A,C,D per rep, with B (upstream) first so a dead swap
would be known in minutes.

### 3.1 The answer: both LCIs are clean, and upstream needs no fork

    A  fork      ppn 14 / ndev 7  / poll 2   3 of 3 EXACT
    B  upstream  ppn 14 / ndev 7  / poll 2   3 of 3 EXACT
    C  upstream  ppn  7 / ndev 4  / poll 2   2 of 2 EXACT
    D  fork      ppn 14 / ndev 14 / poll 1   0 of 2  -- see 3.4

424,897,832 components and max_size 185,317,566 on all eight completed
arms. No hang, no crash, no corruption signature on any of them.

**Upstream LCI at `b8069dd` runs the application correctly on Frontier
Slingshot/CXI.** The fork commit it drops is titled "changes to work on
frontier"; five arms say the application does not need it. That is the
result the swap was built to get, and it is the strongest statement in
this report that rests on a measurement rather than on code reading.

**Fork versus upstream, in his own sweep shape, shows no stall
difference.** Which is what section 0 predicts: the application is
protected by reconverse's `ofi_lock_mode`, so removing the fork's
threading downgrade changes nothing it was doing.

### 3.2 The qd-stall meter says busy sender, not delayed delivery

`keepalive_gaps`, aggregated over the **final** print per process (the
counters are cumulative and `-i 2` prints them twice, so a naive sum
double-counts — the numbers below use the last print of each of the 128
processes):

| arm rep | ARR over25 / over100 | SEND over25 / over100 | worst arrival | that node's own send max |
|---|---|---|---|---|
| B-up r1 | 2104 / 1133 | 2030 / 1034 | 14149.1 ms @ 15.7 s | 11991.5 ms |
| B-up r2 | 2013 / 1105 | 1935 / 1024 | 1555.4 ms @ 5.8 s | 1229.9 ms |
| B-up r3 | 2037 / 1102 | 1949 / 1017 | 1571.9 ms @ 5.8 s | 1190.4 ms |
| A-fork r1 | 2034 / 1082 | 1978 / 1010 | 1424.5 ms @ 5.6 s | 1244.8 ms |
| A-fork r2 | 2025 / 1160 | 1971 / 1074 | 1490.6 ms @ 6.0 s | 1232.6 ms |
| A-fork r3 | 1943 / 1150 | 1882 / 1059 | 1533.8 ms @ 6.0 s | 1236.5 ms |
| C-up r1 | 2179 / 1081 | 2130 / 994 | 2485.1 ms @ 3.2 s | 2484.8 ms |
| C-up r2 | 2093 / 1197 | 2064 / 1095 | 1225.2 ms @ 3.2 s | 1225.2 ms |

**The arrival tail and the send tail track each other in every arm and
every rep** — arrival `over100` runs 7–10% above send `over100`, and the
process with the worst arrival gap always has a send gap of the same
order on that same process. That is the instrument's "busy sender"
reading, not its delivery-delay reading. It is the same on the fork and
on upstream, and the same in both thread/device shapes.

The timestamps say the same thing: every worst gap falls at 3–6 s, which
is inside load and tree build, not in the sparse two-way QD brackets
where a delivery stall would live. The one 14.1 s outlier is B's first
rep — the cold read of a 76 GB input — and it too carries a matching
12.0 s send gap on the same process.

**So on this fabric, in his own configuration, on both libraries: no
delivery-delay signature.** This is the application-level counterpart of
relay14's pure-LCI null, obtained without tracing.

### 3.3 Timing: no meaningful difference, and the small one runs against upstream

Iteration wall, ms:

| arm | iter 0 | iter 1 |
|---|---|---|
| B upstream ppn14/d7 | 5478, 5487, 5485 (mean 5483) | 5480, 5176, 5346 (mean 5334) |
| A fork ppn14/d7 | 5406, 5333, 5487 (mean 5409) | 5272, 4918, 5312 (mean 5167) |
| C upstream ppn7/d4 | 4678, 4737 (mean 4708) | 4700, 4600 (mean 4650) |

Upstream is 1.4% (iter 0) to 3.2% (iter 1) above the fork, in the same
direction in every phase — `phase1` 0.651/0.657/0.662 against
0.635/0.639/0.653. **I would not call that a regression.** The ranges
overlap (A's worst iter-0 rep, 5487 ms, equals B's worst), three reps is
three reps, and I built an ordering confound into the job myself: B ran
immediately before A in every rep, so any residual warm-up favours A.
`warmup_offpath` is identical to 5 ms across the two arms, which is what
one expects if the LCI layer is not the term that moved.

### 3.4 Arm D failed, and the failure is worth more than the arm was

Both reps of `+ppn 14 +lci_ndevices 14 +backend_poll_thread 1` died the
same way, ~150 s in, before any iteration:

    backend_ofi.cpp:register_memory_impl:458 <lci:Assert failed: false>
    err : No space left on device

That is a provider resource limit, not a delivery finding — I said before
the run that 14 devices x 8 processes = 112 libfabric domains per node was
double his 56 and might not fit, and it does not. Memory registration is
what runs out first.

**This retires relay16 item 13 at ppn 14.** "One thread per domain" means
`ndevices == ppn`, and at his sweep's `+ppn 14` that shape does not exist
on Frontier — the NIC cannot register memory for 112 domains per node.
The remedy is only available at `+ppn 7` (7 x 8 = 56 domains per node,
which relay16 measured clean). Anyone repeating relay16's recommendation
at higher ppn will hit this wall.

Given section 0, that costs nothing: the application is already serialised
by `ofi_lock_mode`, so it does not need one thread per domain.

### 3.5 An unasked-for result: his sweep's ppn 14 is 13% slower than the runbook's ppn 7

Arms B and C are the same binary, the same library and the same job — the
only difference is `+ppn 14 +lci_ndevices 7` against `+ppn 7
+lci_ndevices 4`. Iteration 1, mean over reps:

| | ppn 14 (B) | ppn 7 (C) | |
|---|---|---|---|
| iteration | 5.334 s | 4.650 s | **−12.8%** |
| `uf2` | 1.518 | 0.910 | −40% |
| `phase3_walk` | 0.845 | 0.680 | −20% |
| `loadCache` | 0.039 | 0.036 | flat |
| `phase1` (device) | 0.599 | 0.653 | +9% |
| `relabel` | 0.160 | 0.169 | +6% |

Half the PEs per process is faster per iteration at 16 nodes, and almost
all of the difference is `uf2` — the same anti-scaling with PE count that
relay13/15 recorded for the communication-bound phases. Phase 1 goes the
other way, slightly, which is expected: one GPU per process either way,
fewer host PEs to stage it.

**Caveats, because this is not the question the job was built for.** Two
reps against three, one node count, and 896 against 1792 PEs is a
different decomposition, not only a different mapping. It is an
observation to check, not a recommendation. But his 08-14 node sweep
(design/fof3-2b-scaling.md, jobs 5264070-5264135) ran `+ppn 14` at every
node count, so if it holds the whole sweep is measuring a shape that is
not his best one.

### 3.6 The preflight failed, and it was my error, not a result

Both 10k preflight arms aborted with `Reader NNN failed to open tipsy
file .../inputs/10k.tipsy`. That is 1792 readers opening one small file on
the home filesystem at once, not an LCI difference — **both binaries
failed identically**, so it discriminated nothing. The arms below it were
unaffected because the preflight was written as advisory. A future
preflight should use a Lustre-resident input.

---

## 4. What to change

1. **Ask Ritvik the relay16 question anyway.** Does his real (untracked)
   batch script set `LCI_OFI_THREADING_HINT`? Relay17 removes the LCI fork
   as a candidate for his stalls in his own shape, which makes his
   remaining differences from what I ran more important, not less. The
   dangling `else` in `4ee0d15` still means setting that variable to any
   value also stops `control_progress = FI_PROGRESS_MANUAL`.
2. **Brace the dangling `else` in `4ee0d15`.** Unchanged from relay16 item
   15. One pair of braces.
3. **Fix the fork's threading contract, or retire the fork.** Upstream
   `b8069dd` ran the application correctly five times out of five on CXI.
   If the three fork commits are not buying anything the application
   needs, dropping them removes a `FI_THREAD_DOMAIN` contract violation
   that is currently masked by a lock in a different repository.
4. **Do NOT repeat relay16 item 13 at ppn 14.** `ndevices == ppn` does not
   fit on Frontier above about 7 devices per process (section 3.4). At
   ppn 7 it is available and free; at ppn 14 it is not available at all.
   And with `ofi_lock_mode` set it is not needed.
5. **Extend the section-8 rule with an upper bound.** Every device needs a
   poller, no device should have more than one user thread if the provider
   is `FI_THREAD_DOMAIN` and unlocked — and **`ndevices x processes_per_node`
   must stay near 56, because 112 fails memory registration.**
6. **Check the ppn 7 vs ppn 14 result** (section 3.5) at more than one node
   count before believing it, and if it holds, rerun the 08-14 node sweep
   at ppn 7.
7. **Commit `run_fof3_scale_gpu.sbatch`.** design/fof3-2b-scaling.md §1
   names it as the script that drove the whole 08-14 sweep, and it is not
   in the repository — I recreated the command from §7. Same defect as
   `run_fof3_nosmt_8p.sbatch` in relay16 item 17.

## 5. What I did not do, and why

- **The causal control for section 0 has not been run.** The headline is
  code reading. `sbatch/relay17-ofilock-control-16n.sbatch` is written and
  its binaries are built and gated (`gpu-stalls/lci_multipeer_fork17`,
  `lci_multipeer_up17`); it adds `--ofi-lock on|off` to the reproducer and
  its prediction is written into the script. It was held back by the
  one-job-at-a-time rule. **This is the next job.**
- **No traced arm.** No arm hung, so there is no stall to classify — the
  same reason relay16 item 24 gave. Standing rule: traces only on request.
- **No comparison against relay13/15 numbers**, per the standing cover
  note.
- **Nothing pushed.** No file in any of Ritvik's repositories changed. The
  only code change anywhere is main's `241ddb4` applied as an uncommitted
  working-tree edit to `gpu-stalls/paratreet2`, which is a cherry-pick of
  an already-pushed commit, not new work. No patch is due.

## 6. State on Frontier

    ~/software/gpu-stalls/
      FoF3.fork   md5 8c3698af97bb299adab8a65148dd6689   -> charm/ + charm/lci (fork 4ee0d15)
      FoF3.up     md5 35af28c2f3fe42d9642ae5c644a1f844   -> charm-up/ + lci-up (upstream b8069dd)
      paratreet2  a491d27 gpu-phase1 + 241ddb4 as an UNCOMMITTED edit (fof/FoF.C, fof/FoFPhase1.h)
      charm / charm-up     both 9af1de4b6 + reconverse 397864f -- identical commits
      lci-src 4ee0d15 / lci-up-src b8069dd (a worktree of the same clone)
      lci_multipeer_fork17, lci_multipeer_up17  (reproducer + --ofi-lock, built, not yet run)
    ~/software/paratreet2 (the CPU tree) untouched at 8e08843, production charm, CLEAN.
    Raw logs: /lustre/orion/csc710/scratch/lvkale/s3ab/5307495/
    Scripts:  ~/software/sbatch/relay17-lciswap-16n.sbatch (ran)
              ~/software/sbatch/relay17-ofilock-control-16n.sbatch (ready, not submitted)
    Readout:  ~/software/scripts/relay17-read.sh 5307495
