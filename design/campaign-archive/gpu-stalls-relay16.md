# Ritvik's GPU stalls: the cause is in his LCI fork, not in his GPU code

Jobs **5304940** and **5305170** (pure-LCI reproducer, fork vs production),
**5305230** (his reference run reproduced + the fix), **5305278**
(intermittency, 20 vs 20). 2026-08-18. Companion: `reports/relay16.txt`.

**Verdict, in two halves, because the second half qualifies the first.**

**(a) There is a real defect in his LCI fork, and it is of exactly his
symptom class.** The fork downgrades libfabric's threading contract to
`FI_THREAD_DOMAIN`, and reconverse's thread-to-device mapping then puts TWO
threads in one domain at his `+ppn 7 +lci_ndevices 4`. The pure-LCI
reproducer fails 8 of 8 in that shape and is clean 4 of 4 with one thread
per domain; production LCI is clean 8 of 8 in both shapes. The failure
presents as segfault, as double free, and -- in one arm -- as a silent hang
with no error at all.

**(b) But it did NOT reproduce in the application, in 23 runs of his exact
config, and that is a problem for (a) as the explanation of HIS stalls.**
Job 5305278 ran 20 interleaved reps of each config: **0 failures in 20 for
his config, 0 in 20 for the fix**, on top of the 3 clean reps in 5305230.
His own history shows one hang (5302111, cancelled at 8:06) in three real
attempts today. At a 1-in-3 rate, 23 consecutive clean runs has probability
9e-5; even at 1-in-6 it is 1.5%. **Something differs between my rebuild and
the run that hung, and the untracked batch script is the prime suspect --
see section 5a.** The fix is free and I would still run it, but I have not
earned the claim that it removes his stalls.

## 1. The two free checks, done before any job (Part 1 step 2)

Both pass on his launch line, so neither is the cause:

* **pemap count.** 56 entries, no duplicates, no multiples of 8, range 1-63.
  Equals procs_per_node x ppn = 8 x 7. The phantom-stall trap of his own
  section 8 is not firing.
* **Device coverage.** At ppn 7 / ndevices 4 / stride 2:
  `nthreads_per_device = ceil(7/4) = 2`, so threads {0,1}->d0, {2,3}->d1,
  {4,5}->d2, {6}->d3; pollers are threads 0,2,4,6 -> devices 0,1,2,3. Every
  device has exactly one poller. I also reproduced his ndevices-7
  counter-example: pollers land on devices 0,2,4,6 and devices 1,3,5 get
  none, which is the startup hang he documents.

His rule -- "ceil(ppn/ndevices) must be at least the stride, and no device
may be left unassigned" -- is correct as far as it goes. **What it does not
say is that no domain may have more than one USER.** That gap is the bug.

## 2. What his LCI fork changes (read before building it)

`ritvikrao/lci` branch `frontier-changes` at `4ee0d15`, three commits on top
of upstream `b8069dd`. Both it and production LCI are version 2.0.0, so the
diff is meaningful rather than a generation gap.

| | production LCI | the fork |
|---|---|---|
| `domain_attr->threading` | `FI_THREAD_SAFE`, unconditional | **`FI_THREAD_DOMAIN`** when multiprocess and provider is cxi or unset -- always, on Frontier (`05ee2d8`) |
| MR calls | unserialized | mutex around `fi_mr_regattr`/`bind`/`enable`/`close` (`19fe6b8`) |
| null completion context | -- | `if (!internal_ctx) return;` guard in `progress_read` (`05ee2d8`) |

`FI_THREAD_DOMAIN` is libfabric's contract that **the caller** serializes
access to a domain and every object under it. The fork's own next commit
says what happens when that is violated:

> "FI_THREAD_DOMAIN requires caller serialisation; concurrent `fi_mr_enable`
> calls corrupt CXI's internal LE-append queue and cause
> `cxip_mr_wait_append` to spin forever."

