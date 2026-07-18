# ParaTreeT2

A restructured tree-traversal library for particle computations in Charm++,
extracted from [ParaTreeT](https://github.com/paratreet/paratreet). Initial
driving application: parallel Friends-of-Friends (FoF) cluster finding for
astronomy data, per the design in `../fof_design_note.md` (to be imported here).

## Why a new library

Two constraints of the FoF design that ParaTreeT cannot satisfy without a break:

1. **Node `Data` recomputable after tree build.** ParaTreeT computes node
   payloads (leaf ctor + `operator+=` upward accumulation) only at build time.
   FoF needs a post-phase-1 upward pass to annotate `min_frag`/`max_frag` on
   every node, shipped with remote nodes by the cache.
2. **Visitors with mutable node-local state.** The FoF boundary walk's `open()`
   consults and updates a process-level SEEN table (per-fragment-pair state
   machine). ParaTreeT visitors are pure over `(source, target)`.

## Components carried from ParaTreeT (minimal edits)

- SMP-aware atomic treenode cache: `CacheManager` + `Node`/`FullNode`/node pools
  (`src/CacheManager.h`, `src/Node.h`, `src/Resumer.h`, `src/MultiData.h`)
- Reader pipeline: Tipsy/NChilada load, SFC key generation, sample sort,
  redistribution (`src/Reader.*`, `utility/` structures)
- Decomposition hierarchy (Oct / SFC / kd) and `TreeSpec`
- Tree build: `Subtree` + `Modularization` strategies
- Traverser/Visitor framework (`src/Traverser.h`) — with the visitor contract
  widened as above

## Deliberately restructured

- `Data` gains an explicit "upward pass" API callable between traversals.
- Visitor `open()` may consult mutable process-level state.
- The union-find coupling changes shape entirely: two-level UF. UF_1 is serial
  per-process over particles (freezes at end of phase 1); the existing
  distributed [unionfind](https://github.com/UIUC-PPL/unionfind) library becomes
  UF_2 over ~1000x fewer fragments, driven by an emitted merge-edge stream
  rather than calls from inside a traversal visitor.

## Layout (planned)

```
src/            core library -> libparatreet2.a
examples/fof/   FoF application (first client)
tests/          correctness tests vs serial FOF on small boxes
```

## Status

Scaffold only — extraction in progress. See `../prompt_log.md` for project
history and `../fof_design_note.md` for the algorithm design.

## License and provenance

ParaTreeT2 is licensed under the Apache License 2.0 with LLVM Exceptions
(see `LICENSE`), the same license as Charm++.

This work is based on [ParaTreeT](https://github.com/paratreet/paratreet),
written primarily by **Joseph Hutter** with contributors at the Parallel
Programming Laboratory and collaborating institutions; ParaTreeT2 extends
and streamlines it. See `NOTICE` for full credits. The N-BodyShop
`utility` code (Tipsy/NChilada readers, SFC) retains its own license.
