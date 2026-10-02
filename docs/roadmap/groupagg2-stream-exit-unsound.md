# XpGroupAgg2 STREAM page-skip is unsound on non-monotonic data

**Status: FIXED in `a3a6ba5`. Found 2026-10-01 auditing the correlation cost
gate; closed the same day.** Regression:
`test/reproducers/groupagg2_desc_stream_exit.sh`, 14/14. What was done and what
was deliberately not done is recorded in R1-11 of
`docs/roadmap/silent-drop-reachability.md`; this file is kept as the account of
the defect itself.
Reproducer: `extension/xp_batch/test/reproducers/groupagg2_desc_stream_exit.sh`
(fails on purpose: 7 correct, 5 wrong). Full analysis:
`docs/GROUPAGG2_COST_GATE_AUDIT.md`.

## What is wrong

`XpGroupAgg2`'s Class-1 page skip terminates the scan when a page's leading key
exceeds the predicate's upper bound (`can_early_exit`, `xpb_groupagg2.c`). That
is sound only if the leading key is monotonically non-decreasing across the
heap. Nothing establishes that. The applicability gate accepts the path when
`fabs(pg_stats.correlation) >= 0.8`, which is neither the same condition nor a
proof of it.

Two reachable classes of silent wrong answer, both with no error raised:

| data | predicate | PostgreSQL | XpGroupAgg2 |
|---|---|---|---|
| descending (corr −1.0) | `k1 <= 500` | 501 groups, sum 100200 | **0 groups** |
| nearly sorted (corr 0.9329) | `k1 <= 500` | 501 groups, sum 100200 | 501 groups, **sum 93430** |

The second is the dangerous one: the group count is right and only the
aggregates are short, by 0.1% to 6.8% as physical order degrades from exactly
sorted to correlation 0.93 — all inside what the gate admits.

Only predicates with an **upper** bound on the leading key are affected.
Lower-bound-only predicates were not observed to lose rows; whether that is
soundness or luck is not established.

## Why the existing checks do not catch it

- The runtime STREAM detection reports `order_scope: STREAM`, "monotonic order
  detected at runtime", while rows are being dropped. It inspects only tuples
  that survived the skip, so it is vacuous for exactly the rows that were lost.
- The gate is named "STREAM feasibility gate" and its comment reasons only about
  speed. It is in fact the sole correctness guard on this path, which is why it
  must not be relaxed or replaced by a cost comparison.

## Production consequence

`xp_batch.enabled` and `xp_batch.groupagg2` are both `off` by default
(`boot_val=off`, context `user`), so a default build cannot reach this. Within
the feature it needs only ordinary data: a table loaded newest-first, or any
nearly-sorted table, plus a `<=` or `BETWEEN` predicate on the group key.

**Classification: known blocker.** Not a temporary dev guard — there is no guard;
the wrong answer is returned. The feature is not production-ready while the
correctness of its main mechanism depends on an unverified statistical property.

## Why it was not fixed in the session that found it (historical)

The minimal correct fix is inside the cost gate, and the brief that produced
this finding forbids touching the cost gate (§12) and forbids fixing the
correlation dependency (§15). The choice between the options below is
architectural, not mechanical, so it belongs to @obe / @yoda.

## Options considered (option 3 was taken: remove the skip)

1. **Narrow the gate.** `correlation >= threshold` instead of `fabs(...)`.
   Closes the descending zero-row case only. The nearly-sorted row loss survives
   at any threshold below 1.0, so this is a partial fix and should be labelled
   as one.
2. **Make the skip verify rather than assume.** Track the leading key across
   pages and abandon *the skip* — not the scan — the first time order is
   violated. The only option that makes correctness independent of a statistic,
   and the only one that justifies keeping the gate as a pure performance
   heuristic.
3. **Remove the predicate-driven page skip** until (2) exists, keeping STREAM as
   an output-ordering claim validated over tuples actually seen.

On any of them: convert the reproducer to a regression, add both cases to
`docs/roadmap/silent-drop-reachability.md` (which currently reports
`SILENT_DROP = 0` and would need the count raised honestly, as the Phase 5 rule
requires), and re-check whether `can_skip_prefix` is sound or merely lucky.

## Note for future harnesses

This defect hid behind a planner rewrite, and it briefly made its own reproducer
report all-green. When the inner aggregate's value is not consumed, the planner
removes it from the subquery target list, the node's "exactly one Aggref" guard
declines, PostgreSQL answers, and the check passes without the node ever
running. Any test of this node must consume the aggregate:

```sql
-- useless: Xp never runs
SELECT count(*)          FROM (SELECT k1,k2,sum(v) AS s FROM t WHERE ... GROUP BY k1,k2) a;
-- exercises the node
SELECT count(*), sum(s)  FROM (SELECT k1,k2,sum(v) AS s FROM t WHERE ... GROUP BY k1,k2) a;
```

---

# Second defect, found in the same session

## `EXPLAIN (ANALYZE, VERBOSE)` on XpGroupAgg2 hangs uninterruptibly

**Status: FIXED in `7a75164`, separate defect from the one above.**
Regression: `test/reproducers/groupagg2_explain_verbose.sh`, 3/3, with every
psql call wrapped in a client-side timeout because statement_timeout provably
cannot end this failure mode. Recorded as R1-12 in
`docs/roadmap/silent-drop-reachability.md`.

```sql
-- returns in 0.04 ms
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF)
  SELECT k1,k2,sum(v) FROM t WHERE k1 <= 500 GROUP BY k1,k2;

-- does not return
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF)
  SELECT k1,k2,sum(v) FROM t WHERE k1 <= 500 GROUP BY k1,k2;
```

Observed twice. One backend ran 39 minutes, a second 9 minutes, both on a
200 000-row table whose non-VERBOSE plan completes in 0.04 ms.

The loop has **no interrupt check**:

- `SET statement_timeout='20s'` does not fire;
- `pg_terminate_backend()` does not end it;
- only `pg_ctl restart -m immediate` clears it.

While wedged it holds a relation lock, so every later `DROP TABLE` on that
relation blocks behind it, and the whole test suite blocks behind those on
`IPC/ProcSignalBarrier`. That is how it was found: three gate runs appeared to
hang and were in fact queued behind one stuck `EXPLAIN VERBOSE` from an earlier
command.

Production consequence: a backend that cannot be cancelled or terminated and
holds locks is worse than a slow one. It needs a `CHECK_FOR_INTERRUPTS()` in
whatever loop the VERBOSE path enters, and then the loop's termination condition
needs to be correct on its own.

Not diagnosed further — it was blocking the gate set and the priority was to
clear it and finish the correctness report. The reproducer above is enough to
start from. **Do not run `EXPLAIN (ANALYZE, VERBOSE)` against this node until
this is fixed**; it will wedge the cluster's locks.