That mutex serializes **one** path. Every send and every progress call on a
shared domain is still unserialized -- and reconverse's `initThread` maps
`device_id = thread_id / ceil(nthreads/ndevices)`, so at ppn 7 / ndevices 4
threads 0+1, 2+3 and 4+5 each share a domain. The `progress_read` null guard
is the same story from the other side: a null completion context is what you
get after the provider's event queue has been corrupted.

**A separate landmine in the same fork.** `4ee0d15` adds
`LCI_OFI_THREADING_HINT` with a dangling `else`:

```cpp
  if (threading_hint_env != nullptr) {
    ... sets threading ...
  } else
  hints->domain_attr->control_progress = FI_PROGRESS_MANUAL;
  hints->domain_attr->data_progress = FI_PROGRESS_MANUAL;
```

Setting that variable to **any** value -- including an unrecognised one --
also stops `control_progress` being set to `FI_PROGRESS_MANUAL`, because the
unbraced `else` swallows that statement. It is exactly the knob someone
debugging stalls reaches for, and it silently changes a second thing. Fix is
one pair of braces.

## 3. The decisive experiment (jobs 5304940, 5305170)

`lci_multipeer.cpp` from relay14, relinked against the fork's `liblci` and
run beside the production build in the same job, on the same nodes.

**A trap first, because it nearly cost the whole result.** The first link
against the fork silently resolved to the **production** `liblci.so`: the
linker emits `RUNPATH`, which loses to `LD_LIBRARY_PATH`, and the production
charm lib directory is on it. That would have reproduced relay14's null and
"cleared" the fork. Relinked with `--disable-new-dtags` (verified with
`readelf`: `RPATH`, not `RUNPATH`) and the job now gates on `ldd` before
spending the allocation.

**A defect in my own harness, found and fixed between the two jobs.** In
5304940 the main thread called `lci::barrier_x()` on device 0 while a worker
progressed device 0, on every non-zero rank -- two threads in one domain in
*every* arm. Production's `FI_THREAD_SAFE` tolerates that; the fork's
`FI_THREAD_DOMAIN` does not. So 5304940's "one thread per device" arm was
not one thread per device and isolated nothing. Fixed by making main thread 0
on every rank (workers 1..n-1, thread count unchanged) and rerun as 5305170.

**Job 5305170, 10,000 round trips per arm, 2 reps x 2 peer counts:**

| LCI | shape | threads/domain | K=16 rep1 | K=16 rep2 | K=64 rep1 | K=64 rep2 |
|---|---|---|---|---|---|---|
| fork | 7t/7d | **1** | 7.30 us | 7.31 us | 8.67 us | 9.07 us |
| fork | 7t/4d (**his**) | **2** | **FAIL** | **FAIL** | **FAIL** | **FAIL** |
| fork | 14t/7d | **2** | **FAIL** | **FAIL** | **FAIL** | **FAIL** |
| production | 7t/7d | 1 | 7.26 | 7.77 | 8.58 | 8.65 |
| production | 7t/4d | 2 | 13.90 | 19.77 | 10.90 | 10.49 |

Every clean arm had **0 samples over 1 ms**. The fork survives if and only
if each domain has exactly one thread. Production tolerates both shapes.

**The failure is not one symptom but three**, all from the same violation:

* `cxip_evtq_event_req(): Invalid event type: 19` from the CXI provider,
  then LCI's `Assert failed: is_packet ... Not a packet (address 0x463060)`
* `free(): double free detected in tcache 2`, with segfaults and bus errors
  across many ranks
* **a pure hang with no error at all** (`f-k64-t7d4-r2`), killed by the guard
  at 177 s with zero diagnostic output

The third is his symptom exactly: job 5302111 cancelled at 8:06 with no
output, and his own section 8 describing a configuration that "HANGS in
startup for the whole walltime with no error."

## 4. His reference run, reproduced (job 5305230)

