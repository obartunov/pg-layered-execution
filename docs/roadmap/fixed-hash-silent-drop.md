# Unguarded fixed hash tables that drop rows instead of erroring

Status: **open, reachability unproven. Starts with a reproducer, not a fix.**

Found while updating `docs/TYPED_BATCH_CONTRACT.md` for Dimension Hash Growth
v1. The milestone's own tables were the ones being changed; auditing the table
of *every* fixed-capacity hash in the extension turned up three that are not
merely capped but unguarded, and a fourth whose guard is sound only for part of
what it appears to cover. Two were found in the first pass; `xpb_groupagg2.c`'s
pair was found when review (@Teodor) rejected the first version of that table
for asserting a guarantee the file's second hash does not have. Deliberately kept out of that milestone: this is a
correctness question on paths outside the measured pipeline, and it deserves its
own answer, like `docs/roadmap/groupagg2-int64-overflow.md`.

## The shape of it

Three sites probe every slot and then fall off the end of the loop with no
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

`xpb_groupagg2.c` holds a third and a fourth table, and they are not the same
case as each other.

Its **global** hash has no pre-insert load check either, but a single post-scan
test above 0.95 load is sound against row loss *for that table*: `ngroups`
counts occupied slots and nothing is ever removed, so a row can only be lost
once every slot is occupied, which reports load 1.0 and raises. What makes it
worth carrying here is that its capacity comes from the planner
(`custom_private[6]`, default 16384) rather than a constant, so "can this table
fill" is a question about estimates rather than about dataset cardinality, and
it is the only one reachable through a planner hook rather than an explicit
benchmark function.

Its **per-batch local** hash is a genuine fourth unguarded site, and the most
reachable of the four:

```
xpb_groupagg2.c:450        local_ht = palloc0(local_cap * sizeof(CGroupEntry))
                           local_cap = xp_batch.groupagg2_local_hash_cap
                                       GUC, PGC_USERSET, default 2048, min 256
xpb_groupagg2.c:636-650    insert loop, same fall-off-the-end shape
xpb_groupagg2.c:745-759    second insert loop, same shape
xpb_groupagg2.c:627,738    memset(local_ht, 0, ...) per batch
```

`local_ngroups` is never compared to `local_cap` — it feeds only
`lp_partials_emitted` and `lp_max_groups_in_batch`. Because the table is cleared
per batch, the fill threshold is 2048 distinct `(k1, k2)` pairs **inside one
batch** of up to 65 536 rows, not across the query, which is an ordinary shape
rather than an extreme one. And a row dropped in the local hash never reaches
the merge into the global hash, so `ngroups` is unaffected and the 0.95
post-scan check cannot fire. Loss in the *merge* loop is caught, because that
inserts into the global table and bumps `ngroups`; loss in the local hash is
not.

It is gated behind `xp_batch.groupagg2_local_partial`, which defaults to off —
but that is a `PGC_USERSET` boolean, so a session enabling it is not a build
change or an administrative act.

One useful consequence for the reproducer: `lp_max_groups_in_batch` pins at
`local_cap` exactly when the table saturates, so the observable already exists
and nothing currently checks it. That is the cheapest available signal for step
1 on this site.

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
* `local_ht` is the exception to all of this: it does not read `reg_buh`, its
  capacity is a GUC rather than a constant, and because it is cleared per batch
  its threshold is per-batch key cardinality. Its reachability question is
  therefore independent of the other two and should be answered separately.

So the first question is not how to fix it but at what `reg_buh` shape it
happens, and whether any shape used in the repository already crosses it. If a
published benchmark number was produced past that point, the number is wrong,
not just the code.

## The task

1. **Reproducer first**, per site. Populate `reg_buh` so that
   `projection_experiment` sees 16384 distinct `(pk, ck)` pairs, and show the
   aggregate disagreeing with the equivalent SQL `GROUP BY`. Per-group
   comparison, not grand totals -- a dropped key keeps the row count of the
   *output* plausible. For `local_ht` the reproducer is different and cheaper:
   `SET xp_batch.groupagg2_local_partial = on`, drive more than
   `groupagg2_local_hash_cap` distinct `(k1, k2)` pairs through one batch, and
   watch `lp_max_groups_in_batch` pin at `local_cap` while the sum goes short.
   Start with the minimum cap of 256 to keep the dataset small.
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
   any shape that currently completes. `local_ht` is a third shape again -- it
   exists to be L1-resident, so growing it defeats its purpose, and spilling the
   overflow straight into the global hash is the obvious candidate rather than
   either a refusal or a doubling.
4. A regression that fails without the fix, pinning the arithmetic and not only
   the absence of an error.

## What must not happen

Do not close `local_ht` by documenting the GUC as unsafe and leaving it. It
defaults to off, which limits exposure, but `PGC_USERSET` means a session
enabling it is not a build change or an administrative act, and the failure is a
wrong answer rather than an error.

Do not silently raise `AGG_CAP` or `WHASH_CAP`. That moves the boundary without
closing it, invalidates the published timings for those paths, and leaves the
same silent-wrong-answer shape in place one order of magnitude further out.

Do not fix this inside a benchmark milestone. Both functions are timed in
published results; a correctness change to them needs its own commit and its own
before/after, or the next regression in those numbers will not be attributable.
