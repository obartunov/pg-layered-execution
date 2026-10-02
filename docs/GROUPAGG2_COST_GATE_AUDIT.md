# XpGroupAgg2 Cost-Gate Audit

Starting point `3c84449`. Audit only: no cost constant, gate, hint or statistic
was changed in any measurement that a conclusion rests on. One forced
diagnostic is used and is labelled as such.

**The audit did not find a cost-model question. It found a correctness defect,
and the gate under audit is what was holding it back.** That changes the
character of this document: the planner question is answered, but the answer is
that the gate is load-bearing for correctness and unsound as written.

## Symptom

Recorded at the end of the previous phase:

> The same query over 1,000 groups in 2,000,000 rows was accepted for
> `k1 = g/2000` and refused for `k1 = g%1000`.

That observation was attributed to key correlation. **The attribution was not
isolated**: the two tables differed in the key expression *and* in the `tag`
column (`'keep'`/`'skip'` against `'x'`), which changes tuple width and so
`relpages` — and `relpages` is an input to the Xp cost floor. The earlier
document stated correlation as the cause without having separated the two.

## Minimal reproducer

`extension/xp_batch/test/reproducers/groupagg2_desc_stream_exit.sh`.
It fails on purpose until the defect is closed: 7 correct, 5 wrong.

Two tables, byte-identical contents, differing only in physical row order;
1,000 groups of 200 rows, 200,000 rows:

```sql
INSERT INTO ga_asc  SELECT k1,0,1 FROM (...) ORDER BY k1;       -- correlation  1.0
INSERT INTO ga_desc SELECT k1,0,1 FROM (...) ORDER BY k1 DESC;  -- correlation -1.0

SELECT count(*), sum(s) FROM (SELECT k1,k2,sum(v) AS s FROM ga_desc
                              WHERE k1 <= 500 GROUP BY k1,k2) a;
--  PostgreSQL : 501 | 100200
--  XpGroupAgg2:   0 | null        no error
```

One trap worth stating, because it hid the defect for several attempts and
briefly made this very script report 8/8 green: **the aggregate must be
consumed.** With only `count(*)` outside, the planner removes the unused inner
`sum(v)`, the node's "exactly one Aggref" guard then declines, PostgreSQL
answers, and the check passes without the node ever running.

## Statistics, and what is held fixed

The correlation ladder varies correlation alone. Every rung holds the same row
count, the same `k1` value multiset (0..999, each exactly 2000 times), the same
NDV, and `tag` as a function of `k1` so the stored rows are byte-identical.
Confirmed from `pg_class` / `pg_stats`:

| rung | correlation | n_distinct | relpages | est rows | est groups |
|---|---|---|---|---|---|
| asc | 1.0000 | 1000 | 12739 | 1001657 | 1000 |
| blocks100 | 0.1053 | 1000 | 12739 | — | — |
| blocks10 | 0.1101 | 1000 | 12739 | — | — |
| jitter | 0.9817 | 1000 | 12739 | 997342 | 1000 |
| random | 0.0065 | 1000 | 12739 | — | — |
| desc | −1.0000 | 1000 | 12739 | 986326 | 1000 |

`relpages` is identical at 12739 on every rung, so the earlier `relpages`
hypothesis is ruled out for this ladder. `n_distinct` and the estimated group
count are identical. The row estimate varies by 1.5%, which is sampling noise
in the histogram, not a mechanism.

## Planner estimates and the cost formula

The cost floor (`xpb_cost.c`) takes four inputs and **contains no correlation
term**:

```c
c1_scan   = relpages * seq_page_cost  * 0.95 + relrows * cpu_tuple_cost * 0.85
c2_group  = relrows  * cpu_operator_cost * 0.80
c3_output = ngroups  * cpu_tuple_cost    * 1.00
c4_qual   = npreds * relrows * cpu_operator_cost * 0.90
```

So correlation cannot reach the cost through the formula. And it does not need
to: **for the refused rungs the formula is never evaluated.** The `DEBUG2` line
that `xpga2_add_path` emits on every costing is absent for them:

```
asc       :: xpga2 cost: C1=20616 C2=2003 C3=10 C4=2254 total=24883  pages=12739 nrows=1001657 ngroups=1000
blocks100 :: (nothing)
blocks10  :: (nothing)
jitter    :: xpga2 cost: C1=20579 C2=1995 C3=10 C4=2244 total=24828  pages=12739 nrows=997342  ngroups=1000
random    :: (nothing)
desc      :: xpga2 cost: C1=20486 C2=1973 C3=10 C4=2219 total=24688  pages=12739 nrows=986326  ngroups=1000
```

## The decision point

`xpb_groupagg2.c`, in `xpga2_add_path`, before any cost is computed:

```c
/* STREAM feasibility gate.
 *
 * XpGroupAgg2 wins only when STREAM activates (k1 correlated with
 * heap order). When STREAM fails -> hash fallback -> slower than
 * vanilla HashAgg. Use heap correlation of k1 column as proxy.
 * Threshold: |correlation| >= 0.8 -> STREAM likely -> accept.
 */
...
if (fabs(k1_corr) < 0.8)
{
    elog(DEBUG2, "... declined — k1 attno=%d correlation=%.3f < 0.8, STREAM unlikely", ...);
    return;                     /* <-- before add_path(), before costing */
}
```

The chain, end to end:

```
pg_statistic STATISTIC_KIND_CORRELATION for k1
        -> SearchSysCache3(STATRELATTINH, relid, k1att, false)
        -> get_attstatsslot(..., STATISTIC_KIND_CORRELATION, ...)
        -> fabs(k1_corr) < 0.8
        -> return                       no path is ever offered
        -> add_path() not called, cost floor not computed
        -> PostgreSQL's HashAggregate wins by default, not by comparison
```

That fully explains the ladder, including the part the earlier note got wrong:
acceptance is **not** monotonic in correlation. `fabs` means −1.0 passes exactly
as 1.0 does, which is why `desc` is accepted.

## Correlation ladder — decisions

| rung | correlation | decision | Xp total cost | winning cost |
|---|---|---|---|---|
| asc | 1.0000 | **accepted** `Custom Scan (XpGroupAgg2)` | 24883 | 24883 |
| jitter | 0.9817 | **accepted** | 24828 | 24828 |
| desc | −1.0000 | **accepted** | 24688 | 24688 |
| blocks10 | 0.1101 | refused → `HashAggregate` | not computed | 45283 |
| blocks100 | 0.1053 | refused → `HashAggregate` | not computed | 45364 |
| random | 0.0065 | refused → `HashAggregate` | not computed | 45315 |

The decision boundary is sharp, reproducible, and exactly `|correlation| = 0.8`.
Where the path is offered it always wins, by a factor of 1.8 on estimated cost.

## Actual execution cost — and why §15's comparison cannot be made

§15 asked whether correlation changes the real executor cost (gate is
reasonable) or only the estimate (correlation is a bad proxy). Neither answer
applies, because on the accepted side of the boundary the node does not always
produce the right answer.

### Descending order, accepted, zero rows

```
Custom Scan (XpGroupAgg2) (actual rows=0.00 loops=1)
  GroupAgg Path: StreamingAgg (STREAM, dual-key)
  BatchProperties.order_scope: STREAM
  Pages Total: 1082
  Pages Skipped (STREAM exit): 1081
  Pages Scanned: 2
  Tuples Visited: 0
```

The mechanism is complete in those counters:

1. The gate admits correlation −1.0 because it tests `fabs`.
2. The node's STREAM contract requires `(k1,k2)` **non-decreasing**
   (`xpb_groupagg2.c` header, `order_scope = STREAM`).
3. The Class-1 early exit stops the scan when a page's leading key exceeds
   `stream_key_hi`. That rule is sound only for non-decreasing order.
4. On descending data the *first* page holds the maximum key, so the scan
   terminates having visited zero tuples.
5. Runtime STREAM detection then reports `order_scope: STREAM`, "monotonic order
   detected at runtime" — over an empty tuple set. The detection only inspects
   tuples that survived the skip, so it is vacuous for exactly the rows that
   were lost.

Scope by predicate shape, same two tables:

| table | predicate | PostgreSQL | Xp |
|---|---|---|---|
| ga_asc | `k1 <= 500` | 501 \| 100200 | 501 \| 100200 |
| ga_asc | `k1 BETWEEN 200 AND 300` | 101 \| 20200 | 101 \| 20200 |
| ga_asc | `k1 >= 500` | 500 \| 100000 | 500 \| 100000 |
| ga_desc | `k1 <= 500` | 501 \| 100200 | **0 \| null** |
| ga_desc | `k1 BETWEEN 200 AND 300` | 101 \| 20200 | **0 \| null** |
| ga_desc | `k1 >= 500` | 500 \| 100000 | 500 \| 100000 |
| ga_desc | `k1 >= 0` | 1000 \| 200000 | 1000 \| 200000 |

Only predicates carrying an **upper** bound on the leading key are affected.
Lower-bound-only predicates survive because prefix-skip is a per-page decision
while early-exit terminates the scan.

### Nearly-sorted data, accepted with no forcing, partial row loss

This is the serious case. The gate's test is `|correlation| >= 0.8`; the early
exit needs **monotonic**. Those are different conditions, and the gap between
them loses rows in proportion to the deviation. Physical order jittered by
adding noise to the sort key, nothing forced:

| jitter | correlation | PostgreSQL | Xp | rows lost |
|---|---|---|---|---|
| 0 | 1.0000 | 501 \| 100200 | 501 \| 100200 | 0 |
| 5 | 1.0000 | 501 \| 100200 | 501 \| 100103 | 0.1% |
| 20 | 0.9998 | 501 \| 100200 | 501 \| 99728 | 0.5% |
| 100 | 0.9953 | 501 \| 100200 | 501 \| 98697 | 1.5% |
| 400 | 0.9329 | 501 \| 100200 | 501 \| 93430 | **6.8%** |

The group count is correct — 501 — at every rung. Only the aggregates are
wrong. A plausible row count with quietly wrong sums and no error is the worst
available failure shape; the empty result in the descending case is at least
obvious. At jitter 5 the correlation still rounds to 1.0000 and 97 rows are
already gone.

### Forced diagnostic, labelled

**FORCED — not used for any conclusion about a planner decision.** To establish
whether the gate is what holds the defect back, the stored correlation of a
randomly-ordered table was overwritten in `pg_statistic` inside a transaction
that was rolled back, leaving the physical data untouched:

```
natural correlation = -0.0054
PostgreSQL          : 501 | 100200
Xp, gate refuses    : 501 | 100200      (PostgreSQL answered)
forced correlation  = 1.0000
Xp, gate FORCED open: 501 | 75235       25% of rows lost
after rollback      = -0.0054
```

So the gate is the only thing standing between this node and wrong answers on
uncorrelated data.

## Diagnosis

Not the three options §16 offered.

```
estimator problem        NO   estimated groups = 1000 = actual, on every rung
cost-model problem       NO   the cost formula is never evaluated for refused
                              shapes, and where it is, the Xp path wins 1.8x
legitimate cost effect   NO   the difference is not cost, it is correctness
```

**The finding is a correctness defect, and the gate is a correctness guard
wearing a performance name.**

- It is named "STREAM feasibility gate" and its comment reasons entirely about
  speed ("STREAM fails → hash fallback → slower than vanilla HashAgg"). It is in
  fact the only check preventing silent row loss.
- It is **unsound in sign**: `fabs()` admits anti-correlated data, where the
  node's own STREAM definition cannot hold.
- It is **unsound as a threshold**: `|correlation| >= 0.8` does not imply
  monotonic, and the early exit requires monotonic. Everything between
  "correlated" and "monotonic" loses rows silently.
- The runtime STREAM check cannot catch either case, because it inspects only
  the tuples that were not skipped.

Exposure is bounded but not zero: `xp_batch.enabled` and `xp_batch.groupagg2`
are both `off` by default (`boot_val=off`, context `user`), so a default build
is unaffected. Within the feature, the defect needs only ordinary data —
descending physical order, or any nearly-sorted table — and the node's main
advertised mechanism is what breaks.

## Conclusion

The correlation dependency is real and reproducible, and the chain from
`pg_statistic` to the decision is a single `fabs(corr) < 0.8` test that runs
before costing. The planner question §11 asked is answered.

But the answer makes the original framing obsolete. The gate should not be
relaxed, retuned, or replaced by a cost comparison, because it is not currently
a performance heuristic — removing or loosening it widens a silent-drop path.
Equally it cannot stay as it is, because it already admits two classes of wrong
answer.

Per the project rule, this is stated as a blocker rather than fixed here: the
brief forbids touching the cost gate (§12) and the minimal correct fix lives
inside it.

## Resolution

Option 3 was taken: the predicate-driven page skip is gone (`a3a6ba5`), and the
`EXPLAIN (VERBOSE)` hang was a separate defect with a separate fix (`7a75164`).
Both are recorded as R1-11 and R1-12 in
`docs/roadmap/silent-drop-reachability.md`, with regressions at 14/14 and 3/3.

The correlation gate was left byte-identical. It is no longer load-bearing for
correctness, so it is now what its name always claimed: a performance heuristic
choosing between STREAM and hash. That it has never been *validated* as a
performance heuristic is a separate, open question — the 0.8 threshold and the
`fabs` are still unexamined on that axis, and nothing here measured whether the
node actually loses to `HashAggregate` below 0.8.

The open question this audit raised about `can_skip_prefix` — sound or merely
lucky — was settled while fixing: **unsound**. It compared only the first and
last tuple on the page, so a middle tuple inside the range on an unordered page
was skipped. It is removed, not repaired.

## Still open, not blockers

- The 0.8 gate as a *performance* heuristic is unmeasured. A path that is
  refused is never costed, so the planner cannot be said to have compared
  anything.
- `SELECT sum(v) FROM t WHERE k1 <= 500` is refused although `XpPageAgg` covers
  it semantically. Coverage is not the gap there; selection is.
- A sound page skip would need real per-page bounds, which means visiting every
  tuple on the page. Whether that is worth it is a measurement question that
  nothing in this phase answers.
