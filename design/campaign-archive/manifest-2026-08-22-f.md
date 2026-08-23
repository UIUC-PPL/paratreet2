# uploads/ — 2026-08-22 18:45

**`relay81-cpu-szOFF-timeline.tar` (771 MB) is the viewable CPU trace.** The
relay85 files below are unchanged and still here.

## The trace: one arm, one kind, complete

    cpu-szOFF/  896 x .log.gz   +  .sts  +  .projrc      896 PEs, 0..895 all present
                NO .sum, NO .sumd, NO .sum.sts

This is the per-event (timeline) tranche exactly as you asked — one kind, not
both. Last time I shipped `.sumd`+`.sts` in one tar and `.log.gz` in another and
dropped all 896 `.sum` files **and the `.projrc`** from both, which is why
nothing opened. This time I listed every file kind in the source directory first
and verified the counts *inside* the tar. I also deliberately left out
`.sum.sts`: a summary index with no `.sum` files beside it is the kind of thing
that could send Projections looking for summary data that is not there.

Untar anywhere; it unpacks to `cpu-szOFF/` and that directory is the one to open.

## Which arm this is, since the names are opaque

`szON` / `szOFF` is `FOF_UF_SIZES` — the union-find component-size maintenance
that FoF3 never reads.

    cpu-szON   FOF_UF_SIZES=1   sizes maintained (the old default)   4691.9 ms
    cpu-szOFF  FOF_UF_SIZES=0   add_size skipped                     4635.3 ms   <-- this one

**`szOFF` is the better performer** and is the recommended configuration. Those
two walls are from the traced binary, so they carry the +2.7–3.2% tracing cost
and are not production numbers; untraced the same pair is 4577.7 against 4467.2
(relay82, 3 reps each).

Provenance: job 5326926, `FoF3.cpu-prodtr-v79`
`22777303bf63b966fb99ecf4e9d8a429`, clean trees at `paratreet2 7263ff1` /
`unionfind db73766`, EXACT. The source directory is still on scratch at
`/lustre/orion/csc710/scratch/lvkale/s3ab/5326926/traces/cpu-szOFF/` if the
summary kind is ever wanted as a separate tranche.

## The relay85 tranche (unchanged, still here)

`relay85.txt` — the root distribution. They do **not** concentrate: 125 of 128
chares root straddling components, 264,511 incidences, mean 2,066, median 1,592,
max 9,678 (max/mean 4.7x). Chare 127 roots exactly zero, which is what the
encoding predicts. Your PE 350 (chare 50) is a genuine hotspot at 7,124; PE 840
is at the mean and PE 854 below it.

Section 6 answers the dense-bits follow-up: it is a correctness break (dense
indices restart at 0 on every process, so ties produce a two-cycle), there is no
dense-to-process correlation to exploit, and even the well-defined hashed variant
would not touch the tail — the home PEs are 3.7% busy in the drain — while
likely tripling the 16.5% flip rate, which is the one lever left.

    relay85.txt                     the root census in full
    relay85-rootcensus-16n.sbatch   the job
    relay85-unionfind.diff          report-only patch (NOT pushed)
    relay85-paratreet2.diff         one-line driver hook, FOF_ROOT_CENSUS
    build-v81.sh                    binaries from the patched trees
