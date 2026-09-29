# Silent-Drop Reachability — the map

Status: **map complete, no fixes applied.** Phase 4 of the audit. Nothing in
this document has been repaired; the reproducers are kept failing on purpose so
that the broken behaviour stays observable until each site is closed.

Starting point: checkpoint `cf0c891`. Benchmark 05 is a reviewed boundary and is
not revisited here.

The question asked of every site:

> Can any input valid under SQL and the Typed Batch Contract produce, instead of
> a result or an explicit ERROR, a **silent** change to the answer because a
> local capacity limit was reached?

## Result

```
bounded sites examined        31
  SAFE_GROW                    5
  SAFE_ERROR                  13
  UNREACHABLE                  3   (each with the enforcing code named)
  SILENT_DROP                  8   (all reproduced against PostgreSQL)
  AMBIGUOUS                    4
```

The audit did not stop at capacity. Four of the eight silent drops are capacity
exhaustion; the other four are a **sentinel or a predicate** being dropped, found
by the same sweep and causing the same harm — a valid query, no ERROR, a wrong
answer. A map that listed only the hash tables would have been misleading, so
they are in it, marked.

**The most important finding is not a hash table.** Two independent paths
silently discard predicates they cannot push down, and then answer the query as
if the predicate had been applied. `SELECT count(*) FROM t WHERE name='x'`
returns the count of the whole table. That needs no cardinality at all — one
`SET` and an ordinary query.

Runtime evidence for every R1 below:
`extension/xp_batch/test/reproducers/silent_drop_map.sh`, raw run in
`raw/2026-09-30-silent-drop-map.txt`. Two control cases in the same script stay
green, so the harness is not simply always-red.

---

## SILENT_DROP — reproduced (8)

| # | site | file | cap source | want / got |
|---|---|---|---|---|
| R1-1 | `agg_insert` | `xpb_projection.c:50-65` | `AGG_CAP` 16384 | 16900 / **16384** |
| R1-2 | stage-1 `whash` | `xpb_columnar_pipeline.c:105-120` | `WHASH_CAP` 131072 | 140000 / **131072** |
| R1-3 | stage-2 `s2` ×2 | `xpb_columnar_pipeline.c:152-166, 196-211` | `S2CAP` 16384 | 16900 / **16384** |
| R1-4 | `local_ht` | `xpb_groupagg2.c:636-650, 745-759` | GUC, default 2048 | 301 / **256** groups |
| R1-5 | `pdim_year`/`adim_group` | `xpb_batch_partition.c:283-301` | `-1` sentinel | 400 / **200** |
| R1-6 | residual quals | `xpb_pageagg.c:672-713, 748` | `SQ_MAX_PREDS` 8 + any non-pushable | 2000 / **20000** |
| R1-7 | page min/max skip | `xpb_pageagg.c:393, 470-494` | wrong column | 21929000 / **0** |
| R1-8 | residual quals | `xpb_groupagg2.c:1744, 1798` | `XPB_MAX_PREDS` 8 + any non-pushable | 501 / **5010** |

### The capacity four (R1-1 … R1-4)

All four are the same shape: an open-addressed table that probes every slot and
then **falls off the end of the loop with no `else`**. There is no sentinel to
misread — "table full" and "row processed" are the same control flow and produce
the same absent observable.

`R1-1` and `R1-3` are especially quiet: both arms of the A/B experiment they
live in drop the same rows, so the experiment's own cross-check agrees with
itself while both halves are wrong. `R1-2` does not even increment `ngroups` on
the dropped row, so the reported group count stays consistent with the truncated
table.

`S2CAP` (R1-3) **was not in any previous audit.** It is declared inside a
function body as a bare `#define` and `#undef`'d twice, which is why symbol
sweeps for `*_CAP` at file scope missed it. Its only statement of an invariant
is a comment — `/* Hash into (period×company) = max 6000 slots */` — describing
the benchmark dataset, not anything enforced.

### The other four

`R1-5` — `pdim_year`/`adim_group` return `-1` for "no such key", and the caller
drops on `yr < 0 || ag < 0`. `dim_account.account_group` is plain `int NOT NULL`
with no CHECK anywhere in the repository, so a negative payload is read as a
join miss and every fact row referencing it vanishes. The test is `< 0`, not
`== -1`, so the hole is wider than the sentinel. The sibling implementations in
`xpb_batch_hashjoin.c` do this correctly — `dim_lookup` returns a pointer,
`dim2_lookup` carries `bool *found`; the partition copies dropped the found-flag
and invented a sentinel.

`R1-6` and `R1-8` — the same defect in two files. Both extract pushable
`Var op Const` predicates into a fixed array, set a `has_residual` flag for
everything they cannot represent, and then take the path anyway. Neither ever
re-applies the residual: `xpb_pageagg.c:759-768` and `xpb_groupagg2.c:1798` both
leave `scan.plan.qual = NIL` with `scanrelid = 0`, and neither calls `ExecQual`.
`xpb_groupagg2.c:1744` refuses only when *nothing* was pushable
(`has_residual && npreds == 0`); the comment above it describes the condition the
code does not implement. Across the whole tree, **no caller of `xpb_qual` refuses
on `has_residual` when `npreds > 0`** — its only other consumer is EXPLAIN, which
prints it.

