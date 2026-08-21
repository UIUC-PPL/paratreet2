# merged/ — fresh clones from GitHub, 2026-08-20

Cloned at Kale's request so the current merged code can be explored and the
GPU-helper patch validated against it, WITHOUT touching the study tree in
`gpu-stalls/` (whose provenance is pinned to the relay measurements).

    paratreet2  htram  unionfind  charm  reconverse  lci     499 MB total

Kokkos is not here: `$HOME/kokkos` is an install tree, not a repository.

## The patch question is settled

`fof/gpu/FoFDevice.cpp` is BYTE-IDENTICAL on paratreet2 `main` (6c0a568),
`gpu-phase1` (ae687f7) and the base the fix was developed against (a491d27) —
md5 `d178162bb1a9929ee93753a49e7ec9d8` — even though main is 189 commits and
17,260 insertions ahead. `git apply --check` passes on both branches, and the
patched file compiles clean with hipcc against merged main. **No regeneration
needed.** The patch is applied in `merged/paratreet2` right now, main checked
out.

## OPTION 1 IS BUILT AND MEASURED (job 5319480, 2026-08-21)

Current paratreet2 `main` (6c0a568) + the fix, on the runtime we measured
(charm 9af1de4b6 / reconverse 397864f / LCI b8069dd, reused from gpu-stalls),
with htram 3f2ee40 and unionfind 8933bae as siblings in this folder.
Binary `merged/FoF3.merged`, md5 b89898c1da10fb9bf3780bfeb948477f.
Build script `merged/build-merged.sh`. ALL FOUR ARMS EXACT.

    arm                     Iteration 0     note
    ppn 7  fix on            3635.2 ms      "affinity fix active (SMT siblings)"
    ppn 7  fix off           4682.7 ms      -1047.5 ms, -22.4%
    ppn 14 fix off           5681.9 ms
    ppn 14 fix on            5278.4 ms      "affinity fix DECLINED" -- as designed

**The fix works unchanged on merged code: -22.4%, against -22.8% on the old
base.** And the merge itself is worth about 5% in both arms (ppn 7 no-fix
4915.7-5007.5 -> 4682.7; fix 3859.1-3865.0 -> 3635.2), so the two effects are
independent and additive.

The new decline warning fired in the field at ppn 14, which is what it is for.

**A spread calibration worth keeping:** the ppn-14 pair differs by 403 ms here
and by 394 ms in job 5319180, in both cases between arms that provably differ
only by an env var the fix ignored (it declined). So single-rep spread at
ppn 14 is about 400 ms on 5.5 s, ~7% — larger than the ~3% I had been
assuming. The relay43 ppn-14 result (-17.1%) is still well outside that, but
the caveat is sharper than I wrote it.

## Three things a fresh clone needs that the working tree already had

1. `git submodule update --init --recursive` — the `utility` submodule.
2. `cd utility/structures && ./configure` — it is autotools, and `src/`
   includes `xdr_template.h`, which needs the generated `config.h`.
3. `make AR="ar cr" libTipsy.a` — the generated Makefile has `AR = ar` with no
   operation letters, so the stock rule runs `ar libTipsy.a ...` and ar reads
   the archive name as options. Both trees have this; whoever built the
   gpu-stalls one hit it too.
`build-merged.sh` now does all three automatically.

## Which ref to build — THE REMAINING DECISION (option 2)

| repo | what we built and measured | on GitHub now | note |
|---|---|---|---|
| paratreet2 | a491d27 (gpu-phase1) | main 6c0a568 / gpu-phase1 ae687f7 | main has the merge |
| htram | 3f2ee40 | main 3f2ee40 | unchanged, no decision |
| unionfind | 8933bae | master 23e46a0 (OLDER) | our commit is on `fof_with_aggregation` (head = 8933bae) and `phased-design`. **The default clone checks out the wrong branch.** |
| charm | 9af1de4b6, local branch `gpu-merge-candidate` | that branch is GONE upstream; the commit survives as an ancestor of `reconverse-specific-build` (head 90ef159b9). `main` is c5df4b3d6 and is 5 commits ahead of / 142 behind ours | **needs a decision** |
| reconverse | 397864f (2026-08-14) | main 6f34e68 (2026-08-20, "Changes to GPU/RDMA implementation (#204)") | moved |
| lci (ritvikrao fork) | b8069dd (2026-04-27) | main ca88ce2 (2026-08-18) | moved a long way |

Two coherent choices, and they answer different questions:

- **Current app on the known-good runtime.** paratreet2 `main`, and everything
  else at the refs we measured. Isolates the application merge; the fix's
  numbers stay comparable.
- **Current everything.** Every repo at its current head. Answers "does the
  stack still work", but charm/reconverse/LCI all moved and a build failure
  would not tell us anything about the fix.

The first is DONE (see the option-1 section above). The second — the
all-latest test — is the open one.

**What the all-latest test would be, concretely.** In `merged/`, check out
every repo at its current head, build the runtime from scratch there, and run
the same two arms:

    paratreet2  main                        (already there, patch applied)
    htram       main            3f2ee40     (unchanged, nothing to do)
    unionfind   fof_with_aggregation        NOT master -- master is older
    charm       reconverse-specific-build   or main; NEEDS A DECISION
    reconverse  main            6f34e68
    lci         main            ca88ce2

The charm line is the one that needs a human. `gpu-merge-candidate`, the branch
we built, no longer exists upstream; its commit survives as an ancestor of
`reconverse-specific-build`, whose head is 90ef159b9. Whether that branch or
`main` is the intended successor is a question for whoever did the merge.

Everything else in that list is mechanical. Budget: a charm build from scratch
plus LCI, roughly 20-30 minutes, then the app, then a 4-arm job of about 2
minutes. The failure mode to expect is an interface mismatch between the
current reconverse/LCI and the app, which would be information about the
runtime merge and NOT about the fix.

## What Kale settled about the new defaults

His point, verified in the merged source rather than assumed: with the GPU
path enabled the new defaults are irrelevant, and it is **all** of them, not
most.

`fof/FoFPhase1.h:341` — "Replace: the device computes phase 1 and the CPU
chain does not run. **phaseA, phaseB, merge and relabel are skipped
outright**." That covers `FOF_STEALA`, `FOF_STEALA_GEO` (phaseA claim pool),
`FOF_PB_PARTS` (phaseB pool) and `FOF_PHASEB_SLICE_MS` (phaseB drain).

And `FOF_PE_SETS`: README §36 — "AUTO = one set per PE ... **except under any
GPU mode, where AUTO resolves to 1** (engine contract §3)." So the split
disables itself. Setting it explicitly with a GPU run is a hard error by
design (`FoFPhase1.h:1906`, "the device engine does not implement it").

So my earlier warning — that merged main's defaults make a run there
incomparable to the relay numbers — was wrong for the GPU arm. What survives
is narrower: 189 commits touched 1,455 lines in `fof/` and `src/`, most of it
`FoFPhase1.h` (+1,173), i.e. the CPU chain the GPU path skips. Whether
anything the GPU path *does* use changed is a separate check I have not run;
the candidates are `fof/FoF.C` (+105), `fof/fof.ci` (+11), `src/common.h`
(+134) and `src/Traverser.h` (+30).
