# uploads/ — 2026-08-22 22:35

**START HERE: `relay90.txt`.** The −D sweep, job 5328919, all 17 arms EXACT.
Your relay89 correction is recorded in `reports/relay89.txt` §7 (local only —
you have relay89 archived, so I did not re-send it).

## The answer: the optimum is where we already are

    -D   Iter0                       walk     requests    reply MB   reply/msg
     1   4635.3 [4595.6..4675.1]    0.580 s   9,571,535     3064.9      320 B
     2   4525.7 [4494.8..4556.7]    0.468 s   4,907,015     4360.0      889 B
     3   4553.1 [4528.9..4577.3]    0.448 s   2,644,471     8734.4     3303 B
     4   4725.5 [4699.7..4751.2]    0.550 s   3,665,431    11799.5     3219 B

−D 1 and −D 4 are separated and worse. **−D 2 and −D 3 overlap** — 27 ms apart,
not resolvable at 2 reps. So the U has a flat floor and today's default sits in
it. **No speed is available from this knob.**

−D 4 is *dominated*, not merely slower: more requests than −D 3 (3.67M vs
2.64M), more bytes, far more memory. Deeper sharing stops paying before it
stops growing.

## The number that explains all of it

    -D   nodes fetched   never examined      used
     1     10,165,967      560,619  ( 5.5%)   9,605,348
     2     19,354,087    9,769,221  (50.5%)   9,584,866
     3     40,653,223   31,096,147  (76.5%)   9,557,076
     4     63,406,319   53,706,203  (84.7%)   9,700,116

**The used count is constant — ~9.6M at every depth, varying 1.5% across a
sweep that changes total fetches 6.2×.** That is the walk's intrinsic working
set and no bundling choice touches it.

Everything follows. −D 1 fetches 10.2M to use 9.6M — wastes 5.5%, moves only
3.06 GB — and is still slower, because getting there costs 9.57M requests. −D 3
wastes 76.5% and moves 8.73 GB but needs 2.64M. Exactly the trade relay89
priced: requests are 18.2% of walk wall at 14.9 µs, more PE time than
processing the replies, so the walk would rather ship bytes it never reads than
ask for them singly. **The waste is cheap; the requests are expensive.**

## What −D 2 does buy: memory, not speed

    -D   avg_MB/proc   max_MB/proc   cached_particle_MB
     2      707.0         832.8            2792.9
     3      837.7        1079.2            6315.4

At the same wall, −D 2 holds **130 MB less per process on average, 246 MB less
at peak, and moves 4.4 GB instead of 8.7 GB**. Free if memory or network ever
binds; not defensible as a speed change at 2 reps.

## What this closes

relay88 said the only remaining cache lever was "fetch fewer distinct nodes".
This sweep tried exactly that over the knob that controls it, and fetching fewer
costs more than it saves, because the working set is fixed. **The cache-fetch
reduction line is closed for time**: dedup excluded by construction, bundling
already optimal, working set invariant. What remains is making each *request*
cheaper — your item 3.

Method note: −D 3 reads 4553.1 here against relay82's 4467.2 for the same
config in another job — the ~2% between-job spread again. Only the within-job
comparisons above carry weight.

## Not done

Item 3, the aggregation measurement. It is now head of the queue, and §2 makes
it *more* interesting: with a fixed 9.6M working set and requests as the
expensive term, per-message overhead is the only cost left a design change can
reach.

## Files

    relay90.txt                the above in full
    relay90-dsweep-16n.sbatch  the job (3 binaries x 4 depths)
    build-v84tr.sh             the traced binary used for message counts
