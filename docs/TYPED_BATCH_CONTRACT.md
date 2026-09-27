# Typed batch contract

What the compact pipeline passes between a source and an operator, and what
each side is allowed to assume. Supersedes the int32-only contract.

The scope is deliberately small: the types and semantics the register
workload needs. This is not generic PostgreSQL type support and is not meant
to become it.

## Why it changed

The old contract was:

```c
int32  *int32_cols[XPCB_MAX_COLS];
bool    owns_data;                  /* one flag for the whole batch */
```

Type and NULLability were not representable, so every operator inferred them,
and the inference was the bug:

* the heap source computed byte offsets from `attlen` and applied them to
  every tuple. On a table with a varlena or a nullable attribute before a
  column it read, it returned values from the wrong bytes — with the right
  row count, the right group count and an internally consistent
  `net = debit - credit`. Fixed in `326b116`; measured there.
* the pgcolumnar source could only raise *"the compact batch has no null
  representation"* and stop.
* nothing could carry an `int8` without truncating it.

## Column representation

```c
typedef struct XpBatchColumn
{
    XpbColType  type;
    void       *data;        /* dense array, xpcb_type_width(type) each */
    uint8      *validity;    /* NULL = all valid; else bit set = valid   */
    bool        owns_data;
    bool        owns_validity;
} XpBatchColumn;
```

A batch is `nrows`, `ncols`, and up to `XPCB_MAX_COLS` of these. Row count is
common to the batch; each column carries its own type, validity and
ownership.

**Operators never infer physical layout from `attlen`.** The source states
the type; the accessor checks it.

## Type set

| `XpbColType` | array element | notes |
|---|---|---|
| `XPB_COL_INT4` | `int32` | |
| `XPB_COL_INT8` | `int64` | never truncated to int32 |
| `XPB_COL_NUMERIC` | `Datum` → `Numeric` | pointer-valued |
| `XPB_COL_VARLENA` | `Datum` → detoasted varlena | readable and skippable; no operator computes on it |

Anything else is a clean error naming the column and its type OID. The heap
source maps `int4`, `int8`, `numeric`, and `text`/`varchar`/`bpchar`/`bytea`.

## Dispatch discipline

`xpcb_i32()`, `xpcb_i64()`, `xpcb_numeric()`, `xpcb_varlena()` validate the
column's type on **every** call. That is unconditional rather than under
`Assert`, because a type mismatch reinterprets raw memory and must not vanish
in a non-assert build.

They are therefore meant to be **hoisted out of row loops** — dispatch once
per column per batch, then run an untyped tight loop:

```c
int32 *acct = xpcb_i32(batch, 2);        /* hoisted */
for (int r = 0; r < batch->nrows; r++)   /* no dispatch in here */
    ... acct[r] ...
```

Every migrated loop hoists. There is no per-value fmgr dispatch anywhere in
the pipeline.

## Validity

```
validity == NULL   every row in the column is valid
validity != NULL   bit r set = row r valid, clear = row r NULL
```

A `NOT NULL` column pays nothing: no bitmap is allocated. A source that meets
its first NULL allocates the bitmap then, and rows already written are
retroactively marked valid.

Bit-set-means-valid matches pgcolumnar's own present bitmap, so that source
can hand its bitmap over without inverting it.

**NULL is never a sentinel.** There is no reserved `int32`, no magic
negative, nothing an operator could collide with real data. A NULL row's data
slot is left as it stands and must not be read.

Tested: all valid, some NULL, all NULL, NULL in the first row, NULL in the
last row, NULL across a batch boundary.

## Ownership and lifetime

Per column, not per batch, because one batch legitimately mixes both.

| | meaning |
|---|---|
| `owns_data == true` | allocated by the consumer; `xpcb_release_owned()` frees it |
| `owns_data == false` | **borrowed** from the source; valid only until that source's next `next_batch()`, `rescan()` or `end()` |

