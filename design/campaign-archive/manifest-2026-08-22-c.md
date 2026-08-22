# uploads/ — 2026-08-22 12:15

**START HERE: `relay80.txt`.** Covers three jobs: the CPU confirmation on the
landed code, the GPU arm, and the traced answer to what the saving actually is.
Every arm of all three jobs is EXACT.

## The landed code is confirmed

Pulled to `paratreet2 7263ff1` + `unionfind db73766`, working trees clean —
the local patch was discarded before pulling, so these binaries are the landed
code and nothing else. Seven independent sizes-off arms (CPU and GPU,
production and traced) all EXACT on components *and* max_size, so the
`local_union` gating is inert as you said. **Your form is better than mine**:
gating the local merges makes the field uniformly unmaintained, which is what
makes the `prune_components` abort a complete guard rather than a partial one.

## Where the headlines stand

    arm   config                                    Iter0
    GPU   ppn 7, -l 128, fix on, FOF_UF_SIZES=0   2562.2 ms  [2547.9..2576.5]
    GPU   same, sizes on (today's default)        2630.9 ms
    CPU   ppn 7, -l 32,  FOF_UF_SIZES=0           4467.2 ms  [4450.6..4491.4]
    CPU   same, sizes on (today's default)        4577.7 ms  [4576.3..4578.6]

    CPU -110.5 ms  -2.41%  (3 reps each)     GPU -68.7 ms  -2.61%  (2 each)

The GPU effect is **larger** than CPU, and my pre-registered guess that it
would be smaller was wrong. I reasoned that `FOF_GPU_PHASE1` forces `peSets=1`
so the GPU arm lacks the 794k same-process unions — but the census shows
`fb2_UNION` is ~427k on GPU against ~437k on CPU. Those same-process unions
never used add_size *messages* anyway, since `local_union` handles them inline.

`gpu-szON` at 2630.9 also confirms the relay73 prediction that landing the
prefix removal plus the sort path would put the GPU headline near 2640.

## The drain itself shrank — not just the queuing

    arm         drain     uf protocol CPU in it
    cpu-szON    311 ms         1015.8 PE-ms
    cpu-szOFF   255 ms          769.0        -56 ms  -18%
    gpu-szON    248 ms          626.3
    gpu-szOFF   181 ms          399.2        -67 ms  -27%

And it is not an artefact of the definition. The drain is the span of
union-find activity and `add_size` *is* union-find activity, so deleting it
could shorten the window by construction. `relay81-findonly.py` re-measures
with `add_size` excluded — the find cascade only — and the drain lengths come
out **identical**. add_size never extended past the find cascade.

On GPU essentially the whole wall saving is the drain; on CPU about half, the
rest from the walk-concurrent part of the cascade. One limit worth stating: a
same-chare `add_size` is a direct call inlined into `find_boss2`, so it is
charged to `insertDataFindBoss`. The drain *lengths* are clean; the PE-ms
columns are not a pure find-versus-add_size split.

## A single-rep trap I walked into

relay80 put the CPU delta at −0.66% from **one rep per arm**. relay82 at three
reps gives −2.41%, and relay79 gave −1.92%. relay80's lone sizes-on rep
(4494.3 ms) is faster than every sizes-on rep in the other two jobs. My summary
line also printed "SEPARATED" for two single points, which is vacuous — fixed,
it now refuses to judge separation below n=2.

## Needs your call

`RECOMMENDATION-affinity-fix.md` is stale: it still quotes 2825–2882 ms GPU and
4785–4811 ms CPU, from before the prefix/sort landing and before this knob. It
should be rewritten against the table above. I have not touched it.

## Files

    relay80.txt                     the three jobs in full
    relay81-findonly.py             drain re-measured without add_size
    relay80-sizes-gpu-16n.sbatch    CPU confirmation + GPU arm
    relay81-sizes-traces-16n.sbatch four traced arms
    relay82-cpureps-16n.sbatch      CPU, 3 reps each
    build-v79.sh                    all four binaries from clean trees

Traces kept at `/lustre/orion/csc710/scratch/lvkale/s3ab/5326926/traces/`
(cpu-szON, cpu-szOFF, gpu-szON, gpu-szOFF; 896/896 log.gz and .sumd on each,
725–823 MB apparent per arm). Nothing pushed; trees clean at the landed refs.