The capacity limit is one entrance to this defect and the narrower one: the 9th
predicate is dropped because the array is full, but the 1st is dropped just as
silently if it is a `LIKE`, an `IS NULL`, an `OR`, a non-int4 comparison, or a
`Const op Var`. `HAVING` is never examined either.

`R1-7` — `xppa_build_summaries` builds each page's min/max over the **aggregated**
column (`state->agg_attno`, `:393`) and `xppa_page_can_skip` tests it against
**every** predicate with no attno check (`:476-491`). The comment at `:479-481`
states the intended invariant; the build function does not implement it. Whole
pages are skipped before the visibility check and before the qual. For
`PA_COUNT` it is accidentally safe (`agg_attno = 0` leaves min/max inverted); for
`PA_SUM` the columns genuinely differ, and the measured result is `0` instead of
21 929 000.

---

## SAFE_ERROR (13)

Exhaustion raises before any wrong result is emitted. Each was verified to be a
**pre-insert** guard covering every probe loop in its file, with occupancy
capped below the table size so the probe loop always terminates on a free slot —
the fall-off-the-end path is unreachable rather than absent.

| site | file | guard |
|---|---|---|
| `dim_build` | `xpb_batch_hashjoin.c` | `:156` at 3/4 |
| `dim2_build` | `xpb_batch_hashjoin.c` | `:508` at 3/4 |
| join group hash ×3 | `xpb_batch_hashjoin.c` | `:410, :758, :1010` |
| aggregate group hash ×3 | `xpb_batch_groupagg.c` | `:123, :241, :353` |
| `pdim_build` / `adim_build` | `xpb_batch_partition.c` | `:240, :268` |
| partition group hash | `xpb_batch_partition.c` | `:420` |
| `zlfs_group_sum` group hash | `xpb_zlfs.c` | `:773` |
| `tp_dim` | `xpb_typed_pipeline.c` | `:204` |
| `tp_grp` | `xpb_typed_pipeline.c` | `:591` |
| `TP_MAX_KEYS` / `TP_MAX_SUMS` | `xpb_typed_pipeline.c` | `:415-423`, before the copy |
| `APPEND_MAX_CHILDREN` | `xpb_batch_partition.c` | `:123` |
| `xpb_groupagg2.c` global hash | `xpb_groupagg2.c` | `:1383`, post-scan at 0.95 |
| `v2_grp` / `v2_dim1` / `v2_dim2` allocation ceilings | `xpb_v2_report.c` | `MaxAllocSize`, int overflow |

The `xpb_groupagg2.c` global hash is the one that is post-scan rather than
pre-insert, and it is sound only by a specific argument: `ngroups` counts
occupied slots, nothing is ever removed, so a row can be lost only once every
slot is occupied, which reports load 1.0 and raises. Every path that produces
that table was checked to reach the guard — the label sits inside the `else` of
`if (is_sorted)`, `is_sorted` is only ever assigned `false` and never back, and
all four `goto`s land after the label. No path bypasses it.

## SAFE_GROW (5)

`v2_grp`, `v2_dim1`, `v2_dim2` in `xpb_v2_report.c` (grow at load 0.5, double,
rehash under the new mask — reviewed and accepted in the previous milestone);
the elastic batch in `xpb_groupagg2.c` and `ScalarBatch` (both fixed-size but
lossless: they drain into the aggregator and reset); `s_groups` (repalloc
doubling).

## UNREACHABLE — with the enforcing code named (3)

Assigned only where the enforcement is a nameable line, never because a dataset
happens not to reach it.

| site | enforcing code |
|---|---|
| SIMD `vals[256]` in `xpb_groupagg2.c:1160` | `:1153` `if (false && xpb_groupagg2_simd_enabled …)` — the literal `false` |
| `xpb_src_zlfs.c` `col_map[8]`, no bound in the file | all seven call sites pass a compile-time-bounded value; the largest is bounded by `TP_MAX_KEYS`+`TP_MAX_SUMS` at `xpb_typed_pipeline.c:415/420` |
| `xpb_typed_pipeline.c:451` bound checked *after* the writes | `:415` and `:420` cap `ncols` at 4+4 = `XPCB_MAX_COLS`, so `:451` never fires late |

The last two are safe by arithmetic done elsewhere, not by anything the
declaring code states. Raising `TP_MAX_SUMS` to 5 turns the third into an
overrun with no diagnostic.

## AMBIGUOUS (4)

Source inspection cannot settle these.

- **`slots[XPB_PAGE_TUPLES_MAX]`**, `xpb_groupagg2.c:1121`. The collection loop
  stops at 256 and the rest of the page is never examined. At `BLCKSZ` 8192 the
  narrowest admissible tuple gives 226 max, so it cannot fill; at 16 KB it is
  454 and it can. There is no `StaticAssert`, no `#if BLCKSZ`, no runtime check
  anywhere in the extension. The invariant that exists covers tuple width, not
  block size.