No operator may retain a borrowed pointer past that window. For the
pointer-valued types, borrowed means the pointed-to bytes are the source's
too; an owned column owns both the `Datum` array and everything it points at.

Per source:

| source | data | validity | notes |
|---|---|---|---|
| heap, fixed path | owned by consumer | none | all int4, all NOT NULL by the guard |
| heap, deform path | borrowed | borrowed | both live in the source's per-batch context, reset at the top of each `next_batch()`; numeric and varlena payloads live there too, so nothing is freed per value |
| ZLFS | borrowed | borrowed, or owned when realigned | the zone outlives the scan, so the data pointers are valid longer than the contract promises — consumers must not rely on that. A batch starting at a row that is not a multiple of 8 needs its validity bits restated into a small owned buffer |
| pgcolumnar | borrowed | none yet | either pgcolumnar's decoded stream (zero copy) or this source's per-group buffer; both live until the next group loads |

`xpcb_release_owned()` frees what a batch owns and leaves borrowed columns
alone. The old batch-level flag could not express the mixed case and got it
wrong in two places: `batch_join_probe()` set `owns_data = true` for a batch
whose column 0 was the join state's own `year_buf`, and the join1 cleanup
loop freed that column.

## Join NULL semantics

**NULL equals nothing, including another NULL.**

* A NULL fact key matches no dimension row.
* A dimension row with a NULL key is never loaded into the hash table, so it
  cannot be hit even by accident.
* A NULL dimension *payload* is kept — that is an ordinary value that flows
  on into the group key.
* Under an inner join, an unmatched row is dropped.

## GROUP BY NULL semantics

**NULLs are all the same group.** Grouping equality is *not distinct from*,
not `=`.

These are different comparisons and they are written separately —
`tp_keys_same_group()` versus `tp_dim_lookup()` — and named for what they do.
Reusing one for the other is the obvious bug in this area. NULL hashes to a
fixed constant rather than to a value from the data domain, so NULLs land
together without pretending to be a particular integer.

## Aggregation

SUM skips NULL inputs. **A group whose inputs were all NULL sums to NULL, not
to zero.** Each accumulator carries a "saw a value" flag; nothing is seeded
with a neutral element that could be mistaken for a real zero.

| aggregate | accumulator | emitted as |
|---|---|---|
| `sum(int4)` | `int64` | bigint |
| `sum(int8)` | `INT128` (`common/int128.h`) | numeric, as PostgreSQL does |
| `sum(numeric)` | PostgreSQL `Numeric` via `numeric_add` | numeric |

`INT128` is the same accumulator PostgreSQL's own `sum(int8)` uses. It is
rendered as a decimal string, which is numeric's text form for an integer. A
build without a native int128 refuses by name rather than running untested
fallback code.

## Numeric representation: the two candidates

Required before implementing. Both were considered; **A is implemented.**

### A — PostgreSQL numeric machinery (implemented)

Accumulate with `numeric_add` in a memory context of its own, freeing the
previous accumulator at each step.

* Correct PostgreSQL semantics by construction, including scale propagation.
* Arbitrary scale; no dependence on typmod.
* No hidden assumptions to document or guard.
* Cost: numeric machinery and an allocation in the inner loop. This is the
  slow, obviously-right one.

### B — constrained scaled integer (not implemented)

`numeric(p,s)` → `int128` scaled by `10^s`.

* Compact, fast, vector-friendly.
* Only valid when the typmod is present **and checked**: an unconstrained
  `numeric` column has a per-value scale, so there is no single `s`.
* Needs overflow detection at `p` near the int128 limit, and an explicit
  rounding rule.
* Is not general `numeric`, and must never be presented as if it were.

For this milestone correctness outranks speed, so B stays unimplemented. If
it is added later it belongs behind a checked typmod, as a *specialisation*
that falls back to A — not as a replacement for it.

## Fast path versus generic path

The heap source has two paths and **the caller names the one it wants**.
There is no fallback inside the scan loop.