Stack built from the runbook: paratreet2 `a491d27` on `gpu-phase1`, charm
`9af1de4b6` + reconverse `397864f` (both `gpu-merge-candidate`), LCI fork
`4ee0d15`, Kokkos 4.7.04. Binary verified with `nm`, not the build log:
300 `fofgpu::` symbols, 38 `LogPool`, 4 `deviceWarmup`, and `ldd` confirming
the **fork's** `liblci`.

| arm | components / max_size | device wall | init | warmup_offpath | Iter0 |
|---|---|---|---|---|---|
| his ref 5303289 notrace | gold | 0.584 s | 0.000 | 0.461 | 4275 ms |
| his ref 5303289 traced | gold | 0.582 | 0.000 | 0.379 | 4339 |
| asis-r1 | **EXACT** | 0.584 | **0.000** | 0.465 | 4722 |
| asis-r2 | **EXACT** | 0.571 | **0.000** | 0.420 | 4615 |
| asis-r3 | **EXACT** | 0.573 | **0.000** | 0.418 | 4714 |
| traced | **EXACT** | 0.583 | **0.000** | 0.448 | 5000 |
| fix-r1 | **EXACT** | 0.586 | **0.000** | 0.362 | 4748 |
| fix-r2 | **EXACT** | 0.585 | **0.000** | 0.365 | 4566 |
| fix-r3 | **EXACT** | 0.582 | **0.000** | 0.366 | 4719 |

All 7 arms exact on both gates (424,897,832 / 185,317,566). **`init` is
0.000 everywhere**, so the device-init hoist this run exists to measure is
firing, and device wall reproduces his 0.584/0.582 to within 2%. Iteration 0
runs about 10% higher than his reference (4566-5000 against 4275/4339); that
is a different allocation on a different night and I would not read it as a
regression without an interleaved comparison, which relay15 item 10 is the
standing argument for.

**The fix costs nothing.** `+lci_ndevices 7 +backend_poll_thread 1` gives
device wall 0.582-0.586 against 0.571-0.584, and Iteration 0 4566-4748
against 4615-4722 -- indistinguishable. `warmup_offpath` is if anything
lower (0.362-0.366 against 0.418-0.465).

**And his config did not stall in 3 reps**, which settles nothing about an
intermittent failure. Job 5305278 asks the real question of the application:
20 reps of each config, interleaved, counting failures rather than walls.

## 4a. The application does not reproduce it (job 5305278)

20 interleaved reps of each config, counting failures rather than walls:

| config | threads/domain | failures |
|---|---|---|
| his `+lci_ndevices 4 +backend_poll_thread 2` | 2 | **0 of 20** |
| the fix `+lci_ndevices 7 +backend_poll_thread 1` | 1 | **0 of 20** |

All 40 runs exact, walls 13-16 s (one 27 s cold first read). With the 3 reps
in 5305230 that is **23 consecutive clean runs of the configuration the
reproducer kills 8 times out of 8**.

Both facts are solid, so the gap between them is the finding:

* The reproducer drives one thread through 10,000 tight round trips while
  its partner threads spin on the same domain. FoF's per-thread traffic is
  far sparser, so two threads may almost never be inside one domain at the
  same instant. A latent contract violation that needs a collision to bite
  would look exactly like this: fatal under stress, rare in the app.
* Or his hang has a different cause, and the fork defect -- though real --
  is not it.

**The arithmetic says something differs between my rebuild and his hung
run.** One hang (5302111) in three real attempts today is a 1-in-3 rate;
23 consecutive clean runs at that rate has probability 9e-5, and even at
1-in-6 it is 1.5%. I did not get lucky 23 times.

### 5a. What most likely differs, and the one question to ask Ritvik

**His batch script is UNTRACKED** -- his runbook says so, and section 6
exists only to recreate it from memory. I ran the recreated version. The
run that hung, 5302111 at 13:04, used the real one.

**The specific thing worth checking first: does his real script set
`LCI_OFI_THREADING_HINT`?** He added that knob himself (`4ee0d15`, May), it
is not in the runbook's environment list, and because of the dangling `else`
in that commit, setting it to any value also stops `control_progress` being
set to `FI_PROGRESS_MANUAL`. A job whose control progress is left to the
provider's default is a strong candidate for "hangs for the whole walltime
with no error". That single line would explain his hang, my null, and why
the runbook's recreated script behaves differently.

