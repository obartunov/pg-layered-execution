# Unguarded fixed hash tables that drop rows instead of erroring

Status: **superseded as a register by
`docs/roadmap/silent-drop-reachability.md`, which maps the whole extension.
All four sites here are now R1, reproduced. Not fixed yet, deliberately.**

This note remains the detailed write-up of the first four sites and of how
`local_ht` was reached. The full classification of every bounded structure in
`xp_batch` -- 31 sites, of which 8 are reproduced silent drops -- is in the map,
along with four further silent drops that are not capacity exhaustion and that a
load-factor audit could not have found.

| site | table | reachability | evidence |
|---|---|---|---|
| 1 | `xpb_projection.c` `AGG_CAP` | **R1 reachable** | `test/reproducers/silent_drop_map.sh` (16900 in, 16384 out) |
| 2 | `xpb_columnar_pipeline.c` `WHASH_CAP` | **R1 reachable** | same script (140000 in, 131072 out) |
| 3 | `xpb_groupagg2.c` `local_ht` | **R1 reachable** | `test/reproducers/local_ht_silent_drop.sh` |
| 4 | `xpb_groupagg2.c` global hash | guard sound for itself | — |

**No published result in this repository is affected by site 3.** Nothing under
`benchmarks/` or `extension/` sets `xp_batch.groupagg2_local_partial`, and it
defaults to off; the only two files that enable `xp_batch.groupagg2` at all
(`test/zlfs_v01.sql`, `test/zlfs_persistence.sh`) leave local partial off. The
broken behaviour is being kept observable until sites 1 and 2 have been answered
the same way.

Found while updating `docs/TYPED_BATCH_CONTRACT.md` for Dimension Hash Growth
v1. The milestone's own tables were the ones being changed; auditing the table
of *every* fixed-capacity hash in the extension turned up **three unguarded
tables**, plus a fourth whose guard is sound for itself but does not cover the
third. Two were found in the first pass; `xpb_groupagg2.c`'s pair was found when
review (@Teodor) rejected the first version of that table for asserting a
guarantee the file's second hash does not have.

That is the ordinal scheme used throughout this note: three sites drop rows,
and the fourth table is carried alongside them because its guard is what made
the third one easy to miss.

Deliberately kept out of that milestone: this is a correctness question on paths
outside the measured pipeline, and it deserves its own answer, like
`docs/roadmap/groupagg2-int64-overflow.md`.

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

xpb_groupagg2.c:636-650, :745-759   local_ht, per-batch, key (k1,k2)
    same shape; capacity is xp_batch.groupagg2_local_hash_cap, a GUC
    (PGC_USERSET, default 2048, min 256), alloc at :450,
    memset(local_ht, 0, ...) per batch at :627 and :738
