# Unchecked int64 accumulation in XpGroupAgg2, feeding int8_numeric()

Status: **open, reachability unproven. Starts with a reproducer, not a fix.**

Raised by review (@Teodor) while auditing the Hash Aggregate Growth v1 series.
Deliberately kept out of that series: the same class of defect was fixed there
in `xpb_v2_report.c`, but this path is different, is not on the 05-A..05-E
measurement path, and nobody has yet shown it can be reached from SQL.

## The shape of it

`xpb_groupagg2.c` accumulates a group's sum into a plain `int64` with no
overflow check:

```
xpb_groupagg2.c:481,647    int64 val = ops->getattr_fn(...)
                           sum_val += val            /* unchecked */
xpb_groupagg2.c:306-308    if (agg_outtype == NUMERICOID)
                               result = int8_numeric(sum_val)
```

So when the aggregate's declared output type is `numeric`, a wrapped `int64`
is converted *into* a numeric:

```
sum(bigint)
   |
int64 accumulator
   |   overflow possible, undefined behaviour, no error
   v
wrapped int64
   |
int8_numeric()
   v
plausible but wrong numeric
```

This is precisely the case PostgreSQL makes `sum(bigint)` return `numeric` to
avoid. A numeric result carries no hint that it was built from a value that had
already wrapped, so the failure is silent and the answer looks well-typed.

## Why it is not being fixed yet

The equivalent defect in `xpb_v2_report.c` was demonstrated with a four-row
reproducer before it was touched. This one has not been:

* `xpb_batch_hashjoin.c:1020` has the same unchecked shape but takes int4
  inputs into an int64 accumulator, so it needs on the order of 4.3e9 rows.
* `xpb_1c_register_report`, the twin of the defect already fixed, refused an
  int8 reproducer outright with `debit is type 20, integer required` -- its
  type gate appears to block the input that would be needed.

So the first question is not how to fix it but whether it can happen.

## The task

1. **Reproducer first.** Find a SQL path that reaches `XpGroupAgg2` with
   `agg_outtype == NUMERICOID` and an input whose running sum can leave int64
   range. Check what the type gates on the way in actually admit, and whether
   the numeric output type can be selected while the accumulator stays int64.
2. If such a path exists, it is a **correctness blocker** and gets its own fix
   and its own commit, in the shape used for `xpb_v2_report.c`: checked
   accumulation raising `bigint out of range` with PostgreSQL's errcode, plus a
   regression that fails without the fix.
3. If no such path exists, say so with the evidence -- which type gate closes
   it -- and either add an assertion that records the invariant or note the
   accumulator as int4-input-only at its definition. An unreachable defect that
   nobody has written down is a defect waiting for the gate to be relaxed.
4. Either way, audit the remaining accumulation sites in the same pass. The
   review counted 15 outside `xpb_v2_report.c`; most take int4 into int64 and
   are harmless at any plausible row count, but "most" is not a list.

## What must not happen

Do not fix this by making the accumulator numeric, or by widening it to
int128, before step 1. Both change the measured cost of a path that several
published benchmarks report, and neither is justified by a defect that has not
been shown to be reachable.
