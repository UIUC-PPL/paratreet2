# How to apply the current patch set

Cumulative: these two files carry EVERY working-tree change made on Frontier
since the last pull, so apply only the latest pair and ignore earlier ones.

Base commits (both are upstream GitHub HEADs as of 2026-08-22):

    paratreet2  main    57f2395
    unionfind   master  d72ee66

Apply, one per repository, from that repository's ROOT:

    cd <path>/unionfind   && git checkout d72ee66 && git apply relay79-unionfind.diff
    cd <path>/paratreet2  && git checkout 57f2395 && git apply relay79-paratreet2.diff

They are SEPARATE files because a single diff spanning both repos cannot be
applied by git apply from either root — the paths belong to different trees.

What the patch adds (all three default to today's behaviour):

  [UFSTAT] branch census    always on, cost not detectable (relay78/79)
  FOF_UF_SHORTCIRCUIT=1     Kale's backward short-circuit; re-enables the two
                            commented-out call sites, adds a monotone guard,
                            fixes a std::pair<int,int> truncation. Measured:
                            remote climb hops -32%, wall unchanged (relay78).
  FOF_UF_SIZES=0            skips the add_size calls. Measured -1.92% and
                            EXACT (relay79). NOT SAFE TO SHIP AS A DEFAULT --
                            unionFindLib is shared and local_union still does
                            a direct "size +=", so the field becomes
                            inconsistent rather than absent. Opt-in only.

NOT pushed. The Frontier trees are left uncommitted at the base commits above.
