# Silent-Drop Reachability — the map

Status: **reopened 2026-10-01 for R1-11, then closed again. SILENT_DROP = 0,
AMBIGUOUS = 0.**

Phase 4 mapped the space and phase 5 closed it at 11 defects. The map was then
reopened: auditing the `XpGroupAgg2` correlation cost gate found a twelfth
silent drop that phase 4 never examined, because the inventory was a survey of
**bounded state** — capacity limits — and R1-11 is not a capacity limit. It is an
ordering assumption. The map's original question did not reach it.

`test/reproducers/silent_drop_map.sh` passes 10/10 and
`test/reproducers/groupagg2_desc_stream_exit.sh` passes 14/14; the wrong result
each case originally produced is recorded beside it so each test visibly closes
an observed defect.

The question the map asked of every site, and the one it did not:

> asked:     can a local **capacity** limit silently change the answer?
> not asked: can an unproven assumption about **physical row order** silently
>            change the answer?

R1-11 was reachable through the second question for as long as the map claimed
SILENT_DROP = 0 under the first. Any future survey here should ask both.

Starting point: checkpoint `cf0c891`. Benchmark 05 is a reviewed boundary and is
not revisited here.

The question asked of every site:

> Can any input valid under SQL and the Typed Batch Contract produce, instead of
> a result or an explicit ERROR, a **silent** change to the answer because a
> local capacity limit was reached?

## Result

```
                          phase 4      phase 5
bounded sites examined        31           31
  SAFE_GROW                    5            6   (local_ht now drains and reuses)
  SAFE_ERROR                  13           20   (AGG_CAP, WHASH_CAP, S2CAP,
                                                 and the four former AMBIGUOUS)
  UNREACHABLE                  3            3
  SILENT_DROP                  8            0
  AMBIGUOUS                    4            0

defects found while fixing:  +2  -> both closed (R1-9, R1-10)
initial SILENT_DROP:          8
new defects found:            2   (R1-9, R1-10)
                             +1   (A4, found by analysing an AMBIGUOUS site)
                             +1   (R1-11, found after the map was closed)
fixed:                       12
remaining:                    0
```

The count went from 8 to 12 rather than staying at a tidy 8: fixing the
page-skip exposed two more, resolving the AMBIGUOUS four turned one of them
into a real silent drop, and reopening the map for the cost-gate audit added a
twelfth. All four are recorded at full weight.

R1-11 is not in the bounded-state table above because it is not bounded state.
It sits in its own section at the end.

Neither column sums to 31. The rows are classification entries, not distinct
sites — R1-3 is two instances in one row — and R1-5…R1-10 are semantic defects
that leave the bounded-state taxonomy entirely once fixed. The site inventory
is the sections below, not this summary.

## The rule this phase established

> An Xp execution path either implements the query's semantics completely, or
> refuses the query before executing it. Partially understood SQL is not an
> acceptable fast path.

Six of the ten defects were that rule being broken, not a table being too small.

## Per site

| # | site | before | root cause | policy | after |
|---|---|---|---|---|---|
| R1-1 | `AGG_CAP`, `xpb_projection.c` | 16384 of 16900 | probe loop falls off the end | refuse (timed path; growth would change the measured cost) | SAFE_ERROR |
| R1-2 | `WHASH_CAP`, `xpb_columnar_pipeline.c` | 131072 of 140000 | same | refuse | SAFE_ERROR |
| R1-3 | `S2CAP`, `xpb_columnar_pipeline.c` ×2 | 16384 of 16900 | same | refuse | SAFE_ERROR |
| R1-4 | `local_ht`, `xpb_groupagg2.c` | 256 of 301 groups | same | **drain into the global hash and reuse** — growth would defeat its L1 purpose, and the same file already drains the elastic buffer and ScalarBatch this way | SAFE_GROW |
| R1-5 | `pdim_year`/`adim_group` | 200 of 400 | `-1` meant miss, full, and a legal payload | explicit `bool` + out-param, as the hashjoin siblings already had | fixed |
| R1-6 | residual quals, `xpb_pageagg.c` | 20000 of 2000 | path taken with clauses it cannot represent, never re-applied | refuse when any clause is unrepresented | fixed |
| R1-7 | page skip, `xpb_pageagg.c` | 0 of 21929000 | min/max built over the **aggregated** column, tested against the **qual's** predicates | skip only on a predicate for the summarised column | fixed |
| R1-8 | residual quals, `xpb_groupagg2.c` | 5010 of 501 | refused only when *nothing* was pushable | refuse on any residual | fixed |
| R1-9 | target list, `xpb_pageagg.c` | `'x' \|\| sum(a)` gave `2000`, SQL gives `x21929000` | node emits one value; gate took the first Aggref and ignored the rest of the list | accept only a target list that is exactly one bare Aggref | fixed |
| R1-10 | target list, `xpb_pageagg.c` | `count(*), sum(a)` → protocol error and a backend crash | one-attribute slot serving a two-column plan | same gate | fixed |