| | `XPB_HEAP_FIXED` | `XPB_HEAP_DEFORM` |
|---|---|---|
| access | precomputed byte offsets | `heap_deform_tuple` |
| requires | every requested column `int4`; every attribute up to the highest one read fixed-width and `NOT NULL` | nothing beyond the type set |
| types | int4 | int4, int8, numeric, varlena |
| NULLs | none | yes |
| locking | share lock held on the page while copying; safe only because it reads fixed-width byval data | goes through the table AM, so tuples are deformed with the buffer pinned but unlocked — required, because detoasting under a buffer lock is not allowed |
| used by | benchmarks 02 and 04 | everything else, including the ZLFS builder |

Asking for `XPB_HEAP_FIXED` on a layout it cannot address is an **error, not a
silent downgrade**. `xpb_heap_layout_supports_fixed()` is the single source of
truth for both that guard and a caller asking which path a layout admits, so
the two cannot drift apart.

Benchmarks 02 and 04 still take the fixed path and their checksums are
unchanged: `09e54f9a` and `6854be25`, 200 groups each.

## ZLFS format versioning

The milestone's "ZLFS v1" and "ZLFS v2" are not the on-disk version numbers.
Written out so the two are never conflated:

| milestone name | on-disk `version` | layout |
|---|---|---|
| ZLFS v1 | 2 | int32 columns, no types, no validity |
| ZLFS v2 | 3 | typed columns + validity |

A v3 file records, per column: type code, width, and a flag saying whether a
validity bitmap follows the data. Header then, for each column in order,
`nrows * width` bytes of data followed by `(nrows+7)/8` bytes of bitmap when
the flag is set.

**Version 2 files are not read.** A zone is a rebuildable cache — 78 ms for a
million rows — and a second reader for a format carrying no type or validity
information would have to infer both, which is the failure this milestone
exists to remove. An old file is **refused by name** with a WARNING and a
hint to rebuild; it is never reinterpreted, and never passed over as if it
were merely missing. A column whose recorded type and width disagree is
refused the same way.

The zone builder no longer has its own copy of the fixed-offset tuple
decoding. It drains an `XpBatchSource` on the deform path, so it inherits the
source's guards and its types rather than re-deriving them. Zone build on the
benchmark 04 dataset: 82 ms before, 78 ms median (77–88) after.

## Known limitations

* **No varlena operators.** A varlena column can be read, carried and
  skipped; nothing joins, groups or computes on one.
* **No numeric or varlena group keys.** Group keys are int4 or int8.
* **ZLFS carries int4 and int8 only** — no numeric or varlena in a zone.
* **The pgcolumnar source still rejects NULLs.** It reads validity bitmaps
  and errors on a NULL in a requested column rather than carrying it into the
  batch. The bitmap convention already matches, so this is wiring, not
  design.
