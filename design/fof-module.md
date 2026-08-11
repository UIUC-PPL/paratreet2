# The fof/ module: extracting FoF from the core toolkit

**STATUS: DONE (2026-08-03, branch fof-module). Decision: Kale — extract now,
before further FoF work grows more core->FoF dependencies, and before the
other structural projects (single-distribution mode, SMP-cache extraction, a
Barnes-Hut app) build on the core.**

## What this fixes

paratreet2 is a particle-tree toolkit; FoF is an application on it. But the
FoF code was woven into the core module: `FoFData.h`/`FoFPhase1.h`/
`FoFPhase3.h` lived in `src/`, `src/paratreet.ci` declared the FoF chares and
`extern module unionFindLib`, and — the part that motivated doing this now —
the core had grown real compile-time dependencies on FoF:

1. `TreePiece.h` included `FoFPhase1.h` and carried an entry method with an
   FoF-typed signature: `TreePiece::registerFoF(CProxy_FoFPhase1<Data>, ...)`.
2. `Paratreet.h` included `FoFPhase1.h` and registered the FoF chares in
   `paratreet::Main<T>::__register` — every application (annotate,
   searchAlgos) registered FoF chares it never used.
3. Every application linked unionfind/prefix/htram, because the core module
   extern-referenced `unionFindLib`.

## The shape after extraction

- **`fof/`** (top level, sibling of `src/`): `fof.ci` (module `fof`,
  extern-referencing `paratreet` and `unionFindLib`), `FoFData.h`,
  `FoFPhase1.h`, `FoFPhase3.h`, `FoF.C` (module TU -> `libfof.a`),
  `fof-templates.h`. The coupling direction is FoF -> core only.
- **Core is application-free.** `src/paratreet.ci` has no FoF chares and no
  unionFindLib reference; `libparatreet.a` builds without the sibling
  libraries; annotate/searchAlgos link `-lparatreet` alone.
- **Build order**: sibling unionfind stack -> `src/` -> `fof/` -> FoF
  examples (README Getting Started).
- **Makefile.common** now exposes `FOF_PATH`, `FOF_INCLUDES` (fof + sibling
  include paths + the AGGREGATION toggle) and `FOF_LD_LIBS`
  (`-lfof -lunionFind -lprefix -lhtram_group_unionfind`); the global
  `INCLUDES`/`LD_LIBS` no longer mention any of them. `-DFOF` died entirely
  (nothing tested it). `-DUNIONFIND` moved into `FOF_INCLUDES`.

## The inversion: registerFoF -> callPerTreePieceFn

The one non-mechanical piece. The core now exposes a generic TreePiece-level
hook mirroring the existing per-leaf one:

- `paratreet::PerTreePieceAble<Data>` (CoreFunctions.h): PUP::able functor
  `operator()(Node<Data>* local_root, Particle* particles, int n)`.
- `TreePiece::callPerTreePieceFn(CkReference<PerTreePieceAble<Data>>, cb)`:
  applies the functor once per TreePiece element with the element's local tree
  root and contiguous particle block (stable from end of tree build to the
  next rebuild/reset — the same lifetime contract registerFoF documented);
  skips elements with no tree/particles; contributes to cb.

The fof module consumes it with `fof::TreePieceRegisterFn<Data>`
(FoFPhase1.h): carries the `CProxy_FoFPhase1<Data>`, and per element hands
(root, block) to the FoFPhase1 branch on that PE — exactly what
`TreePiece::registerFoF` did, with the FoF knowledge now on the FoF side.
`runFoFPhase1` sends it through the hook. `Partition::verifySharedLeaves`
stayed in core: its content (leaf-aliasing pointer-identity assertion) is
app-agnostic.

## Per-Data registration

Templated chares of a non-main module are not auto-registered. The fof
module provides `fof::registerChares<Data>()` (FoFPhase1.h) — registers
`CkIndex_FoFPhase1<Data>`, `CkIndex_FoFPhase1Node<Data>`, and the
`TreePieceRegisterFn<Data>` PUPable — and the FoF app's Main subclass calls it
from an overridden `__register()` after the base class's (see
examples/fof3/Main.h). The core's `Main<T>::__register` no longer knows FoF.

## Charm mechanics learned (details in charm-notes best-practices)

- **Template definitions must precede concrete use, via the decl+templates
  idiom.** Generated `CBase_`/closure/proxy templates live in the def.h
  `CK_TEMPLATES_ONLY` section. Any header whose inline code uses a
  concrete-Data chare (FoFPhase3.h's FragData visitors call
  `ckLocalBranch()->member` — completeness required at parse) needs those
  definitions first. The codebase idiom (TreePiece.h/CacheManager.h/
  TreeCanopy.h include `templates.h`) transfers: FoFPhase1.h includes
  `fof-templates.h` right after `fof.decl.h`. Diagnosed by preprocessing the
  pre-extraction TU and finding `CBase_FoFPhase1`'s definition arrived early
  through exactly that chain.
- **PUPable_decl_template fails under a dependent base.**
  `TreePieceRegisterFn<Data> : PerTreePieceAble<Data>` reaches PUP::able through
  a dependent base, so the macro's unqualified `register_constructor` call
  does not resolve (unqualified lookup skips dependent bases). Hand-expanded
  the macro with `PUP::able::register_constructor` qualified; the templated
  `my_PUP_ID` static lives in the header as a template definition.
- **A fully-templated module still needs one TU.** `FoF.C` plainly includes
  `fof.def.h` for the non-template module-registration part; the app's
  `extern module fof` wires `_registerfof` into its registration chain.

## Residual FoF traces in core (deliberate, follow-ups)

- **Particle layout**: `Particle::group_number` (FoF component id) and
  `Particle::vertex_id` are plain fields with no FoF type dependency, but
  they are FoF-motivated payload every app carries (~12B/particle). Removing
  them needs per-app particle-field parameterization — same family as the
  CachedParticle work; do deliberately, not inside this extraction.
- `Node.h` order-range fields ("for FoF optimization") — generic in type,
  FoF-motivated; same disposition.
- Comments across core mentioning FoF as the motivating example — harmless.

## Validation (2026-08-03, laptop classic)

fof3 12-run matrix 12/12 (72/390/3549); 1M 4-proc b0.2 = 333,889 and
b0.8 = 41,315 (histograms match the recorded baselines); fof1 `make test`
PASSED (incl. STEP1); annotate 4/4 multi-process sfc+oct; searchAlgos both
configs; annotate/searchAlgos link lines contain no unionfind/htram/prefix.
Reconverse (recharm stack) validation: see the running notes below this
line as it lands.
