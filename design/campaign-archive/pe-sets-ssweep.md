# Machine-wide `s` sweep — the answer to "have we tried s=3,4?"
# Job 5288941, 16 nodes, `-u dist`, machine-wide, **S3 off on every arm**,
# 2 reps each. ALL 24 ARMS EXACT.

No — machine-wide had only ever been run at s=2 and s=14. Here are 3,4,5,7,10,
and both modes.

| s | mode | phase1 | phaseB | uf2 | Iter0 |
|---|---|---|---|---|---|
| 1 | — | 5.089 | 3.326 | 0.460 | 8091.7 |
| 2 | blocked | 4.881 | 3.112 | 0.527 | 7922.6 |
| 2 | **round-robin** | 2.647 | 0.877 | 0.541 | 5791.3 |
| 3 | blocked | 4.567 | 2.792 | 0.485 | 7617.5 |
| 3 | **round-robin** | 2.207 | 0.380 | 0.603 | 5419.8 |
| 4 | blocked | 3.446 | 1.650 | 0.494 | 6480.7 |
| 4 | **round-robin** | 2.154 | 0.230 | 0.641 | 5403.4 |
| 5 | blocked | 2.689 | 0.855 | 0.494 | 5733.4 |
| 7 | blocked | 2.864 | 1.085 | 0.527 | 5970.6 |
| 7 | **round-robin** | **2.115** | 0.181 | 0.635 | **5365.9** |
| 10 | blocked | 2.236 | 0.489 | 0.597 | 5433.9 |
| 14 | (either) | 2.115 | 0.001 | 0.671 | 5420.1 |

Note `s=1` here is the CRIPPLED baseline — no split AND no stealing — not the
production baseline. The serial/S3-on reference is 6480 ms (job 5288750).

## 1. Intermediate s works, and s=14 is not special

s=4, 7, 10 and 14 (round-robin) are **5403 / 5366 / 5434 / 5420 ms** — a 68 ms
spread over four configurations, which is the run-to-run noise at 2 reps. s=3
round-robin is 5420. **Anything from s=3 upward with round-robin is equivalent.**
s=7 round-robin is nominally best and is not distinguishable from the rest.

So the s=14 result is not a knife-edge and does not depend on "one PE per set".
What matters is only that enough pair weight crosses a set boundary.

## 2. MODE is the variable that actually matters, not s

At every intermediate s, round-robin beats blocked by a wide margin:

| s | blocked Iter0 | round-robin Iter0 | Δ |
|---|---|---|---|
| 2 | 7922.6 | 5791.3 | **−2131** |
| 3 | 7617.5 | 5419.8 | **−2198** |
| 4 | 6480.7 | 5403.4 | **−1077** |
| 7 | 5970.6 | 5365.9 | **−605** |

The mechanism is visible in the phaseB column. Blocked keeps SFC-adjacent PEs
in the same set, so very little pair weight crosses a boundary and very little
phaseB work leaves: at s=2 blocked, phaseB is 3.112 s against a baseline of
3.326 s — it barely split anything. Round-robin puts adjacent PEs in different
sets and removes most of it (0.877 s). The two modes converge as s → 14, where
every PE is its own set and the distinction disappears.

**Practical consequence: `FOF_PE_SETS_MODE=1` is not a tuning preference, it is
required for any s < 14.** A blocked split at small s looks like the feature
doing nothing.

## 3. The two costs, and where they balance

- **phaseB falls fast and saturates**: 3.326 → 0.877 (s=2) → 0.380 (s=3) →
  0.230 (s=4) → 0.181 (s=7) → 0.001 (s=14). Almost all of the available saving
  is already taken by s=3-4.
- **uf2 rises slowly and monotonically**: 0.460 → 0.541 → 0.603 → 0.641 →
  0.635 → 0.671. Over the whole range that is only ~210 ms, because `-u dist`
  absorbs the fragment inflation (in `-u serial` the same range costs 1.6 s).

They cross in a flat basin around s=4-7. There is no sharp optimum, which is a
good property: the knob does not need tuning per problem.

## 4. S3 only becomes free once phaseB is nearly gone

Comparing this job (S3 off) against job 5288750 (S3 on) at the same
configuration:

| config | S3 on | S3 off |
|---|---|---|
| s=2 round-robin | 5601.3 | 5791.3 |
| s=14 | 5413.5 | 5379.4 / 5420.1 |

At s=2 there is still 0.88 s of phaseB left, and stealing still earns its
keep — turning it off costs 190 ms. At s=14 phaseB is 0.001 s, there is
nothing to steal, and turning it off is free or slightly better.

**So the two knobs are coupled**: `FOF_S3=0` is only safe at large s. At s=3-4
keep stealing on.

## 5. Revised recommendation

```
-u dist   FOF_PE_SETS=7   FOF_PE_SETS_MODE=1   FOF_S3=0
```
or `FOF_PE_SETS=14` (mode irrelevant), which is within noise of it.
If a smaller s is wanted for any reason, use mode 1 and keep `FOF_S3=1`.