* **Fixed-capacity hash tables, with three exceptions, all in one file.**
  Benchmark 05-E measured where the group boundary sat -- 12 288 groups, a
  `#define`, not a resource. Hash Aggregate Growth v1 then replaced the group
  hash and Dimension Hash Growth v1 the two dimension hashes, in
  `xpb_v2_report.c` and nowhere else:

  | table | `#define` | behaviour |
  |---|---|---|
  | `xpb_v2_report.c` group hash | `V2_GRP_CAP` 16384 | grows at load 0.5, doubling, no spill |
  | `xpb_v2_report.c` dim1 (company) | `V2_DIM1_CAP` 256 | grows at load 0.5, doubling, no spill |
  | `xpb_v2_report.c` dim2 (account) | `V2_DIM2_CAP` 1024 | grows at load 0.5, doubling, no spill |
  | `xpb_typed_pipeline.c` group | `TP_GRP_CAP` 16384 | fixed, errors at 3/4 |
  | `xpb_typed_pipeline.c` dimension | `TP_DIM_CAP` 4096 | fixed, errors at 3/4 |
  | `xpb_batch_groupagg.c` group | `GRP_CAP` 16384 | fixed, errors at 3/4 |
  | `xpb_batch_hashjoin.c` group | `GRP_CAP` 16384 | fixed, errors at 3/4 |
  | `xpb_batch_hashjoin.c` dim1 / dim2 | `DIM_CAP` 256 / `DIM2_CAP` 512 | fixed, errors at 3/4 |
  | `xpb_batch_partition.c` group | `GRP_CAP` 16384 | fixed, errors at 3/4 |
  | `xpb_batch_partition.c` dim1 / dim2 | `DIM_CAP` 256 / `DIM2_CAP` 512 | fixed, errors at 3/4 |
  | `xpb_projection.c` aggregate | `AGG_CAP` 16384 | fixed, **no guard -- drops rows when full** |
  | `xpb_columnar_pipeline.c` window | `WHASH_CAP` 131072 | fixed, **no guard -- drops rows when full** |
  | `xpb_zlfs.c` group | `GRP_CAP` 16384 | fixed, errors at 3/4 |
  | `xpb_groupagg2.c` group | `hash_cap_used`, planner-supplied, default 16384 | fixed, **no pre-insert guard**; post-scan `ERROR` above 0.95 load |

  The three growing tables are all reached through `xpb_v2_register_report`. So
  "aggregation scales to 147 456 groups", and the dimension cardinalities
  measured in Dimension Hash Growth v1, are statements about the benchmark
  report function, not about the pipeline. Every other table is unchanged and
  still carries its fixed ceiling. Nothing spills anywhere.

  The growing tables raise `ERROR` only at a real allocation boundary
  (`MaxAllocSize`, or a slot count that would overflow), not at a load factor.

  `xpb_groupagg2.c` is the one row whose capacity is not a `#define` at all:
  it arrives from `custom_private[6]`, so a planner underestimate is what fills
  the table. Its insert loops carry no load check; the protection is a single
  post-scan test, and it works only because a lost row cannot raise `ngroups`
  past `hash_cap` — a full table reports load 1.0 and errors. That is a coarser
  guarantee than every other row here, and it is the only one reachable through
  a planner hook (`set_rel_pathlist_hook` / `create_upper_paths_hook`, GUC
  `xpb_groupagg2_enabled`) rather than an explicit benchmark function.

  The two rows marked **no guard** are not merely capped: `agg_insert()` in
  `xpb_projection.c` and the window-hash insert in `xpb_columnar_pipeline.c`
  probe every slot and then fall off the end of the loop, discarding the row
  without an error -- a wrong answer rather than a failure, the same defect
  class that was fixed in `xpb_v2_report.c`'s dimension hashes. Both are
  pre-existing and on paths outside the measured pipeline; reachability is
  unproven and is recorded as its own task in
  `docs/roadmap/fixed-hash-silent-drop.md`. It is not folded into a benchmark
  milestone, for the same reason the `xpb_groupagg2.c` overflow is not.

* **`sum(int8)` has three different behaviours, on purpose so far but not by
  design.** `xpb_typed_pipeline.c` accumulates in INT128 and returns numeric,
  which is what this document describes. `xpb_v2_report.c` accumulates in a
  checked int64 and raises `bigint out of range` where PostgreSQL's
  `sum(bigint)` would have answered in numeric. `xpb_groupagg2.c` accumulates
  unchecked and can convert a wrapped int64 with `int8_numeric()` -- an open
  question recorded in `docs/roadmap/groupagg2-int64-overflow.md`, reachability
  unproven. A single contract needs one answer here.
* **No planner integration**, no automatic pipeline selection, no expression
  evaluation, no MVCC under concurrent writers.
* **`make installcheck` is a no-op** — there is no `REGRESS` target. The real
  suites are `test/contract_tests.sh`, `test/heap_layout_guard.sh` and
  `test/regression_guards.sh`.
