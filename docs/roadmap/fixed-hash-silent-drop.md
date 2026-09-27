# Unguarded fixed hash tables that drop rows instead of erroring

Status: **open, reachability unproven. Starts with a reproducer, not a fix.**

Found while updating `docs/TYPED_BATCH_CONTRACT.md` for Dimension Hash Growth
v1. The milestone's own tables were the ones being changed; auditing the table
of *every* fixed-capacity hash in the extension turned up two that are not
merely capped but unguarded. Deliberately kept out of that milestone: this is a
correctness question on paths outside the measured pipeline, and it deserves its
own answer, like `docs/roadmap/groupagg2-int64-overflow.md`.

## The shape of it

Both sites probe every slot and then fall off the end of the loop with no
`else`, so a row whose key is absent from a full table is discarded:

```
xpb_projection.c:51-65        agg_insert(), AGG_CAP 16384, key (k1,k2)
    for (pr = 0; pr < AGG_CAP; pr++) {
        if (!ht[idx].occ)               { insert; return; }
        if (ht[idx].k1==k1 && ...)      { accumulate; return; }
    }
    /* falls through -- row silently discarded */

xpb_columnar_pipeline.c:107-121   window hash, WHASH_CAP 131072, key (k1,k2,k3)
    same shape; on fall-through ngroups is not incremented either
```

This is the same defect class that was found and fixed in `xpb_v2_report.c`'s
dimension hashes: not an error, not a partial result flagged as partial, but a
sum that is quietly too small. Ten of the extension's other fixed tables
(`xpb_typed_pipeline.c`, `xpb_batch_groupagg.c`, `xpb_batch_hashjoin.c`,
`xpb_batch_partition.c`, `xpb_zlfs.c`) raise at 3/4 load before the probe loop,
which both prevents the drop and guarantees the loop terminates on a free slot.
These two have no such guard.

`xpb_groupagg2.c` is a third case and belongs in the same task, but it is not
the same defect. Its insert loops have no load check either, yet a single
post-scan test errors above 0.95 load, and that is sound against row loss: a
lost row cannot raise `ngroups` past `hash_cap`, so a full table reports load
1.0 and raises. What makes it worth carrying here is that its capacity comes
from the planner (`custom_private[6]`, default 16384) rather than a constant,
so the question "can this table fill" has a different shape -- it is a question
about estimates, not about dataset cardinality -- and it is the only one of the
three reachable through a planner hook rather than an explicit benchmark
function.

Because linear probing here scans all slots, a drop requires the table to be
**completely** full, i.e. 16384 or 131072 distinct keys respectively -- not
merely heavily loaded.

## Why it is not being fixed yet

Both functions are SQL-callable (`projection_experiment(lo,hi)` and
`ctr_pipeline(lo,hi)`, declared in
`extension/xp_batch/sql/xp_batch--1.0.sql`), so reaching the
*code* is trivial. What is unproven is whether the key cardinality can reach the
capacity:

* Both read the fixed benchmark table `reg_buh` by name
  (`RelnameGetRelid("reg_buh")`), not an arbitrary relation. The cardinality is
  therefore a property of whatever `reg_buh` is loaded with, which the caller
  controls.
* `agg_insert` groups on `(pk, ck)` at 16384 slots. Whether the benchmark 02/04
  `reg_buh` shapes reach 16384 distinct pairs has not been measured.
* The window hash groups on three keys at 131072 slots, eight times the
  headroom.

So the first question is not how to fix it but at what `reg_buh` shape it
happens, and whether any shape used in the repository already crosses it. If a
published benchmark number was produced past that point, the number is wrong,
not just the code.

## The task

1. **Reproducer first.** Populate `reg_buh` so that `projection_experiment`
   sees 16384 distinct `(pk, ck)` pairs, and show the aggregate disagreeing
   with the equivalent SQL `GROUP BY`. Per-group comparison, not grand totals --
   a dropped key keeps the row count of the *output* plausible.
2. **Check the shipped shapes.** Count distinct keys in every `reg_buh` that
   the benchmark scripts in `benchmarks/` build. If any is at or above the cap,
   the affected published numbers are named explicitly and corrected or
   withdrawn. This step is not optional and must come before the fix, because
   after the fix the old behaviour is no longer observable.
3. **Then the fix**, in the shape already used for `xpb_v2_report.c`: a guard
   that refuses before the table can fill, or growth under the same
   preregistered 0.5/double policy. Which of the two is a design decision, not a
   detail: `xpb_projection.c` and `xpb_columnar_pipeline.c` are timed paths in
   benchmarks 02 and 05-A, so introducing growth changes a measured cost and
   needs its own paired control, whereas a refusal does not change the timing of
   any shape that currently completes.
4. A regression that fails without the fix, pinning the arithmetic and not only
   the absence of an error.

## What must not happen

Do not silently raise `AGG_CAP` or `WHASH_CAP`. That moves the boundary without
closing it, invalidates the published timings for those paths, and leaves the
same silent-wrong-answer shape in place one order of magnitude further out.

Do not fix this inside a benchmark milestone. Both functions are timed in
published results; a correctness change to them needs its own commit and its own
before/after, or the next regression in those numbers will not be attributable.