- **pgcolumnar validity slice requires `capacity % 8 == 0`**,
  `xpb_src_pgcolumnar.c:438-439`. An `Assert` is the only guard; in a non-assert
  build an odd capacity shifts the validity window by up to 7 bits. Every
  consumer today passes 1024 or `XPCB_BATCH_CAP`.
- **`palloc(nblocks * 200)` with no bounds check on the fill**,
  `xpb_projection.c:90, 123-127`. 200 is not a bound on heap tuples per page
  (`MaxHeapTuplesPerPage` is 291). Observable is memory corruption, not a drop.
- **sub-partitioned children**, `xpb_batch_partition.c:136-188`.
  `find_inheritance_children` returns direct children only, and a partitioned
  child is handed to `xpb_heap_source_create` with no relkind check. Whether that
  errors or silently scans zero rows cannot be settled from this repository.

---

## Found during the audit, not capacity, not yet reproduced

Recorded so they are not lost. Each is a candidate for its own reproducer.

- **Duplicate dimension keys.** All four dimension builds insert at the first
  free slot with no duplicate check, so N dimension rows sharing a key emit the
  fact row once with an arbitrary payload, where SQL emits it N times. Neither
  `dim_period` nor `dim_account` has a UNIQUE constraint anywhere in the repo,
  and heap order decides which payload wins, so the answer is not stable across
  a VACUUM. (`xpb_v2_report.c`'s equivalent is deliberate, documented and tested
  — this is about the other four.)
- **`validity` is ignored by four operator files.** `xpb_batch_hashjoin.c`,
  `xpb_batch_groupagg.c`, `xpb_batch_partition.c` and `xpb_zlfs.c` never call
  `xpcb_isnull()`. The heap source deliberately leaves a NULL row's data slot
  untouched (`xpb_src_heap.c:249-253`), so a NULL in a read column puts
  uninitialised memory into a group key or a SUM. The `slot`/`copy` control paths
  in `xpb_batch_groupagg.c:186-190` refuse a nullable zone; the measured path
  beside them does not.
- **`zlfs_group_sum` reads zone columns as `int32 *` with no type check**
  (`xpb_zlfs.c:760-762`), while `zlfs_build_zone` accepts int8 columns.
- **The partition ZLFS arm ignores the period predicate** and accepts any valid
  zone for the partition whatever its bounds (`xpb_batch_partition.c:174-187`),
  so the same query returns different answers depending on whether
  `zlfs_build_zone` happened to have been called.
- **`totals[i] == 0` means both "no group" and "sums to zero"**,
  `xpb_columnar_pipeline.c:177-178`; the sibling experiment emits every period.
- **Unaligned `fixed_offset`.** `xpb_qual.c:213-215` accumulates `att->attlen`
  without `att_align_nominal`, though its own comment says alignment. For
  `(int4, int8)` a predicate on the int8 reads bytes 4..11. Compare
  `xpb_src_heap.c:1008`, which aligns.
- **The heap fixed path has no tuple-length bound** (`xpb_src_heap.c:716-729`).
  `ALTER TABLE ADD COLUMN NOT NULL DEFAULT` does not rewrite; the layout check
  passes but pre-ALTER tuples are short, and the read lands past the tuple. The
  deform path goes through `heap_deform_tuple` (missing-value semantics) and the
  projected path refuses explicitly at `:480-485`; the fixed path does neither.
- **`XPB_PAGE_TUPLES_MAX` is defined twice** as two independent literal 256s
  (`xpb_groupagg2.c:200`, `xpb_simd.h:122`), agreeing by coincidence of editing.
  Same for `XPB_BM_WORDS`.
- **Scratch buffers sized from `XPCB_BATCH_CAP` while sources bound themselves by
  `batch->capacity`** — they agree only because every consumer assigns the same
  constant.

## Performance observations

None taken. Measurement comes after correctness closure, and no fix has been
applied yet, so there is nothing to compare. When it happens, its only purpose is
to check for a catastrophic regression from removing the ceilings — not to
optimise rehash.

---

## What phase 5 must decide, per site

The policy preference is growth where the structure is naturally dynamic state,
and an explicit ERROR where growth would change the scope of the milestone.
Ignoring, dropping, returning not-found and partial results are all ruled out.

- **R1-1, R1-2, R1-3** are per-query aggregation state in experiment functions —
  natural growth candidates, same 0.5/double policy already proven in
  `xpb_v2_report.c`. But they are timed paths in benchmarks 02 and 05-A, so
  growth changes a measured cost and needs its own paired control, whereas a
  refusal does not change the timing of any shape that completes today.
- **R1-4** `local_ht` exists to be L1-resident, so growing it defeats its
  purpose. Spilling the overflow straight into the global hash is the candidate.
- **R1-5** is not a capacity fix at all: give the lookups a `found` out-param,
  as the hashjoin siblings already have.
- **R1-6, R1-7, R1-8** are not capacity fixes either. The correct policy is to
  decline the path whenever `has_residual` is set, or to apply the residual —
  and for R1-7, to build the summary over the predicate's column or not to skip.

Do not add spill-to-disk anywhere in this task; that is a separate architectural
layer.