Two other differences worth ruling out, in order:

1. **Binary vintage.** 5302111 ran at 13:04; the pre-hoist reference
   (5302545) ran at 15:16 and the post-hoist one (5303289) at 16:28. His
   hang predates *both* reference runs, so it was a different build than
   either arm the runbook documents.
2. **Node set.** His hang and my 23 runs are different allocations.

## 5. What to change

1. **Run with `+lci_ndevices 7 +backend_poll_thread 1`** (at ppn 7). It is
   the only mapping that satisfies both his section-8 poller rule and the
   `FI_THREAD_DOMAIN` contract his fork imposes, and it is free.
2. **Or fix the fork instead**, which is better if the GPU work wants fewer
   devices: either keep `FI_THREAD_SAFE` (production's choice, and the
   evidence here is that the provider handles it), or serialize per-domain
   inside LCI rather than only around the MR calls.
3. **Brace the dangling `else`** in `4ee0d15` before anyone uses
   `LCI_OFI_THREADING_HINT` to debug this.
4. **Extend his section-8 rule** to say what it means: every device needs a
   poller *and* no device may have more than one user thread. With the fork's
   threading model, `ndevices` must equal `ppn`.
5. **Commit `run_fof3_nosmt_8p.sbatch` to `gpu-phase1`** -- his runbook flags
   it untracked, and section 6 exists only to recreate it by hand.

## 6. Five runbook defects, found by following it

Every one of these stops a second person cold; none is in the runbook.

1. **`KOKKOS_DIR` defaults to `/ccs/home/rrao/kokkos`** in both
   `fof/gpu/Makefile:14` and `src/Makefile.common:95`. His home is not
   readable by anyone else. The runbook says it "defaults to `$HOME/kokkos`".
   Pass `KOKKOS_DIR=$HOME/kokkos` explicitly, or change the default.
2. **`module load rocm/6.2.4` is a silent no-op** non-interactively: it sets
   no `ROCM_PATH` and does not appear in `module list`. Kokkos and
   `reconverse/CMakeLists.txt:126` (`find_package(hip REQUIRED CONFIG)`) both
   fail. Both need `-DCMAKE_PREFIX_PATH=/opt/rocm-6.2.4`.
3. **Charm's `amd` target builds `reconverse-linux-x86_64-amd`**, not
   `reconverse-linux-x86_64`. `CHARM_HOME` must include the suffix; the
   runbook's "set CHARM_HOME to that build directory" reads as the latter,
   and `hapi.h` is only in the former.
4. **`git clone kokkos` now gives master**, whose `Kokkos_BitManipulation.hpp`
   needs C++20 while `fof/gpu/Makefile:25` compiles `-std=c++17` -- 20 errors.
   Pin a 4.x release; 4.7.04 works.
5. **Section 1's clone list is incomplete.** The build also needs
   `N-BodyShop/utility` (as `paratreet2/utility`, for
   `utility/structures/Vector3D.h`) and the sibling `unionfind` and `htram`
   checkouts. Without them `src` does not build at all.

## 7. What I did not do, and why

* **Step 4 of the cover note (CPU-only control on his runtime) was not run,
  by the cover note's own branching**: it is conditional on step 3 coming
  back clean ("clean -> move to 4"). Step 3 did not come back clean, so the
  bug is located and the control is not needed.
* **Step 5 (classify the stall on the traced arm) is not possible from my
  traces**: the traced arm ran clean, so there is no stall in it to classify
  as idle-gap versus busy-opaque versus QD-settle. If job 5305278 catches a
  hang, that arm is the one to trace.
* **No comparison against relay13/15 numbers**, per the cover note: his
  branch predates the PE-set split and the cleanup.
* **Nothing pushed**, and no file in his repositories changed. The stack
  lives in `~/software/gpu-stalls/`.