```

This is the same defect class that was found and fixed in `xpb_v2_report.c`'s
dimension hashes: not an error, not a partial result flagged as partial, but a
sum that is quietly too small. Ten of the extension's other fixed tables
(`xpb_typed_pipeline.c`, `xpb_batch_groupagg.c`, `xpb_batch_hashjoin.c`,
`xpb_batch_partition.c`, `xpb_zlfs.c`) raise at 3/4 load before the probe loop,
which both prevents the drop and guarantees the loop terminates on a free slot.
These three have no such guard.

That statement is about **insert** loops, and it is correct. It is also
incomplete: `xpb_batch_partition.c`'s *lookup* loops return `-1` for a miss and
the caller drops on `yr < 0 || ag < 0`, so a negative dimension payload -- a
legal `int NOT NULL` with no CHECK anywhere -- silently deletes every fact row
referencing it. A load-factor audit could not find that, and this file should
not be read as vouching for `xpb_batch_partition.c` as a whole. See R1-5 in the
map.

### The fourth table: a guard that covers only itself

`xpb_groupagg2.c` holds a second hash besides `local_ht`, and it is the reason
the third site was easy to miss.

Its **global** hash has no pre-insert load check either, but a single post-scan
test above 0.95 load is sound against row loss *for that table*: `ngroups`
counts occupied slots and nothing is ever removed, so a row can only be lost
once every slot is occupied, which reports load 1.0 and raises. What makes it
worth carrying here is that its capacity comes from the planner
(`custom_private[6]`, default 16384) rather than a constant, so "can this table
fill" is a question about estimates rather than about dataset cardinality, and
it is the only one reachable through a planner hook rather than an explicit
benchmark function.

### Why the third site is the most reachable of the three

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

### R1: reproduced, 2026-09-28

`extension/xp_batch/test/reproducers/local_ht_silent_drop.sh`, raw run in
`raw/2026-09-28-local-ht-r1.txt`. 301 distinct groups, a 256-slot local hash:

```
PostgreSQL                  groups=301  sum=901  md5 1311def94a0c6016
xp_batch local_partial=off  groups=301  sum=901  md5 1311def94a0c6016
xp_batch local_partial=on   groups=256  sum=766  md5 a2b89ab2b85a4b31
```

`off` reproduces PostgreSQL bit for bit, so the divergence is the local hash and
nothing else. No error, backend alive, query returns normally.

**The reachable shape was the part that needed finding.** `xpga2_add_path()`
requires `|correlation(k1)| >= 0.8`, on the reasoning that the node only wins
when STREAM activates -- but `local_ht` is used only on the hash fallback
(`if (!is_sorted)`, :419), and `is_sorted` is decided by an optimistic probe of
**page 0 alone** (:362-390). So the shape that reaches it is a table that is
globally clustered, which is what the planner gate asks for, but whose first page
is locally out of order. One row inserted ahead of a clustered body is enough.
That is an ordinary table after a few out-of-order inserts, not a contrived one.

**Failure mode is pure loss**, measured per group against PostgreSQL: 45 groups
missing entirely, **0 groups with a wrong sum, 0 groups invented**. Survivors are
exact. The output group count saturates at exactly `local_cap` and never grows
again:

```
distinct groups   255  256  257  258  301  501
returned          255  256  256  256  256  256
```

255 is clean and 256 is clean; 257 loses exactly one. Reproduces identically at
the shipped default cap of 2048 (2101 groups in, 2048 out), so it is not an
artifact of the 256 minimum.

**Two corrections to this note's earlier guesses**, both found by running it:

* `lp_max_groups_in_batch` is **not** a usable signal. The final partial flush
  (:745-783) never updates it -- only the full-batch path at :681-682 does -- so
  on a single-batch query it stays 0 while the table is saturated.
  `LP Partials Emitted` does pin at `local_cap` and is the signal to use.
* The node is only chosen when the aggregate is **consumed downstream**. With
  `SELECT count(*) FROM (SELECT k1, k2, sum(v) ... GROUP BY ...)` the planner
  prunes `sum(v)` from the subquery tlist, `naggs != 1` at :1558 then declines,
  and the measurement silently becomes stock HashAggregate. Three blocks of the
  reproducer reported correct results for exactly this reason before it was
  found. Any test for sites 1 and 2 has to prove the node was used, not assume it.

This also contradicts the file's own stated contract at :1589 -- *"v0 contract:
correct result OR explicit decline. No silent corruption."* The NULL and
multi-aggregate guards honour it; `local_ht` does not.

Because linear probing scans all slots, a drop requires the table to be
**completely** full -- not merely heavily loaded. What "full" costs differs by
an order of magnitude between the three:

```
xpb_projection.c       AGG_CAP     16384 distinct (k1,k2)      per query
xpb_columnar_pipeline  WHASH_CAP  131072 distinct (k1,k2,k3)   per query
xpb_groupagg2 local_ht local_cap    2048 distinct (k1,k2)      PER BATCH
                                     256 at the GUC minimum
```

The per-batch scope is what makes the third one different in kind rather than
in degree: a batch holds at most 65 536 rows, so 2048 distinct keys inside one
is an ordinary shape, and 256 is close to unavoidable.

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