R1-9 and R1-10 were found by running the R1-7 fix: the query shape used to
check it was itself mis-answered.

The four sites source inspection could not classify were resolved the same way:
each became a provable refusal instead of a "cannot prove".

| # | site | was | resolution | after |
|---|---|---|---|---|
| A1 | `slots[XPB_PAGE_TUPLES_MAX]`, `xpb_groupagg2.c` | cannot fill at `BLCKSZ` 8192, can at 16 KB; nothing enforces `BLCKSZ` | `poff` hoisted out of the collection loop; a page left unfinished now raises `ERRCODE_PROGRAM_LIMIT_EXCEEDED` instead of aggregating a truncated page | SAFE_ERROR |
| A2 | validity slice needs `capacity % 8 == 0`, `xpb_src_pgcolumnar.c` | `Assert` only — a non-assert build shifts the window by up to 7 bits | the `Assert` replaced by a real `ereport(ERROR)` stating the byte-alignment invariant | SAFE_ERROR |
| A3 | `palloc(nblocks * 200)`, `xpb_projection.c` | 200 is not a bound on tuples per page (`MaxHeapTuplesPerPage` is 291); observable is corruption, not a drop | fill loop bounded; `nrows >= capacity` raises before writing `col_pk[nrows]` | SAFE_ERROR |
| A4 | sub-partitioned children, `xpb_batch_partition.c` | could not be settled from source | **it was a silent drop.** A partitioned child owns no storage, so its whole subtree contributed zero rows with no error. `get_partitions_in_range` now refuses any child whose `relkind != RELKIND_RELATION` | SAFE_ERROR |

A4 is counted as a new defect, not absorbed into the original eight. Its
reproducer is the last case in `silent_drop_map.sh`: a two-level partitioned
table whose 400 was returned as 0.

## HAVING

Proven by refusal, which was the cheaper of the two branches the brief allowed.
`root->parse->havingQual != NULL` now declines the path in both
`xpb_pageagg.c` and `xpb_groupagg2.c`. Verified by EXPLAIN: a `HAVING` query
selects neither node. Nothing re-applies `HAVING` above either of them — both
replace the whole grouping rel — so refusing is the only correct option short of
implementing it.

## The A/B equivalence rule

`AGG_CAP` and `S2CAP` both dropped the **same** rows in **both** arms of the
experiment that hosts them, so each experiment's own equivalence check stayed
green while both halves were wrong. Agreement between two arms of the same
experiment is therefore not a correctness oracle, and never was.

**Rule for the benchmark harness from here: every A/B correctness claim needs a
third party — plain PostgreSQL, or an exactly generated expectation.** The
reproducer suite is written this way; `run-dim-growth.sh` and the 05-x gates
already were.

## EXPLAIN gate for the refusals

A refusal that accidentally disabled the path would be "correct" and useless, so
acceptance was checked in both directions:

```
supported predicate only          -> Xp PRESENT
two supported predicates          -> Xp PRESENT
text equality / LIKE / IS NULL    -> Xp ABSENT
OR / int8 column / mixed          -> Xp ABSENT
HAVING                            -> Xp ABSENT
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
`raw/2026-09-30-silent-drop-map.txt` (2 of 9 agreeing — the defects as found).
Two control cases in the same script stay green, so the harness is not simply
always-red. The same script after phase 5, 10 of 10:
`raw/2026-09-30-silent-drop-map-phase5-green.txt`.

The script also gained a preflight and a numeric check on both sides of every
comparison. Without them a dead server made six of the cases pass vacuously —
`psql` wrote the same connection error to both sides and they compared equal.
That was observed, not hypothesised; a wrong port now exits 2.

---

## SILENT_DROP — as phase 4 reproduced them (8)

All eight are closed; the table is the record of what was observed, kept so the
regression suite is visibly tied to measured defects rather than to a theory.

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

## AMBIGUOUS (4) — all resolved, now 0

Source inspection could not settle these in phase 4. Phase 5 did not try harder
to prove them safe; it made each one refuse, so the proof is no longer needed.
Three became SAFE_ERROR guards over invariants that were previously implicit;
the fourth (sub-partitioned children) turned out to be a genuine silent drop and
is counted as one. Resolutions are in the A-table above. The phase-4 reasoning
is kept below unchanged.

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
  *(Settled by running it: it scanned zero rows and returned no error. See A4.)*

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

## Performance

Measured after correctness, as a paired before/after: the phase-5 source diff is
reverted, rebuilt, installed, the server restarted, measured; then re-applied,
rebuilt, restarted, measured again — back to back in one session, on one host.
Cross-session timings on this host drift by up to 1.6× and are not used.

**Benchmarks 02 and 05-A — no detectable change.** Median of 5 warm runs,
after/before:

```
02  xpb_heap 1.02   xpb_zlfs 0.97   xpb_pgcolumnar 0.95   plain SQL 0.94
05-A (by range 1..1 / 1..3 / 1..6 / 1..12)
    xpb_heap        1.15  1.25  1.07  0.99
    xpb_zlfs        0.89  1.02  0.95  1.00
    xpb_pgcolumnar  1.40  0.93  0.73  0.86
```

This is not a claim that nothing changed. Only the `xpb_pgcolumnar` arm runs
through a file the diff touches (`xpb_src_pgcolumnar.c`, which now restores the
validity bit on a predicate-rejected row). The `xpb_heap` and `xpb_zlfs` arms
are untouched by the diff and are therefore the noise floor — and they scatter
0.89–1.25, as wide as the touched arm's 0.73–1.40, which includes ratios below
1.0 that added work cannot produce. **This pairing cannot resolve anything
smaller than roughly ±25%.** No regression is shown; none is excluded either.

**The refusals do cost, and the cost is the point.** Where a wrong-but-fast Xp
path is now declined, the query falls back to the PostgreSQL executor. On 2M
rows, median of 5:

```
                                     before            after          ratio
pageagg, residual qual               18.0 ms           72.8 ms        4.05x
  (WHERE name='x')                   2 000 000 rows    200 000 rows
                                     WRONG             correct
pageagg, page skip, supported-only   20.1 ms           56.8 ms        2.83x
  (WHERE period_key < 10)            0                 200192900000
                                     WRONG             correct
```

The "before" numbers are not a baseline to defend: both produced the wrong
answer, and the page-skip one was fast precisely because it skipped every page.
A 4× slowdown that replaces a wrong answer with a right one is the intended
outcome of this phase, recorded rather than compensated for.

**groupagg2's refusal cost is unmeasured, not zero.** No shape was found in
which `XpGroupAgg2` both wins on cost and carries a residual qual: at 10 000
rows it is selected and the runtime is below timing resolution; at 2M rows the
planner picks a parallel `GroupAggregate` in both arms, so the refusal changes
nothing that was going to be chosen. Stated as a gap, not as a result.

Raw runs: `/tmp` scratch only — these are diagnostic pairings, not published
benchmark artifacts, and nothing under `benchmarks/*/raw/` or `plans/` was
rewritten.

---

## What phase 5 decided, per site (the phase-4 plan, kept for comparison)

Every line below was followed as written; the per-site table at the top is what
was actually built. The one place the plan was silent is A4, which the plan
listed as unknowable and which turned out to be a defect.

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

---

## R1-11 — XpGroupAgg2 assumed a physical row order it never established

Found 2026-10-01, after this map had been closed, while auditing the
`XpGroupAgg2` correlation cost gate. Not a capacity limit, which is why the
phase-4 survey did not reach it. Full analysis in
`docs/GROUPAGG2_COST_GATE_AUDIT.md`; reproducer
`test/reproducers/groupagg2_desc_stream_exit.sh`.

### Before

| data | predicate | PostgreSQL | XpGroupAgg2 |
|---|---|---|---|
| descending, correlation −1.0 | `k1 <= 500` | 501 groups, sum 100200 | **0 groups** |
| nearly sorted, correlation 0.9998 | `k1 <= 500` | 501 groups, sum 100200 | 501 groups, **sum 99728** |
| nearly sorted, correlation 0.9953 | `k1 <= 500` | 501 groups, sum 100200 | 501 groups, **sum 98697** |
| nearly sorted, correlation 0.9329 | `k1 <= 500` | 501 groups, sum 100200 | 501 groups, **sum 93430** |

No error in any case. The near-sorted rows are the dangerous ones: the group
count is right and only the aggregates are short, by 0.1% to 6.8% as physical
order degrades — all at correlations the applicability gate admits, with nothing
forced.

### Root cause

Three page-skip mechanisms, all resting on an ordering nothing established:

1. **Class 1 tail exit** — terminated the whole scan when a page's *first* tuple
   key exceeded the predicate's upper bound.
2. **Class 1 prefix skip** — skipped a page whose first and last tuple keys were
   both below the lower bound.
3. **Page-level quick reject** — took the first and last tuple on the page,
   called them `pmin`/`pmax`, and rejected the page as "definitely outside
   range". Its own comment called it a heuristic for locally clustered data.

None used a true per-page min/max, so all three were unsound even per page on a
page that is not internally ordered. (1) and (2) were additionally licensed by
`is_sorted`, which at that point had been inferred from **page 0 alone** — and a
page of equal keys passes that probe, which is how descending data was reported
ordered and then truncated on its first page.

The applicability gate accepted the path on `fabs(correlation) >= 0.8`. That is
not the same condition as monotonic, and `fabs` additionally admitted
correlation −1.0, where the node's own STREAM definition cannot hold.

Runtime STREAM detection could not catch any of it: it reports
`order_scope: STREAM`, "monotonic order detected at runtime", while rows are
dropped, because it inspects only the tuples that survived the skip. It is
vacuous for exactly the rows that were lost.

### Policy chosen

**Scan rather than early-exit.** All three mechanisms removed. The correlation
gate is left exactly as it was — not re-tuned, not replaced by another
statistical threshold — and is now what its name claims: a performance
heuristic choosing between STREAM and hash, with no correctness load.

Deliberately not done: inferring monotonicity from the prefix already scanned,
and substituting a different statistic. A sound page skip needs real per-page
bounds, which means visiting every tuple on the page; that is a separate
decision and is recorded as such, not smuggled into a correctness repair.

### After

Every rung correct against PostgreSQL, and the node still runs — the repair is
not correctness-by-declining:

```
ga_asc   all four predicate shapes          ok
ga_desc  all four predicate shapes          ok   (was 0 of 501 on two of them)
jitter   0 / 20 / 100 / 400                 ok   (was short by up to 6.8%)
XpGroupAgg2 chosen, ascending               ok
XpGroupAgg2 chosen, descending              ok
```

Counters on the formerly failing case: `actual rows=501`, `Tuples Visited:
200000`, every page read. On sorted data `order_scope: STREAM` and
`StreamingAgg (STREAM, dual-key)` still activate, so the fast path survived; it
simply no longer skips pages.

Classification: **fixed.** The two cases are permanent regression tests.

### Known cosmetic inaccuracy, not fixed

`Pages Scanned` now reports 1083 for a 1082-page relation: page 0 is counted
once by the hash-preallocation probe and once by the main scan loop. An
off-by-one in an observability counter, pre-existing, left alone to keep this
repair to the correctness boundary.

