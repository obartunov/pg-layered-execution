# Benchmark 05-A — Selectivity Crossover

```bash
benchmarks/05-1c-like-v2/load.sh           <PGPORT> [PGHOST] [DBNAME]   # default: onec2
benchmarks/05-1c-like-v2/run-selectivity.sh <PGPORT> [PGHOST] [DBNAME]
```

Use a database of its own. Dataset `1c-like-v2` shares no table and no file
with `1c-like-v1`; benchmark 04's data, scripts and checksum are untouched.

## The question

> At what predicate selectivity does `pgColumnar + xp_batch` recover the cost
> of columnar decode through row-group pruning, relative to `heap + xp_batch`?

> **Partly superseded by 05-B, below.** This section's answer is true of the
> heap arm *as 05-A measured it*, which ran on the generic deform path. 05-B
> puts the same row shape on fixed offsets and the ordering reverses at full
> scan. Read the two together.

**Answer, on this workload: it never has to.** pgColumnar + xp_batch is faster
than heap + xp_batch at all four measured points, and its margin *narrows* as
the predicate widens — the opposite direction from a crossover. See
[Crossover analysis](#crossover-analysis) for why, and why this does not
contradict benchmark 04.

## Revisions and build identity

| | |
|---|---|
| repo base | `main` = `3eb21db`, plus the Typed Batch Contract v2 milestone |
| PostgreSQL | 20devel `5713b43` + `patches/required/0001-executor-batch-api.patch` |
| pgColumnar | `5b20ae8` (1.0-alpha5) + `patches/pgcolumnar-alpha5/0001-export-fold-reader-api.patch` |
| `xp_batch.so` | md5 `c82d4b7da6e8793316f30cb2bd5a6d21` |
| `pgcolumnar.so` | md5 `8ace119c0e5246b716eac5c8dc7dd1cd` |

> **That `xp_batch.so` no longer exists in this tree, and re-running
> `run-selectivity.sh` at HEAD will not reproduce the table below.** Five later
> milestones added instrumentation to the same measured path — deform and
> projected counters, group-hash counters, and two overflow-checked adds per
> aggregated row. The correctness gate still passes unchanged, and the
> *ordering* of the arms has held at every re-run, but the absolute
> milliseconds are those of the pinned build. Later sections state their own
> build conditions; none of them is comparable with this one.

The required executor batch API patch was previously checked against stock
PostgreSQL on the same base commit and showed no measurable effect on the
ordinary row-executor path (347.0 ms against 345.2 ms, ranges almost entirely
overlapping). This control is not repeated here.

## Dataset `1c-like-v2`

Deterministic, seed 20260924, 1 000 008 rows. Measured by `verify.sql`, not
asserted:

```
rows                              1 000 008      rows per period       83 334 / 83 334
distinct period                          12      accounts per company   200 / 200
distinct company_key                     50      companies per account   50 / 50
distinct account_key                    200      report groups              200
distinct (company, account)          10 000  = 50 x 200
```

The pair count is the check that matters — `1c-like-v1`'s first generator made
`company_key` and `account_key` determine each other, which would make the
second join trivial. Heap and pgColumnar hold byte-identical rows
(`11836397e8afc7ea1fd8a76587e13155` over all nine columns).

Row shape, the Typed Batch v2 shape:

| attnum | column | type | |
|---|---|---|---|
| 1 | `period` | `int4 NOT NULL` | range predicate, clustering key |
| 2 | `comment` | `text` | **varlena, ahead of everything the pipeline reads** — 25% NULL, 25% empty |
| 3 | `company_key` | `int4 NOT NULL` | join 1 |
| 4 | `account_key` | `int8 NOT NULL` | join 2, values past 2³¹ |
| 5 | `quantity` | `int8` | 11.1% NULL |
| 6 | `debit` | `numeric(18,2)` | 16.7% NULL |
| 7 | `credit` | `numeric(18,2) NOT NULL` | |
| 8 | `debit_cents` | `int8 NOT NULL` | **measured** |
| 9 | `credit_cents` | `int8 NOT NULL` | **measured** |

### Why the report aggregates int8 and not numeric

Every batch source can carry an `int8`. **None can carry a `numeric`**: a ZLFS
zone holds int4 and int8, and the pgColumnar source refuses a variable-width
stream by name. Aggregating the numeric columns would leave nothing to compare
across paths, which is the entire point of the exercise.

The numeric columns stay in the row — they are part of what puts the heap arm
on the deform path — and are measured separately below. Because the measured
money columns are `NOT NULL`, `sum(debit) - sum(credit) = sum(debit - credit)`
holds for every group, which the gate checks directly.

This is §17's "otherwise" route, taken explicitly: **05-A measures the
selectivity/source crossover, not NULL handling.** NULL handling is covered by
`contract_tests.sh`, including through the pgColumnar source.

## Physical clustering — the prerequisite

A selectivity benchmark is meaningless without pruning opportunity.
`1c-like-v1` assigned period round-robin, which is why benchmark 04 measured
`rowgroups=7 copied=0` with nothing skipped. `1c-like-v2` is generated and
loaded in period order, and `inspect-rowgroups.sql` **proves** the result
against pgColumnar's own zone maps rather than inferring it from insert order:

```
rowgroup 1  rows=150000  periods  1..2      rowgroup 5  rows=150000  periods  8..9
rowgroup 2  rows=150000  periods  2..4      rowgroup 6  rows=150000  periods  9..11
rowgroup 3  rows=150000  periods  4..6      rowgroup 7  rows=100008  periods 11..12
rowgroup 4  rows=150000  periods  6..8
```

| predicate | rowgroups read | skipped |
|---|---|---|
| `period = 1` | 1 of 7 | 6 |
| `period BETWEEN 1 AND 3` | 2 of 7 | 5 |
| `period BETWEEN 1 AND 6` | 4 of 7 | 3 |
| `period BETWEEN 1 AND 12` | 7 of 7 | 0 |

## Logical selectivity is not physical selectivity

This distinction is the reason the benchmark records both:

| predicate | logical rows | % of rows | rowgroups read | rows in those groups | % physically decoded |
|---|---|---|---|---|---|
| 1/12 | 83 334 | 8.3% | 1 of 7 | 150 000 | **15.0%** |
| 3/12 | 250 002 | 25.0% | 2 of 7 | 300 000 | **30.0%** |
| 6/12 | 500 004 | 50.0% | 4 of 7 | 600 000 | **60.0%** |
| 12/12 | 1 000 008 | 100% | 7 of 7 | 1 000 008 | 100% |

A 1/12 predicate is **not** an 8.3% physical scan. It decodes 15% of the rows,
because row-group boundaries do not coincide with period boundaries, and the
adapter then discards the surplus — 60 000 of the 150 000 rows in rowgroup 1
are dropped by vector-level skipping plus the per-row recheck.

## The heap arm runs on the generic deform path

Recorded on every run (`heap_path=deform` in the report line). The v2 row has
a varlena at attnum 2, ahead of every column the pipeline reads, so the
fixed-offset path cannot address it and says so rather than downgrading
silently.

**Its source timing is therefore not comparable with benchmark 04's.** Those
are different mechanisms on different rows: benchmark 04 measured
`XPB_HEAP_FIXED` over a 57 MB all-`int4 NOT NULL` table; this measures
`heap_deform_tuple` over a 95 MB table with a varlena, two numerics and three
nullable columns.

## Results

PG 20devel `5713b43` + required patch, pgcolumnar `5b20ae8` + export patch,
one container, warm cache, 1 warm-up + 5 measured runs, median (min–max).
Milliseconds from another machine are not comparable with these; the
checksums, the group counts and the shape of the breakdown are.

GUCs, set explicitly on every run rather than taken from the cluster:
`work_mem = 64MB`, `enable_hashjoin = on`, `jit = off`,
`max_parallel_workers_per_gather = 0` for `vanilla_single` and `= 2` for
`vanilla_parallel`. `jit` is off by boot value as well — this build has no
LLVM — so **JIT is not exercised at all here.**

### xp_batch paths

`total = build + open + source + operators`, with nothing unaccounted.
`operators = join1 + join2 + agg`.

| path | range | median | min | max | open | source | operators | rowgroups read | rows decoded |
|---|---|---|---|---|---|---|---|---|---|
| heap + xp_batch | 1/12 | **57.7** | 53.7 | 75.7 | 0.0 | 56.4 | 1.1 | — | 1 000 008 |
| pgColumnar + xp_batch | 1/12 | **14.4** | 10.3 | 17.6 | 0.3 | 12.3 | 1.3 | 1 of 7 | 150 000 |
| ZLFS + xp_batch | 1/12 | 20.1 | 19.4 | 23.1 | 18.8 | 0.0 | 1.1 | — | 83 334 |
| heap + xp_batch | 3/12 | **69.1** | 66.2 | 73.6 | 0.0 | 66.0 | 2.3 | — | 1 000 008 |
| pgColumnar + xp_batch | 3/12 | **20.0** | 17.9 | 33.8 | 0.2 | 17.0 | 2.3 | 2 of 7 | 300 000 |
| ZLFS + xp_batch | 3/12 | 24.2 | 24.0 | 27.6 | 21.6 | 0.0 | 2.2 | — | 250 002 |
| heap + xp_batch | 6/12 | **77.1** | 73.0 | 83.1 | 0.0 | 71.6 | 4.4 | — | 1 000 008 |
| pgColumnar + xp_batch | 6/12 | **27.5** | 26.3 | 30.0 | 0.3 | 22.8 | 4.0 | 4 of 7 | 600 000 |
| ZLFS + xp_batch | 6/12 | 35.7 | 34.4 | 38.6 | 30.8 | 0.0 | 4.2 | — | 500 004 |
| heap + xp_batch | 12/12 | **88.3** | 82.4 | 101.6 | 0.0 | 79.5 | 8.2 | — | 1 000 008 |
| pgColumnar + xp_batch | 12/12 | **46.9** | 41.2 | 70.4 | 0.3 | 38.4 | 7.8 | 7 of 7 | 1 000 008 |
| ZLFS + xp_batch | 12/12 | 44.9 | 39.5 | 48.5 | 34.6 | 0.0 | 9.0 | — | 1 000 008 |

### Stock PostgreSQL executor

Whole-query timings only. No phase breakdown is invented for these.

| path | 1/12 | 3/12 | 6/12 | 12/12 |
|---|---|---|---|---|
| heap, `vanilla_single` | 84.9 | 131.7 | 196.2 | 339.4 |
| heap, `vanilla_parallel` (2 workers) | 51.9 | 74.8 | 120.6 | 181.2 |
| pgColumnar, `vanilla_single` | 39.7 | 109.3 | 217.0 | 409.1 |
| pgColumnar, `vanilla_parallel` (2 workers) | 39.8 | 64.7 | 123.8 | 219.2 |

`EXPLAIN (ANALYZE, BUFFERS, SETTINGS)` for each is in `plans/`.

## Crossover analysis

**Observation 1 — the batch pipeline shows no crossover, because pgColumnar
already wins everywhere.**

| range | heap source | pgColumnar source | ratio |
|---|---|---|---|
| 1/12 | 56.4 | 12.3 | 4.6× |
| 3/12 | 66.0 | 17.0 | 3.9× |
| 6/12 | 71.6 | 22.8 | 3.1× |
| 12/12 | 79.5 | 38.4 | 2.1× |

The margin *shrinks* as the predicate widens. Extrapolating the trend, the two
would meet somewhere past a full scan — and there is nothing past a full scan.
Within this experiment there is no crossover point to report.

**Observation 2 — the heap source barely responds to selectivity at all.**
56.4 ms at 1/12 against 79.5 ms at 12/12: a 41% increase for twelve times the
rows. The heap source has no pruning; it reads every block and filters per
row, so its cost is dominated by scanning 95 MB whatever the predicate says.
An empty range (periods 90..91) still costs ~44 ms and returns nothing.

**Observation 3 — the pgColumnar source scales with rowgroups read**, not with
logical selectivity: 12.3, 17.0, 22.8, 38.4 ms for 1, 2, 4 and 7 rowgroups.
That is close to linear in groups decoded, which is what pruning is supposed
to deliver.

**Observation 4 — operator cost is independent of the source**, as the
diagnostic predicted. At every point the two paths agree within noise: 1.1 vs
1.3 at 1/12, 8.2 vs 7.8 at 12/12. It tracks rows emitted, not the storage
they came from. Benchmark 04's finding survives the change of row shape.

**Observation 5 — the *stock* executor does have a crossover, between 3/12 and
6/12.** Single-core: columnar wins at 1/12 (39.7 vs 84.9) and 3/12 (109.3 vs
131.7), loses at 6/12 (217.0 vs 196.2) and 12/12 (409.1 vs 339.4). This
experiment has four samples, so the only defensible statement is that it lies
somewhere between 3/12 and 6/12. No percentage is interpolated from it.

**Hypothesis, not measured here.** The reason the batch pipeline sees no
crossover where the stock executor does is most likely that the two arms are
not symmetric: the batch heap arm is on the deform path over a 95 MB row (100
bytes/row) while the columnar arm reads 15 MB (16 bytes/row) and decodes only
the five projected columns. Benchmark 04, where the heap arm used the
fixed-offset path over a narrow all-int4 row, measured the reverse ordering
(heap 25.1 ms against pgColumnar 35.6 ms). Separating "columnar helps" from
"the deform path is expensive" would need a heap arm on the fixed path over
this row shape, which the layout does not permit. That is a next experiment,
not a conclusion from this one.

### ZLFS is measured differently and should be read differently

`source = 0.0 ms` on every ZLFS row because the zone is already materialized.
But `open` is not free: the first call **in a backend** loads the zone file,
and these runs use a fresh backend each time.

Measured in one backend, same range 1..12:

```
first call    total 42.4 ms   open 33.0   source 0.0   operators 8.5
second call   total  8.1 ms   open  0.0   source 0.0   operators 7.9
third call    total  8.1 ms   open  0.0   source 0.0   operators 7.9
```

Benchmark 04's 7.0 ms measured the resident case, because its runner built the
zone in the same session. Both numbers are real; they answer different
questions. The steady-state figure here is `total - open`.

Zone build, measured separately and never part of query latency:

```
zone [1..1]    83 334 rows    62 ms    1 627 KB
zone [1..3]   250 002 rows    70 ms    4 882 KB
zone [1..6]   500 004 rows    95 ms    9 765 KB
zone [1..12] 1 000 008 rows  122 ms   19 531 KB
```

## Physical sizes

```
heap reg2           95 MB     100.0 bytes/logical row
pgColumnar reg2_col 15 MB      16.0 bytes/logical row
ZLFS zone [1..12]   19 531 KB   20.0 bytes/logical row  (5 columns only)
```

The ZLFS zone holds five columns; the other two hold all nine. That 6.3:1
heap-to-columnar ratio is most of why the columnar source reads less.

## numeric versus int8, on the same rows

Same grouping, same row count, heap deform, money columns differing only in
type (5 runs each, median):

```
int8     (debit_cents, credit_cents)    73.8 ms
numeric  (debit, credit)               207.3 ms      2.8x
```

Numeric aggregation through `numeric_add` dominates the pipeline when it is
used. This is a measurement, not a complaint: it is the cost of the
"correctness first" choice recorded in `docs/TYPED_BATCH_CONTRACT.md`, and it
is what a constrained scaled-integer representation would be trying to buy.

## Correctness gate

Runs before any timing; timing is not taken if it fails.

* one canonical checksum per range across **all** measured paths — vanilla
  heap, vanilla columnar, xp_batch heap, xp_batch pgColumnar, and pgColumnar
  repeated in the same backend. Every grouping key and every aggregate, sorted
  before hashing, so output ordering cannot enter it. Timing excluded.
* the same group count within each range.
* `sum(debit) - sum(credit) = sum(debit - credit)`, both in aggregate and
  per group (0 groups violate it).
* ranges 1/12, 3/12, 6/12, 12/12, an empty range (90..91 → 0 groups) and a
  single boundary period (12..12 → 200 groups).
* ZLFS checked against the same result, twice in one backend.

```
range 1..1    06ee205763e88a4c69acc3500e485869
range 1..3    9a53da4a03a9b707d0ffc164f32e40f2
range 1..6    ece42c97cbb5c90a1357d2eb4df41cee
range 1..12   135d70b3a7b8f075335c56de08a27a39
range 90..91  d41d8cd98f00b204e9800998ecf8427e
range 12..12  e48979f80b3dfbc2094b0ff9efe98cad
```

Also in `expected-checksums.txt`. Benchmark 02 (`09e54f9a`) and benchmark 04
(`6854be25`) are unchanged.

## Cache discipline

All primary numbers are warm-cache: one warm-up run per matrix point, then
five measured. No cold-cache experiment was run, so nothing here is a
cold-cache claim, and warm and cold are not mixed.

## Counters that are not available

Recorded as unavailable rather than derived from timings:

| wanted | status |
|---|---|
| rowgroups total | available — `pgcolumnar.row_group` |
| rowgroups read | available — counted in the adapter |
| rowgroups skipped | available — total minus read |
| rows in read rowgroups | available — counted in the adapter |
| rows actually decoded | **not available** — the fold API reports vectors left undecoded per group, but not a row count for them; `rows_vec_skipped` counts rows the adapter dropped for being in a ruled-out vector, which is a lower bound on what was not needed, not a measure of what was decoded |
| rows copied/materialized | available — 0 here; every group was served zero-copy |

## What this experiment does and does not support

Supported:

* On this dataset and this report, with row groups spanning 2–3 periods,
  `pgColumnar + xp_batch` completes faster than `heap + xp_batch` at 1/12,
  3/12, 6/12 and 12/12, and the margin narrows monotonically as the predicate
  widens.
* pgColumnar source cost tracks row groups read, close to linearly.
* Heap source cost is nearly flat in selectivity, because that source prunes
  nothing.
* Operator cost is the same for both sources at equal row counts.
* The stock executor's columnar-versus-heap crossover lies between 3/12 and
  6/12 single-core.

Not tested:

* Cold cache.
* Any heap arm on the fixed-offset path over this row shape — the layout
  forbids it, so "columnar helps" and "deform is expensive" are not separated.
* Group cardinality, skew, aggregate count, join depth. Only the period
  predicate varied.
* NULLs in the measured columns; they are `NOT NULL` by design here.
* numeric, varlena or NULL through the pgColumnar or ZLFS sources in a
  *measured* path — `contract_tests.sh` covers correctness for these.
* Scale beyond 1M rows.
* JIT, which this build does not have.
* Anything about 1C.

---

# Benchmark 05-B — Heap Deform Cost

05-A left an asymmetry: benchmark 04's *narrow fixed-layout* heap source beat
pgColumnar, while 05-A's *wide generic-deform* heap source lost to it. That
comparison is confounded — width, types, NULLability, attribute order, source
path and generator all differ between the two. 05-B isolates one variable.

> How much of the 05-A heap-source cost is tuple deformation, and how much is
> the wider physical row itself?

**Answer: it is overwhelmingly deformation.** On one physical table, generic
`heap_deform_tuple` costs **2.2–2.5× fixed-offset extraction**. The difficult
attribute layout adds only a few percent beyond that.

## The three arms

Two heap tables, **logically identical rows, same order, same payload**,
differing only in whether the five columns the report reads form a
fixed-width NOT NULL prefix. Definitions in `schema-layouts.sql`.

| arm | table | path | why |
|---|---|---|---|
| **A** | `reg2_bad` | deform | `comment` (varlena) at attnum 2, ahead of the projection — fixed *cannot* address it |
| **B** | `reg2_fixed` | deform **forced** | same rows, projection is a fixed prefix; deform forced anyway |
| **C** | `reg2_fixed` | fixed | the same table through fixed-offset extraction |

`B − C` is the strongest estimate of deformation cost: same physical table,
same rows, same pipeline. `A − B` is the layout effect beyond the mechanism.

The mode is **proved from outside**, by a counter the fixed path cannot
increment, not by trusting the flag passed in:

```
fixedlayout-deform   heap_path=deform  tuples_deformed=1000008  attrs_deformed=9000072
fixedlayout-fixed    heap_path=fixed   tuples_deformed=0        attrs_deformed=0
bad-fixed            ERROR: "reg2_bad" cannot use the fixed-offset path:
                            column "comment" is variable-width
```

Forcing exists only for this benchmark. Production path selection is
unchanged, there is no GUC, and `-fixed` on an unaddressable layout errors
rather than downgrading.

## Proof of equivalence

`verify-layouts.sql`, before any timing:

```
logical checksum IDENTICAL: a5fdd715c60cab8cbe03a5525bbc4df1   (all 9 columns, 1 000 008 rows)
period / company / account / payload-length / NULL distributions   all identical
SQL report identical over both layouts (200 groups)
```

The SQL equivalence is checked **before xp_batch is involved**, so a generator
or load mistake cannot be mistaken for a source-path effect.

### Physical size is *not* identical — recorded, not hidden

Reordering attributes changes alignment padding:

| | relation | pages | avg_width | buffers on a full scan |
|---|---|---|---|---|
| `reg2_bad` | 95 MB | 12 150 | 65 | `shared hit=12150` |
| `reg2_fixed` | 92 MB | 11 795 | 65 | `shared hit=11795` |

`reg2_fixed` is **2.92% smaller**. So `A − B` is "layout *and* a 3% size
difference", not layout alone. `B − C` is unaffected — it is the same table.

Both scans are fully cached (`shared hit` only, no `read=`), so no disk I/O is
being read as deform cost.

## Results

Warm cache, 1 warm-up + 5 measured runs, median (min–max). Same GUCs as 05-A,
set explicitly: `work_mem=64MB`, `enable_hashjoin=on`, `jit=off`,
`max_parallel_workers_per_gather=0`.

| predicate | arm | total | min | max | **source** | operators | rows selected |
|---|---|---|---|---|---|---|---|
| empty (90..91) | A bad / deform | 49.5 | 48.6 | 56.6 | **49.0** | 0.0 | 0 |
| | B fixed / deform | 47.7 | 45.6 | 51.0 | **47.2** | 0.0 | 0 |
| | C fixed / fixed | 19.0 | 18.0 | 20.4 | **18.6** | 0.0 | 0 |
| 1/12 | A bad / deform | 54.4 | 53.1 | 55.0 | **53.0** | 0.9 | 83 334 |
| | B fixed / deform | 49.4 | 48.2 | 60.5 | **47.9** | 0.9 | 83 334 |
| | C fixed / fixed | 22.3 | 22.1 | 23.9 | **20.9** | 0.8 | 83 334 |
| | *pgColumnar (context)* | *10.0* | *9.9* | *15.9* | *8.4* | *0.8* | *83 334* |
| 12/12 | A bad / deform | 81.8 | 78.2 | 83.3 | **74.0** | 7.4 | 1 000 008 |
| | B fixed / deform | 77.6 | 74.0 | 79.6 | **69.5** | 7.5 | 1 000 008 |
| | C fixed / fixed | 42.2 | 39.3 | 43.9 | **32.1** | 7.5 | 1 000 008 |
| | *pgColumnar (context)* | *44.1* | *42.0* | *76.1* | *36.0* | *7.3* | *1 000 008* |

### The two estimates

| predicate | A | B | C | **B − C** (deform) | **A − B** (layout) |
|---|---|---|---|---|---|
| empty | 49.0 | 47.2 | 18.6 | **28.6 ms, 2.54×** | 1.8 ms |
| 1/12 | 53.0 | 47.9 | 20.9 | **27.0 ms, 2.29×** | 5.1 ms |
| 12/12 | 74.0 | 69.5 | 32.1 | **37.4 ms, 2.17×** | 4.5 ms |

This is **Case 1**: `B >> C`. Generic tuple deformation is the dominant source
penalty.

**The empty range is the cleanest measurement in the experiment.** No rows are
emitted, no batch is materialized, no operator runs — it is scan plus tuple
access plus predicate evaluation and nothing else. Deform still costs 28.6 ms
more than fixed offsets, for a million tuples every one of which is rejected.
Deformation is being paid before any row survives.

`A − B` is 1.8–5.1 ms against a 2.92% (≈2.8 MB) size difference between the
tables, so most of it is plausibly the size, not the attribute order. The
honest statement is that the difficult layout costs **little beyond forcing
the deform path in the first place** — which it does absolutely, since fixed
cannot address it at all.

## Where the deform work goes

The counters answer §10 directly:

```
attrs_requested   5
attrs_deformed    9 000 072  =  9 attributes x 1 000 008 tuples
```

`heap_deform_tuple()` deforms **every attribute of the tuple descriptor** —
not only the requested ones, and not only up to the highest requested attnum.
On `reg2_fixed` the projection is attnums 1..5, yet all 9 are deformed, including
the two numerics and the varlena that nothing reads.

*Follow-up hypothesis, not implemented and not measured:* a deform bounded to
the highest requested attnum (as `slot_getsomeattrs` does) would cut this from
9 attributes to 5 on `reg2_fixed`, and from 9 to 9 on `reg2_bad` — so it would
help exactly the layouts that are already fast, and not the difficult one.
Left alone per the no-optimization rule.

## What this does to the 05-A conclusion

05-A reported that pgColumnar beat heap at every selectivity point. With the
heap source on **fixed offsets over the same wide row**, that reverses at full
scan:

| | 1/12 | 12/12 |
|---|---|---|
| heap, deform (`reg2_bad`, re-run here) | 53.0 | 74.0 |
| heap, fixed offsets | 20.9 | **32.1** |
| pgColumnar | **8.4** | 36.0 |

At 12/12 the fixed-offset heap source is now *faster* than pgColumnar (32.1 vs
36.0), restoring benchmark 04's ordering on a row shape ten times wider. At
1/12 pgColumnar still wins by 2.5×, and that margin is rowgroup pruning, which
the heap has none of.

So **05-A was substantially measuring deform cost, not a columnar advantage.**
The columnar advantage that survives is the one attributable to pruning.

This is also partly **Case 4**: pgColumnar's win at 1/12 is *not* caused by
heap deform, because it persists against the fixed-offset arm.

## Correctness gate

All three arms plus the SQL form over *both* physical tables, on the empty
range, 1/12 and 12/12. Checksums are identical to 05-A's, which is itself a
check that the two new layouts carry the same data:

```
range 90..91  d41d8cd98f00b204e9800998ecf8427e   0 groups
range 1..1    06ee205763e88a4c69acc3500e485869   200 groups
range 1..12   135d70b3a7b8f075335c56de08a27a39   200 groups
```

`sum(debit) − sum(credit) = sum(debit − credit)` holds everywhere.

## Limitations

* `A − B` carries a 2.92% relation-size confound; `B − C` does not.
* No `filter_ms` / `materialize_ms` split — isolating those needs hot-loop
  instrumentation that would change what is being measured. Recorded as not
  measured rather than estimated.
* Warm cache only. No cold-cache experiment was run.
* Only 3 of the 4 selectivity points; 3/12 and 6/12 were not needed, since the
  three measured points were unambiguous.
* The fixed-offset path was widened from int4 to int8 for this benchmark —
  without it arm C could not exist at all, since the report reads three int8
  columns. That is a capability addition to the *fixed* path, not an
  optimization of the deform path being measured.
* No optimization was applied during measurement.

---

# Benchmark 05-C — Projected Heap Deform

05-B established that generic tuple deformation is the dominant heap→batch
cost, and that the generic path deforms 9 attributes per tuple where the batch
asks for 5. 05-C tests one narrowly defined alternative:

> Walk a PostgreSQL heap tuple correctly — NULL bitmap, alignment, varlena
> headers — but materialize only the attributes the batch requested.

`XPB_HEAP_PROJECTED`. Experimental, never auto-selected; `XPB_HEAP_FIXED` and
`XPB_HEAP_DEFORM` are unchanged.

## Answer

**Case B, partially.** `fixed < projected < deform` at full scan, where
projected closes ~33% of the deform→fixed gap. At low selectivity projected is
*worse* than deform — for an identifiable implementation reason, not an
architectural one. The walk itself turns out to be cheap; what deform pays for
is materializing attributes nobody asked for.

## Implementation

A structural mirror of `heap_deform_tuple()`, reusing its own inline helpers
from `access/tupmacs.h` — `fetch_att_noerr()`, `align_fetch_then_add()`,
`first_null_attr()` — and its three-phase shape: a cached-offset prefix
(`firstNonCachedOffsetAttr`), a no-NULL run, and a tail that may contain
NULLs. Nothing about alignment, short/external varlena headers or NULL bitmap
interpretation is reinvented; it is delegated to those helpers, so the
implementation is explainable line-by-line against PostgreSQL's.

Two differences from `heap_deform_tuple`, and only two:

* it stops at the **highest requested attnum** instead of `natts`;
* it stores into the batch's typed columns instead of a `Datum`/`isnull` array
  of width `natts`, so an unused attribute costs a step and nothing more.

A tuple with fewer attributes than the projection needs `getmissingattr()`
semantics; that is **refused with an error** rather than guessed, since a
wrong answer there is silent corruption.

## Counters — walked versus materialized

Exactly the distinction §3 asks for, and it shows the two layouts behaving
differently:

| layout | attributes walked | attributes materialized |
|---|---|---|
| `reg2_fixed` (projection is attnums 1–5) | 5 000 040 = **5**/tuple | 5 000 040 = 5/tuple |
| `reg2_bad` (varlena at attnum 2) | 9 000 072 = **9**/tuple | 5 000 040 = 5/tuple |

On `reg2_bad` every attribute must still be walked — a later attribute's
position depends on the varlena ahead of it — so only materialization can be
skipped. That is the architecturally interesting case, and it is the one
`heap_deform_tuple` cannot improve on.

## Results

Warm cache, 1 warm-up + 5 measured runs, median (min–max), `source_ms`. Same
GUCs and protocol as 05-B. The whole matrix was run twice; the second run is
published, and **every number in this section is from that pass**. The first
pass is kept alongside it in `raw/projected/` but the two differ by 11-13% at
12/12 (fixed 40.3 against 35.7; bad-projected 60.5 against 65.0), enough to
move this section's headline ratio from 33% to 41%, so they are not pooled and
the first is not quoted.

| predicate | `reg2_fixed` fixed | `reg2_fixed` deform | `reg2_fixed` **projected** | `reg2_bad` deform | `reg2_bad` **projected** |
|---|---|---|---|---|---|
| empty (90..91) | 22.8 | 40.6 | **45.5** | 45.9 | **57.4** |
| 1/12 | 26.7 | 43.5 | **48.1** | 47.9 | **58.6** |
| 12/12 | 35.7 | 66.2 | **56.0** | 70.9 | **65.0** |

pgColumnar context, unchanged code: 7.7 ms at 1/12, 30.3 ms at 12/12.

### At full scan, where the comparison is clean

Nothing is rejected at 12/12, so predicate placement is irrelevant and the
arms differ only in decoding:

```
fixed      35.7          deform -> projected saves 10.2 ms
projected  56.0          projected -> fixed still costs 20.3 ms
deform     66.2          projected closes ~33% of the gap
```

**Projected closes about a third of the distance from full deform to fixed
offsets.** The unpublished first pass gave 41% on the same three arms; the
spread between two passes on this host is wide enough that the figure should be
read as "roughly a third", not as 33.4%.

On `reg2_bad`, where all 9 attributes must be walked regardless, projected
still beats deform (65.0 against 70.9). Skipping *materialization alone* —
with no reduction in walking — is worth ~8%.

Comparing the two projected arms isolates the walk: 56.0 walking 5 attributes
against 65.0 walking 9, for the same 5 materialized. **Walking four extra
attributes costs ~9 ms; materializing four extra costs ~10 ms.** They are the
same order, and neither alone explains the 30 ms gap to fixed offsets — the
rest is the per-attribute loop machinery that fixed offsets skip entirely.

### At low selectivity, projected loses — and why

This is an implementation asymmetry I introduced, not a property of projected
decoding, and it makes the empty-range case **not a clean answer to §11**:

* the **deform** arm evaluates the predicate straight out of its `Datum`
  array, *before* storing anything into the batch;
* the **projected** arm materializes all five columns into the batch and only
  then reads the predicate column back out of it.

So on a rejected row, projected pays five stores that deform does not. With
every row rejected (the empty range) that is the entire difference: 45.5
against 40.6.

*Follow-up hypothesis, not implemented (§19).* Evaluate the predicate as soon
as the predicate column has been walked, then `break` out of the attribute
loop. Unlike `heap_deform_tuple`, which is all-or-nothing, the projected
walker can abandon a tuple mid-way — it would skip both the remaining
materialization *and* the remaining walk. That should turn the empty-range
result around, and it is the obvious next measurement rather than something to
slip into this one.

## Correctness

53 cases in `contract_tests.sh`, all passing. New for the projected path, each
compared against both `heap_deform_tuple` and PostgreSQL itself: varlena
before a requested attribute; varlena *between* requested attributes; NULL
before a requested attribute; NULL *in* a requested attribute; every nullable
attribute NULL in every row; external toasted varlena before a requested
attribute; the toasted column itself when requested; mixed int4/int8;
`INT64_MIN`/`INT64_MAX`; NULL across a batch boundary; numeric at exact
decimal scale.

The existing corruption regressions and layout guards are untouched.

### The unused toasted value is not detoasted

§12, proved through the TOAST relation's block counters rather than by
instrumenting the hot path:

```
skipping the toasted column     0 toast blocks touched
requesting it                 299 toast blocks touched
```

Locating the next attribute needs only the 18-byte pointer's length header.
Touching the toast relation at all would mean the value had been followed.

## Where this leaves the architecture

| | 1/12 | 12/12 |
|---|---|---|
| heap, full deform | 47.9 | 70.9 |
| heap, projected | 58.6 | 65.0 |
| heap, fixed offsets | 26.7 | 35.7 |
| pgColumnar | **7.7** | **30.3** |

Projected deform is a real but partial improvement, and only at full scan in
its current form. It does not bring the generic heap path near either fixed
offsets or pgColumnar.

The 05-B conclusion stands and is sharpened: the heap→batch penalty is mostly
**materializing attributes nobody asked for**, and partly the per-attribute
loop machinery itself. Removing the first recovers about a third of it;
removing the second is what the fixed-offset path does, and it is only
available on layouts that permit it.

## Limitations

* The empty-range and 1/12 comparisons are confounded by predicate placement
  (above). Only the 12/12 column is a clean decode-versus-decode measurement.
* `reg2_fixed` is 2.92% smaller than `reg2_bad` (carried over from 05-B), so
  cross-layout comparisons carry that; same-table comparisons do not.
* Warm cache only; no cold-cache experiment.
* No optimization was applied during measurement. No SIMD, prefetch, vector
  predicates, planner integration, expression compilation or JIT.
* `XPB_HEAP_PROJECTED` is not auto-selected anywhere. `xpb_heap_source_create()`
  is unchanged, and 05-A/05-B numbers are unaffected.
* A tuple shorter than the projection is refused rather than handled with
  missing-value semantics.

---

# Benchmark 05-D — Early Predicate Pushdown into Projected Heap Decode

05-C ended on an open hypothesis: the projected walker, unlike
`heap_deform_tuple`, can abandon a tuple mid-way, so it should be able to
evaluate the predicate the moment the predicate attribute is available and
stop. 05-D implements exactly that and measures it.

> How much can the projected heap decoder save if it evaluates the predicate
> as soon as the predicate attribute is available and stops walking the tuple
> immediately when the row is rejected?

`XPB_HEAP_PROJECTED_EARLY`. Experimental, never auto-selected. `XPB_HEAP_FIXED`,
`XPB_HEAP_DEFORM` and `XPB_HEAP_PROJECTED` are unchanged and all four live in
the same binary, so late-versus-early is a controlled comparison.

## Answer

**Case A, and the 05-C hypothesis is confirmed.** At selective predicates
early rejection is **1.4–1.8× faster than projected-late**, and at full scan
the two are indistinguishable. The mechanism counters show why directly: on
the empty range a rejected tuple walks **1 attribute instead of 9**.

Early rejection also overtakes full deform at selective predicates — 05-C's
loss at low selectivity was predicate placement, exactly as diagnosed, and it
is gone. It does **not** reach fixed-offset extraction, and it does not come
near pgColumnar's rowgroup pruning.

## Implementation

One new mode, sharing the 05-C walker rather than a second one. In each of
the walker's three phases, immediately after an attribute is stored:

```c
if (early && attnum + 1 == st->pred_attno &&
    !xpb_heap_pred_passes(batch, st->pred_col, nrows,
                          st->pred_lo, st->pred_hi))
{ rejected = true; break; }
```

and the tuple is abandoned: `nrows` is never incremented, so the partially
written batch slot is overwritten by the next tuple.

Deliberately *not* built (§3, §23): no expression executor, no fmgr dispatch
per row, no operator lookup, no SIMD or branchless filtering. The predicate is
`int4 BETWEEN lo AND hi` and nothing else.

Four properties worth stating explicitly:

* **The predicate attnum is explicit, not assumed.** `pred_attno` is recorded
  in the source descriptor and the walker tests `attnum + 1 == st->pred_attno`.
  It happens to be 1 in this benchmark's tables; the code does not rely on that.
* **NULL rejects the row.** `xpb_heap_pred_passes()` returns false when the
  predicate attribute is NULL, before reading the value — `BETWEEN` on NULL is
  UNKNOWN, and a WHERE filter drops the row. No integer sentinel is used.
  Tested on a table whose predicate column is nullable.
* **An unsupported predicate column is an ERROR, not a downgrade.** A non-int4
  predicate column, or the early mode with no predicate at all, raises rather
  than silently falling back to late evaluation — a silent fallback would make
  every subsequent measurement meaningless.
* **Accepted rows take the identical path.** The rejection test is the only
  addition; layout handling, alignment, varlena headers and NULL bitmap
  interpretation are the 05-C code, unmodified.

## Counters — the mechanism, not an inference

Increments live in the walk loop. Across all 25 measured runs every counter is
**bit-identical**, which is itself the check that they are not being derived
from selectivity after the fact.

| predicate | layout | rejected early | walk/tuple | mat/tuple | walk per *rejected* tuple | walk per *accepted* tuple |
|---|---|---|---|---|---|---|
| empty (90..91) | `reg2_bad` | 1 000 008 | **1.00** | 1.00 | **1.00** | — |
| empty (90..91) | `reg2_fixed` | 1 000 008 | **1.00** | 1.00 | **1.00** | — |
| 1/12 | `reg2_bad` | 916 674 | 1.67 | 1.33 | **1.00** | 9.00 |
| 1/12 | `reg2_fixed` | 916 674 | 1.33 | 1.33 | **1.00** | 5.00 |
| 12/12 | `reg2_bad` | 0 | 9.00 | 5.00 | — | 9.00 |
| 12/12 | `reg2_fixed` | 0 | 5.00 | 5.00 | — | 5.00 |

For comparison, projected-*late* walks 9.00 (`reg2_bad`) or 5.00
(`reg2_fixed`) and materializes 5.00 per tuple at **every** predicate.

This is §13's strongest case, and it comes out exactly as specified: on the
empty range the early path walks one attribute, tests it, rejects, and moves
to the next tuple. No join probe, no aggregate, no output row.

`mat/tuple` is 1.00 rather than 0.00 on a rejected tuple because the predicate
attribute *is* stored into its batch slot before being tested. That store is
then abandoned — the row is never committed — but it is real work and the
counter reports it rather than hiding it.

## Results

Warm cache, 1 warm-up + 5 measured runs per pass, **5 passes** = 25
measurements per cell. Median `source_ms` (min–max). Same GUCs, tables and
pipeline as 05-B/05-C. Raw logs in `raw/early/`, aggregate in
`results-early-predicate.csv`.

| predicate | `reg2_fixed` fixed | `reg2_fixed` deform | `reg2_fixed` proj-late | `reg2_fixed` **proj-early** | `reg2_bad` deform | `reg2_bad` proj-late | `reg2_bad` **proj-early** |
|---|---|---|---|---|---|---|---|
| empty (90..91) | 21.4 | 43.2 | 53.4 | **37.8** | 49.3 | 66.6 | **37.6** |
| 1/12 | 22.1 | 43.6 | 51.7 | **37.5** | 50.8 | 69.2 | **39.5** |
| 12/12 | 35.6 | 70.7 | 61.0 | **60.6** | 75.9 | 74.1 | **74.6** |

pgColumnar context, code unchanged since 05-A: 8.3 ms at 1/12, 32.4 ms at 12/12.

`total_ms`, rows selected and all counters per cell are in
`results-early-predicate.csv`; nothing is buried in the logs.

### Late − early (§20)

| predicate | layout | late | early | late − early | ratio | deform / early |
|---|---|---|---|---|---|---|
| empty | `reg2_bad` | 66.6 | 37.6 | **+29.0 ms** | **1.77×** | 1.31× |
| empty | `reg2_fixed` | 53.4 | 37.8 | **+15.6 ms** | **1.41×** | 1.14× |
| 1/12 | `reg2_bad` | 69.2 | 39.5 | **+29.7 ms** | **1.75×** | 1.29× |
| 1/12 | `reg2_fixed` | 51.7 | 37.5 | **+14.2 ms** | **1.38×** | 1.16× |
| 12/12 | `reg2_bad` | 74.1 | 74.6 | −0.5 ms | 0.99× | 1.02× |
| 12/12 | `reg2_fixed` | 61.0 | 60.6 | +0.4 ms | 1.01× | 1.17× |

Connecting mechanism to timing, and no further: on `reg2_bad` at the empty
range the walk drops 9.00 → 1.00 attributes/tuple and materialization 5.00 →
1.00, and the time drops 1.77×. The counters establish that the work was
avoided; they do not by themselves establish that the walk is the *only* thing
that changed, because abandoning a tuple also skips the batch-commit
bookkeeping. No stronger causal claim is made here.

### The 12/12 control (§10, §15)

`tuples_rejected_early = 0` at 12/12, and walk and materialization counters are
**identical** to projected-late. There is therefore no mechanism by which the
two arms can differ except the per-tuple range comparison itself.

During this benchmark one pass nevertheless reported `fixedlayout-early` 20%
slower than `fixedlayout-late` at 12/12. §10 requires investigating that before
interpreting anything, so it was, and it was **host measurement noise**, not
overhead:

* Per-pass medians across the five published passes — `fixedlayout-late`
  60.6 / 69.2 / 56.2 / 62.8 / 59.4 against `fixedlayout-early`
  60.6 / 65.7 / 57.4 / 58.9 / 61.4 — overlap; the 20% gap did not reproduce.
* 6.1% of all `source_ms` measurements in this benchmark exceed 1.25× their
  own cell median, spread across **all eight** arms including
  `fixedlayout-fixed` and `pgcolumnar`. This host produces occasional spikes;
  block-ordered runs
  let one land entirely inside one arm's five runs and move its median.
* A dedicated interleaved paired A/B — late and early alternating within one
  session, 15 pairs per layout, so every pair sees the same conditions — was
  run twice (`raw/early/*-1212-paired-{a,b}.txt`):

| run | layout | paired (early − late) | 95% CI | early faster in |
|---|---|---|---|---|
| a | `reg2_bad` | −2.13 ms | [−7.35, +3.10] | 8/15 pairs |
| a | `reg2_fixed` | +2.18 ms | [−0.19, +4.55] | 4/15 pairs |
| b | `reg2_bad` | +2.65 ms | [−1.81, +7.11] | 5/15 pairs |
| b | `reg2_fixed` | +1.67 ms | [−2.45, +5.80] | 8/15 pairs |

All four confidence intervals include zero, and the sign of the point estimate
is not stable between runs.

**§15 answer: the cost of carrying early-predicate capability when it cannot
prune is below this harness's resolution.** The paired point estimates span
−2.1 to +2.7 ms on a 59–74 ms source phase and none is distinguishable from
zero. That is the expected shape:
the added work is one `int32` range comparison per tuple against a value
already in a batch column.

## Correctness gate

68 cases in `contract_tests.sh`, all passing, plus the 05-D benchmark gate
(seven arms and both SQL layouts must produce one checksum and one group count
per predicate range). Timing is not taken if the gate fails.

New for the early path, each compared against **both** projected-late **and**
PostgreSQL's own SQL over the same table:

* predicate accepts all rows; rejects all rows; accepts a subset (two ranges)
* predicate column NULL → row rejected
* predicate before a varlena, and before an **external toasted** varlena
* accepted rows still decode int8 correctly and preserve NULL validity
* the early mode without a predicate is refused rather than downgraded

### A defect this found in the test harness itself

`xpb_contract_probe` gained the optional `pred_lo`/`pred_hi` pair, which means
it can no longer be `STRICT` — and without `STRICT` the executor stops
NULL-checking its other arguments. The first version dereferenced a NULL
`relname` and **segfaulted the backend**. Fixed by checking arguments 0–2
explicitly, and a half-specified range (one bound NULL) is now refused too,
since accepting it would silently have measured the unfiltered path. Six
cases cover this, the last of which asserts the backend is still alive
afterwards. The probe is test-only and on no measured path.

### An early reject reads no toast blocks (§17)

Same method as 05-C — the TOAST relation's own block counters, with
`pg_stat_force_next_flush()` so the reading is not stale — rather than
instrumenting the hot path:

```
early reject before a toasted column     0 toast blocks touched
```

A rejected row stops before the toast pointer is ever followed.

## pgColumnar context (§21)

The comparison that is now meaningful is *tuple-level early rejection* against
*rowgroup-level pruning*, not columnar against full deform:

| | 1/12 | 12/12 |
|---|---|---|
| heap, full deform (`reg2_bad`) | 50.8 | 75.9 |
| heap, projected-late (`reg2_bad`) | 69.2 | 74.1 |
| heap, **projected-early** (`reg2_bad`) | **39.5** | **74.6** |
| heap, **projected-early** (`reg2_fixed`) | **37.5** | **60.6** |
| heap, fixed offsets (`reg2_fixed`) | 22.1 | 35.6 |
| pgColumnar | **8.3** | **32.4** |

pgColumnar was not re-run for 05-D beyond this context measurement and its
code is unchanged; its numbers agree with 05-A/05-C within noise.

Early rejection is **4.5–4.8× behind pgColumnar at 1/12** (37.5 on `reg2_fixed`,
39.5 on `reg2_bad`, against 8.3). The two mechanisms are
not close: pgColumnar skips whole rowgroups without touching them, while the
early heap path still visits every tuple and pays a page-at-a-time scan to do
it. Tuple-level rejection removes decode work; it cannot remove the scan.

At 12/12, where neither mechanism can prune, pgColumnar (32.4) and fixed-offset
heap extraction (35.6) are comparable, and both are well ahead of any generic
heap decode.

## What this does and does not support

* Early predicate evaluation in a projected heap decoder is a **real and large
  win at selective predicates** — 1.4–1.8× over late evaluation — and is free
  when it cannot prune.
* It turns the generic heap path from *slower* than full deform at low
  selectivity (05-C) into *faster* than full deform (1.14–1.31×).
* It does **not** close the gap to fixed-offset extraction: 37.5 against 22.1
  at 1/12 on the same table, still 1.7×.
* It does **not** approach columnar rowgroup pruning.

## Limitations

* **The predicate sits at physical attnum 1 in both tables, which is the best
  case.** A rejected tuple walks exactly 1 attribute here. The walker steps
  over intervening attributes to reach its target — `walk/accepted = 9.00` on
  `reg2_bad` shows it doing so — so a predicate at physical attnum *k* would
  cost *k* walked attributes per rejected tuple, and the benefit would shrink
  accordingly. §12's predicate-position experiment (attnum 1 / 5 / 9) was
  **not performed**: it needs a third table and a report mode of its own, which
  is more than the "only if easy" the brief allows. The scaling statement above
  is a mechanism-supported expectation, not a measurement.
* One predicate, one type: `int4 BETWEEN`. Nothing here supports a claim about
  general predicate pushdown.
* Single-column predicate only. No conjunctions, no disjunctions.
* `reg2_fixed` is 2.92% smaller than `reg2_bad` (carried from 05-B), so
  cross-layout comparisons carry that; same-table comparisons do not.
* Warm cache only. A cold-cache run would be dominated by I/O and would not
  measure decode.
* The 05-A/05-B/05-C numbers reproduce within this host's noise but are not
  bit-comparable across passes; this README does not restate them as if they
  were.
* `XPB_HEAP_PROJECTED_EARLY` is **not** auto-selected anywhere.
  `xpb_heap_source_create()` and automatic source selection are unchanged.

## The decision after 05-D (§29)

The heap→batch spectrum is now measured end to end, on one dataset, one
pipeline, and two controlled layouts:

```
heap arms measured on reg2_fixed; pgColumnar on reg2_col (1/12 / 12/12, source_ms)

fixed offsets      22.1 / 35.6     lower bound; only on eligible layouts
projected + early  37.5 / 60.6     general heap path, any layout
projected late     51.7 / 61.0     superseded by the above
full deform        43.6 / 70.7     what PostgreSQL gives you
pgColumnar          8.3 / 32.4     rowgroup pruning
```

On `reg2_bad`, where the fixed-offset path is not available at all, the same
spectrum reads 39.5 / 74.6 for projected-early against 50.8 / 75.9 for full
deform — the general heap path's realistic case.

**The evidence points to A: the heap→batch decoder is good enough to stop
working on.** Three observations support that:

1. The two remaining heap wins are structural, not incremental. Fixed-offset
   extraction is already implemented and already the floor; it is unavailable
   on `reg2_bad`-shaped layouts for a reason that no decoder change can fix.
2. Between projected-early (37.5) and fixed (22.1) the residue is the
   per-attribute loop machinery itself. Removing it means compiling the
   projection or vectorizing the walk — both explicitly out of scope (§23),
   and both large enough to be their own project.
3. The gap that actually matters for the architecture is the 4.5× to
   pgColumnar at 1/12, and that is a *scan-granularity* gap, not a decode gap.
   No amount of tuple-level decoding work closes it.

The case for B would be a predicate-position experiment showing the benefit
collapsing for realistic schemas where the predicate is not the first
attribute. That is the one open measurement, and it is cheap enough to settle
before committing to A — but it would refine this result, not change the
spectrum above.

---

# Benchmark 05-E — Group Cardinality and Memory Boundary

05-A..05-D characterised the source. 05-E moves the bottleneck downstream on
purpose:

> How does the batch pipeline behave as the number of live groups grows from
> hundreds to tens of thousands, and where is its current memory/capacity
> boundary?

Nothing about heap decode, pgColumnar, the projected paths, predicate
pushdown or the ZLFS format changes here. The aggregate was measured, not
optimised.

## Answer

**Three regimes, and the boundary is a compile-time constant rather than a
memory limit.**

* Up to ~6 000 groups the aggregate is nearly flat and the pipeline is
  source-dominated.
* From half-full onwards the aggregate climbs steeply and dominates the
  operators, entirely because of hash-table **load factor** — the measured
  probe counts track textbook linear probing.
* At **12 288 groups** the run stops with a clean ERROR. That is
  `V2_GRP_LOAD`, a `#define` in `xpb_v2_report.c`, and it is reached while the
  table holds 393 kB of live entries. PostgreSQL, on the same query, keeps
  49 152 groups in 17 MB without spilling. **The limit is a chosen constant,
  not a resource.**

## Holding everything but cardinality still

The group key is `(company_group, account_group, company_key)`, and
`company_group` is a function of `company_key`, so cardinality is exactly

```
distinct(company_key) x distinct(account_group)
```

and `account_group` is a **dimension payload**. So the ladder is walked by
rewriting 384 dimension rows — `xpe_set_cardinality(k)` — and nothing else.
Across the whole benchmark:

| held fixed | how |
|---|---|
| fact table | byte-identical; never rewritten, `pg_relation_size` checked each pass |
| row count | 983 040 at every point |
| predicate, selectivity | `period BETWEEN 1 AND 12`, all rows, every point |
| join shape | same two joins, same dimension row counts (128 and 384) |
| dimension hash occupancy | 128/192 and 384/768 of their load limits, constant |
| ZLFS zone | built once over the fact table, reused by every point |

Rebuilding a differently-keyed fact table per point — the obvious way to do
this — would have moved source cost and join occupancy alongside cardinality
and confounded all three. The measured flatness of `source_ms` and both join
phases below is therefore a check on the construction, not a discovery.

Sizing: 128 companies x 384 accounts = 49 152 pairs, each occurring exactly
20 times. When `k` divides 384 every group holds exactly `20 * 384 / k` rows,
so the distribution is uniform by construction rather than by sampling (§8 —
no skew here). `k = 80, 88, 92` do not divide 384; they exist only to resolve
the knee, and `verify-cardinality.sql` reports their group-size spread
(max/min 1.25) rather than letting them pass as uniform.

## The hash table being measured

`xpb_v2_report.c`, unchanged by this benchmark:

```c
#define V2_GRP_CAP   16384
#define V2_GRP_LOAD  (V2_GRP_CAP * 3 / 4)     /* = 12288 */
```

Open addressing, linear probing, `sizeof(V2Group) = 32` bytes, allocated once
with `palloc0` before a single row is read. **It never grows and never
rehashes.** Those two counters are reported as constant zeros rather than
omitted, so that "this implementation does not grow" is a recorded
measurement and not something the reader has to infer.

## Results

Primary arm is ZLFS (§2): the zone is already materialised, so `source_ms` is
0.0 and what remains is operator behaviour. 5 passes x (1 warm-up + 5 measured
runs) = 25 runs per cell, median `ms`. Raw in `raw/group-cardinality/`,
aggregate in `results-group-cardinality.csv`.

| groups | rows/group | load | source | join1 | join2 | **aggregate** | operators | total | status |
|---|---|---|---|---|---|---|---|---|---|
| 256 | 3840 | 0.016 | 0.0 | 2.5 | 2.8 | **3.8** | 9.1 | 86.8 | OK |
| 1 024 | 960 | 0.063 | 0.0 | 2.4 | 2.8 | **4.4** | 9.6 | 81.8 | OK |
| 2 048 | 480 | 0.125 | 0.0 | 2.5 | 2.9 | **4.5** | 10.0 | 86.6 | OK |
| 4 096 | 240 | 0.250 | 0.0 | 2.4 | 2.8 | **4.4** | 9.6 | 82.2 | OK |
| 6 144 | 160 | 0.375 | 0.0 | 2.5 | 2.9 | **4.9** | 10.3 | 81.7 | OK |
| 8 192 | 120 | 0.500 | 0.0 | 2.6 | 2.9 | **9.1** | 14.6 | 89.9 | OK |
| 10 240 | 96 | 0.625 | 0.0 | 2.4 | 2.9 | **14.0** | 19.3 | 91.4 | OK |
| 11 264 | 87 | 0.688 | 0.0 | 2.5 | 2.9 | **16.6** | 22.0 | 96.2 | OK |
| 11 776 | 83 | 0.719 | 0.0 | 2.4 | 2.8 | **17.4** | 22.8 | 96.4 | OK |
| 12 288 | 80 | 0.750 | 0.0 | 2.4 | 2.9 | **19.3** | 24.7 | 96.7 | OK |
| 12 416 | — | — | — | — | — | — | — | — | **ERROR** |
| 16 384 | — | — | — | — | — | — | — | — | **ERROR** |
| 24 576 | — | — | — | — | — | — | — | — | **ERROR** |
| 49 152 | — | — | — | — | — | — | — | — | **ERROR** |

`total` includes `open` (71-76 ms), which for the ZLFS arm is the registry
scan -- it reads and validates every zone file in the data directory,
including the 19 MB zone this benchmark rebuilds at the start of each pass. It
is timed as its own phase, is constant across the ladder, and is excluded from
`source` and `operators`, so it offsets every row equally and cannot affect
the cardinality result (section 17). Building the zone (~180 ms) happens once
per pass, before any measured run, and the runner reports it separately.

The heap fixed-offset arm carries the same aggregate under a real source
(`source_ms` 39.1-41.0 throughout) and its aggregate column agrees with the
ZLFS arm at every point -- 3.4, 4.2, 4.6, 4.5, 5.1, 9.4, 14.0, 16.5, 17.6,
19.6 -- which is the check that the aggregate behaviour is a property of the
operator and not of the source.

### Source and join really are flat

Across a 48x change in group count: `source_ms` 0.0 everywhere (ZLFS) and
39.1-41.0 (heap); `join1` 2.4-2.6; `join2` 2.8-2.9. §13 asked for this to be
measured rather than assumed, and it holds.

### The aggregate follows load factor, not group count

| groups | load | probes/lookup measured | linear-probe theory | max probe | aggregate ms |
|---|---|---|---|---|---|
| 256 | 0.016 | 1.016 | 1.008 | 2 | 3.8 |
| 1 024 | 0.063 | 1.034 | 1.033 | 4 | 4.4 |
| 2 048 | 0.125 | 1.076 | 1.071 | 4 | 4.5 |
| 4 096 | 0.250 | 1.149 | 1.167 | 5 | 4.4 |
| 6 144 | 0.375 | 1.286 | 1.300 | 12 | 4.9 |
| 8 192 | 0.500 | 1.540 | 1.500 | 37 | 9.1 |
| 10 240 | 0.625 | 1.909 | 1.833 | 63 | 14.0 |
| 11 264 | 0.688 | 2.201 | 2.100 | 84 | 16.6 |
| 11 776 | 0.719 | 2.397 | 2.278 | 84 | 17.4 |
| 12 288 | 0.750 | 2.707 | 2.500 | 91 | 19.3 |

Theory is the textbook successful-search cost for linear probing,
`0.5 * (1 + 1/(1-load))`; the lookups here are overwhelmingly successful
(970 752 hits against 12 288 inserts at the last point). Measurement tracks it
within a few percent at every load factor.

**This is the §22 distinction, and it comes out on the side of saturation.**
The aggregate does not slow down because there are more groups — between 256
and 6 144 groups, a 24x increase, it moves 3.8 to 4.9 ms. It slows down
because the table fills: every point above half-full costs roughly what
linear probing says it should. The hash function is ordinary
multiply-and-xor with no final mixing and takes the table index from the low
bits, which is a fair thing to be suspicious of — but the average probe counts
do not convict it. The `max_probe` tail (91 slots at 0.75) is worse than the
average suggests and is the one place a weakness could still be hiding.
Recorded as a hypothesis for a later experiment, not acted on here (§23).

### Where the curve stops being smooth

The knee is at **load factor 0.5**, 8 192 groups: 4.9 to 9.1 ms, an 86% jump
for a 33% increase in groups, after five points that were nearly flat. §21
suggested this point might matter more than the hard ERROR, and it does — it
is where the implementation stops being cardinality-insensitive, and it
arrives at half the nominal capacity, well before anything fails.

### Memory does not grow

| | |
|---|---|
| aggregate table | **524 288 bytes at every cardinality point** |
| entry size | 32 bytes |
| initial capacity | 16 384 slots |
| final capacity | 16 384 slots |
| growth events | 0 |
| rehash events | 0 |
| dimension hashes | 3 072 + 24 576 bytes, also constant |

There is no allocation curve to plot. The table is sized once, before the
first row, and `bytes_per_group` is therefore an artefact of division rather
than a property of the implementation: it *falls* from 2 048 B/group at 256
groups to 42.7 B/group at 12 288. §14 permits that metric only where the
accounting supports it directly; here it does not, and it is reported as the
fixed allocation it actually is. No process RSS was used.

## The boundary (§26)

| | |
|---|---|
| last successful group count | **12 288** |
| first failing group count | **12 289** (the run requesting 12 416) |
| hash capacity at failure | 16 384 slots, 524 288 bytes |
| live bytes at last success | 12 288 x 32 = **393 216 bytes** |
| failure trigger | **compile-time constant**, `V2_GRP_LOAD = V2_GRP_CAP * 3 / 4` in `xpb_v2_report.c` |
| not | allocation failure, `work_mem`, OOM, or spill |

> Superseded by Hash Aggregate Growth v1, below: the table now grows and
> `V2_GRP_LOAD` no longer exists. The 05-E measurements are left exactly as
> taken, as the fixed-capacity baseline that milestone is compared against.

```
ERROR:  v2_register_report: group hash overflow (12288 groups, cap 16384)
```

The error names the count and the capacity, fires before any write, and is
raised identically at 12 416, 16 384, 24 576 and 49 152 requested groups —
deterministic, clean, non-corrupting, and the backend survives. Every failing
point was run the full 5 passes to establish that, rather than being observed
once.

## Correctness gate

`05-E gate PASS` in all five passes. **Per-group** equality, not grand totals:
at each of the ten successful cardinality points, the ZLFS arm, the heap arm
and PostgreSQL's own SQL must produce one identical md5 over every
(company_group, account_group, company_key, debit, credit, net) tuple, sorted
canonically, with NULLs rendered as explicit markers rather than swallowed by
concatenation. The gate additionally asserts the group count is `128 * k` and
that `sum(debit) - sum(credit) = sum(debit - credit)` in every row. Timing is
not taken if it fails.

## PostgreSQL baseline (§19, §20)

Single core, `max_parallel_workers_per_gather = 0`, same query, medians of 25
runs:

| groups | xp_batch total (heap arm) | PostgreSQL |
|---|---|---|
| 256 | 50.4 | 286.8 |
| 8 192 | 57.0 | 288.2 |
| 12 288 | 66.5 | 302.4 |

**The plan shapes are not the same, and the difference must not be read as
executor overhead.** PostgreSQL applies eager aggregation:

```
Finalize HashAggregate          rows=12288   Batches: 1  Memory Usage: 6161kB
  -> Hash Join (account)        rows=49152
     -> Hash Join (company)     rows=49152
        -> Partial HashAggregate rows=49152  Batches: 1  Memory Usage: 17425kB
           -> Seq Scan          rows=983040
```

It aggregates 983 040 rows down to 49 152 **before** the joins, then joins two
tiny dimensions and finalises. xp_batch joins all 983 040 rows and then
aggregates. Two consequences:

1. PostgreSQL's cost is nearly flat across the ladder (286.8 to 302.4 ms) because
   its dominant hash table is the partial aggregate, which holds 49 152 groups
   *at every point of the ladder*. Its flatness is a property of the plan, not
   evidence that its aggregation scales better.
2. **PostgreSQL holds 49 152 groups — 4x the count at which xp_batch errors —
   in 17 MB with `Batches: 1`, no spill.** The plan shape is constant across
   all three measured points; no switch to `GroupAggregate`, no partitioning,
   no batching.

**This is an execution-path measurement, not a claim about PostgreSQL.** On
this controlled workload the hand-wired xp_batch pipeline completed in 50-67 ms
against 287-302 ms for the plan above. The two are not the same computation
arranged the same way: PostgreSQL runs a planner-selected plan that aggregates
before the joins, through the full executor, with MVCC and general expression
evaluation; xp_batch is one hard-coded query shape that joins every row and
then aggregates. Nothing here supports a statement of the form "xp_batch is N
times faster than PostgreSQL". It also stops at a quarter of the cardinality
PostgreSQL handles on the same query.

## Limitations

* **`k` is bounded above by the aggregate's own capacity**, so §5's ~100 000
  group point is unreachable by construction and was not faked. The ladder
  ends where the implementation ends.
* The instrumentation is not free and is included in the numbers above, but
  **its cost is not quantified here**: the comparison that was published
  rested on a 15-run measurement that was never saved to `raw/`, so it has
  been withdrawn rather than restated. What can be said without it: the probe
  counters are O(rows) and independent of group count, so they offset the
  cardinality curve by a constant rather than distorting its shape.
  05-A..05-D's published timings predate them.
* Uniform distribution only. No skew, no hot keys (§8, §28).
* One row count. §27's optional row-count control was **not performed**: it
  needs a second fact table and a second report mode, and every
  (company, account) pair in this dataset lives entirely within one `period`,
  so the row count cannot be halved with a predicate without also halving the
  cardinality. Per-row and per-group costs are therefore not separated
  experimentally here; the flat region below load 0.375 is the closest thing
  to a per-row reading.
* `ns/group` is reported in the CSV but is not a complexity model: it falls
  from 14 844 to 798 and then rises again to 1 571 on the ZLFS arm, because
  group *sizes* change by a factor of 48 across the ladder (§14). The heap arm
  traces the same shape at different values; mixing the two would make the
  sequence meaningless.
* The ZLFS arm's `open_ms` is dominated by a registry scan over all zone files
  in the data directory. 55 orphaned zone files left by earlier sessions
  (their source relations dropped) were moved aside to `/tmp/zlfs-orphans`
  before measuring. That was environment hygiene done before the published
  passes, not a measurement: no before/after `open_ms` figure was recorded, so
  none is quoted. The published passes show `open_median_ms` 70.9-75.6. No zone
  belonging to 05-A..05-D was touched and their arms still run.
* `zlfs_drop_zone(lo, hi)` keys on the period range alone, not on the
  relation, so any benchmark that drops zones for [1..12] -- 05-A and 04 both
  do -- also drops this one's. The runner therefore rebuilds its own zone at
  the top of every pass rather than inheriting one, which makes 05-E
  independent of gate ordering. The cross-relation reach of `zlfs_drop_zone`
  is recorded here as an observation; changing it is out of scope (section 32).
* Warm cache only.
* No spill was implemented, no capacity was enlarged, and the hash function,
  sizing policy, key encoding and collision strategy are untouched (§23, §24,
  §32).

## The decision after 05-E (§34)

The measurements put this at **C, with a correction to how the question is
usually posed.**

The capacity boundary is real and close: 12 288 groups is not a large number
for a register workload — a year of postings across a few thousand accounts
and a few hundred cost centres passes it easily. So the boundary is the
blocker, and growth or spill has to be designed (option C).

But the numbers say the ceiling is not where the design discussion usually
puts it:

* It is **not a memory problem**. 393 kB of live entries, against 17 MB
  PostgreSQL spends on 4x the groups without spilling. Nothing was exhausted;
  a constant was reached.
* The cost curve breaks at **half** the nominal capacity, not at the limit.
  Any growth policy that waits for the load limit inherits a table that has
  already been slowing down for a factor of two. Whatever replaces this should
  grow at or before 0.5, which also means a growth design has to answer what
  the rehash costs, since 05-E establishes that today there is none.
* Aggregation is **not** expensive in itself. At realistic occupancy the
  aggregate is 4-5 ms against a 39-41 ms source — option B is not supported.

So: not A (the boundary is too close to move on to skew), not B (aggregation
is cheap where it fits), not D (12 288 groups is not sufficient for realistic
register workloads, which is precisely why it must be fixed before 10M scale
is worth measuring). C — and the design target that follows from the data is a
table that grows at load 0.5, not one that merely raises 16 384 to a bigger
constant.

---

# What benchmark 05 established, end to end

Six milestones, one pipeline, one dataset family. The chain is meant to be read
as an argument, and each step's conclusion is the next step's *tested* premise
rather than an assumption:

1. **Operator cost is source-independent and scales with rows emitted.** Not
   assumed — 05-E holds the fact table byte-identical across a 48x change in
   group count and both join phases stay flat, and the heap arm's aggregate
   column tracks the ZLFS arm's at every ladder point. This is the bridge that
   lets a source experiment and an aggregate experiment on different tables add
   up.
2. **So the cost is in acquiring the representation**, and it is dominated by
   generic tuple deformation: 2.2–2.5x fixed-offset extraction on one physical
   table (05-B), with the excess going to materializing attributes nobody asked
   for and to the per-attribute loop itself (05-C).
3. **Decode has a floor, and it has been reached.** Projecting recovers about a
   third; rejecting early recovers another 1.4–1.8x and costs nothing when it
   cannot prune (05-C, 05-D). What remains is 1.7x behind fixed offsets and
   4.5x behind pgColumnar at 1/12 — and that residue is **scan granularity**,
   not decode. No further decode work closes it.
4. **Downstream, the next limit was not aggregation — it was a `#define`.**
   05-E found the group hash erroring at 12 288 groups while holding 393 kB,
   against PostgreSQL's 17 MB for four times as many, with the cost curve
   breaking at half full. Growing at load 0.5 removed it: probe cost flat
   across 14x cardinality, ceiling 12x higher, and the only remaining growing
   cost is rehash — a policy parameter.

Net: acquisition cost is floored by scan granularity, operators are cheap and
linear in rows, and the apparent scaling limits were compile-time constants.
That is the three-cost decomposition `docs/architecture.md` already assumed,
now measured.

## The condition on step 1, and what it costs

05-A through 05-D all ran at **200 groups** — entirely inside the flat
aggregate regime that 05-E only later bounded. At 147 456 groups the aggregate
is 77 ms against a 33–49 ms source: the phase those four milestones treated as
negligible is now the larger one.

So step 1 holds *as measured*, at low cardinality, and the composite reading
"source dominates, operators are cheap" is **conditional on group count**.
Nothing in 05-A..05-D is wrong; the synthesis simply does not extend to the
high end of 05-E's ladder, and none of those sections was re-run there.

## What this baseline does not establish

* **Growth is one code path.** At this milestone, only the benchmark report
  function's group hash grew. `xpb_typed_pipeline.c`, `xpb_batch_groupagg.c`
  and `xpb_batch_hashjoin.c` still carry the fixed ceiling and the 0.5 knee.
  Nothing spills anywhere. *(Superseded in part: Dimension Hash Growth v1,
  below, made it three code paths — the same file's two dimension hashes.)*
* **Uniform distribution only.** Every probe figure rests on it, and probe
  stability is exactly what a hot key attacks.
* **One query shape, hand-wired, no planner, no MVCC, warm cache, single
  core.** Two comparisons against PostgreSQL appear here; both carry the
  plan-shape difference and neither supports a claim of the form "N times
  faster than PostgreSQL".
* **The binding boundary was then the dimension hashes**, `V2_DIM1_CAP` = 256
  and `V2_DIM2_CAP` = 1024, at 192 and 768 keys — the same class of
  compile-time constant the growth milestone removed from the group table.
  *(Removed in turn by Dimension Hash Growth v1, below; both are now initial
  capacities.)*
* **05-D's predicate-position experiment was never run.** Its 1.4–1.8x is the
  best case, with the predicate at physical attnum 1. That is the one
  unmeasured premise under "decode is done".

Timings are comparable only within a section. This host drifts between
sessions, and in one case within a day, by more than several of the effects
measured here; every section states the build and the day it was taken on.

---

# Hash Aggregate Growth v1

05-E established that the aggregation ceiling was artificial: a static
16 384-slot table with a 3/4 load limit, erroring deterministically at 12 289
groups, with the cost curve already breaking at half full. This milestone
replaces it.

> Replace the fixed-capacity group hash with a memory-resident growing table,
> preserving the hash function, probing strategy, key representation,
> aggregate semantics and result correctness.

The policy was **preregistered**: grow when the table is half full, double the
capacity. It was fixed before any number was looked at and has not been tuned
since, which is the only reason the causality below is readable.

No spill. No planner changes. No change to the hash algorithm, the key layout
or the source paths.

## Read the timings within this section only

**Every timing here was taken on 2026-09-27. Timings in 05-A through 05-E were
taken on 2026-09-24 and are not comparable with them.** This host drifts
between sessions by more than the effects being measured. Measured on a code
path this milestone does not touch at all — the fixed-offset heap source:

```
fixedlayout-fixed, 1..12, source_ms      05-D/05-E, 09-24:  35.6
                                         same arm,  09-27:  57.4
```

1.6x on unchanged code. PostgreSQL's own baseline for the same query moved
286 -> 450 ms over the same interval. So all cross-session comparisons have
been removed from this section rather than caveated, and the two comparisons
that matter are made **within** one session: the rehash control below, and
xp_batch against PostgreSQL measured on the same day.

## Answer

**Outcome A, with a B component that is now quantified.**

The 05-E knee is gone. Load stays at or below 0.5 and is 0.375 at most
measured points, probe cost is flat from 10 240 to 147 456 groups, and the
last successful cardinality is **147 456 groups — 12x the old ceiling**, which
is where the *dimension* hashes run out, not the group table.

The part that still grows is the rehash: at 147 456 groups the ladder reports
26.2 ms of a 77.2 ms aggregate — about a third — and a paired control run
separately confirms the accounting is sound and slightly conservative.

## What changed, and what deliberately did not

| | |
|---|---|
| capacity | 16 384 fixed → 16 384 **initial**, doubling |
| threshold | 3/4 hard limit, ERROR → 1/2, grow and continue |
| hash function | unchanged, character for character |
| key equality, linear probing | unchanged |
| key and aggregate-state layout | unchanged |
| NULL and grouping semantics | unchanged |

The hash is now reached through a function rather than being written inline,
because rehash has to recompute the same value from the stored keys. That is
the only reason it moved.

Initial capacity stayed at 16 384 rather than dropping to something smaller,
and section 27 forbids sizing the table from the cardinality the benchmark is
about to produce.

### Rehash reinserts, it does not copy

A slot index is a function of the capacity, so memcpying occupied slots to the
same indexes would leave groups where the probe sequence for their key can no
longer reach them. Every subsequent lookup would then create a *second* group
with the same key and the sums would split in silence. Every live group is
reinserted with the new mask, and a group that finds no free slot raises
rather than being dropped — unreachable in a table that is at most a quarter
full after doubling, but the alternative to checking is losing a group's
accumulated sums with no other symptom.

### Overflow is checked before anything is allocated

Slot count and byte size are both tested before the allocation, and the table
is left untouched if either cannot be represented. Capacity is never wrapped.
Past `MaxAllocSize` the growth is refused cleanly rather than worked around
with a huge allocation — that is the group table's own next boundary, around
16.8M groups, and it was not reached here.

### The turnover sums are checked too

Separate from table growth, and found by review of this milestone rather than
by it: the int8 turnover accumulation was unchecked and wrapped silently on
input PostgreSQL refuses outright. It now raises `bigint out of range` with
PostgreSQL's own errcode. The benchmark gate could not have caught it — the
gate asserts `sum(debit) - sum(credit) = sum(debit - credit)`, and under
two's-complement wraparound both sides wrap identically, so the identity held
on a wrong answer. The identity is a useful invariant; it is not an overflow
detector, and there is now a regression that says so.

Because this adds two checked adds per aggregated row, every timing in this
section includes them and no earlier section's does.

## Correctness

90 cases in `contract_tests.sh`, all passing. Every growth count is compared
**per group** against PostgreSQL — a rehash that lost or duplicated a group
would keep the row count right while splitting one group's sums, so grand
totals are not accepted anywhere in this milestone.

| case | growths |
|---|---|
| 31 groups, below half of 64 | 0 |
| 32 groups, exactly half of 64 | 0 |
| 33 groups, one past half | 1 |
| 65 groups | 2 |
| 129 groups | 3 |
| 256 groups, exactly half of 512 | 3 |
| 257 groups, one past half | 4 |
| 512 groups | 4 |

The threshold is pinned exactly (section 12). The invariant, recorded in the
code and in the tests: **a group is only ever added to a table that is
strictly less than half full, so occupancy can reach exactly capacity/2 but
the load factor never exceeds 0.5.** The decision reads `ngroups` and
`capacity` only, so it cannot depend on where probing happened to stop.

Also covered: load factor never above 0.5; peak memory is exactly old+new
across a rehash; repeated growth leaves one live table; a second, smaller
report **in the same backend** starts fresh at the initial capacity with no
groups carried over; a growth refused by a test-only ceiling errors cleanly,
the backend survives, and the next fitting report still answers correctly; a
capacity that is not a power of two is refused; the two probe marks behave as
specified; int8 overflow raises rather than wrapping.

The probe tests needed their own dataset. The threshold dataset varies one key
component, and since the hash multiplies each key by an odd constant,
consecutive keys land on a stride that never collides — every chain is length
1 at any load factor and a reset is invisible.

## Results

Primary arm is ZLFS, so `source_ms` is 0.0 and what remains is operator
behaviour. 5 passes x (1 warm-up + 5 measured runs) = 25 runs per cell,
median ms, all 2026-09-27. Raw in `raw/hash-growth/`, aggregate in
`results-hash-growth.csv`, recomputable with `check-summaries.py`.

Two fact tables. `reg2_card` is the 05-E table; `reg2_card2` (192 x 768 pairs,
1 032 192 rows) exists only because `reg2_card` cannot express more than
49 152 groups.

| groups | cap | load | growths | agg | rehash | agg−rehash | probes/lookup | probe cur | probe life | MB | peak MB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 256 | 16 384 | 0.016 | 0 | 8.9 | 0.00 | 8.93 | 1.016 | 2 | 2 | 0.50 | 0.50 |
| 2 048 | 16 384 | 0.125 | 0 | 10.8 | 0.00 | 10.82 | 1.076 | 4 | 4 | 0.50 | 0.50 |
| 6 144 | 16 384 | 0.375 | 0 | 13.8 | 0.00 | 13.77 | 1.286 | 12 | 12 | 0.50 | 0.50 |
| 8 192 | 16 384 | 0.500 | 0 | 17.1 | 0.00 | 17.11 | 1.540 | 37 | 37 | 0.50 | 0.50 |
| 10 240 | 32 768 | 0.313 | 1 | 15.2 | 0.86 | 14.38 | 1.200 | 17 | 37 | 1.00 | 1.50 |
| 12 288 | 32 768 | 0.375 | 1 | 17.2 | 0.90 | 16.37 | 1.275 | 17 | 37 | 1.00 | 1.50 |
| 24 576 | 65 536 | 0.375 | 2 | 21.1 | 2.46 | 18.73 | 1.282 | 10 | 37 | 2.00 | 3.00 |
| 49 152 | 131 072 | 0.375 | 3 | 27.2 | 5.68 | 21.48 | 1.312 | 17 | 37 | 4.00 | 6.00 |
| 24 576 † | 65 536 | 0.375 | 2 | 22.5 | 2.50 | 19.87 | 1.308 | 16 | 32 | 2.00 | 3.00 |
| 49 152 † | 131 072 | 0.375 | 3 | 29.2 | 5.67 | 23.34 | 1.316 | 12 | 32 | 4.00 | 6.00 |
| 98 304 † | 262 144 | 0.375 | 4 | 48.0 | 12.13 | 34.15 | 1.279 | 17 | 32 | 8.00 | 12.00 |
| **147 456** † | 524 288 | 0.281 | 5 | 77.2 | 26.21 | 50.99 | 1.284 | 11 | 32 | 16.00 | 24.00 |

† on `reg2_card2`, which has 5% more rows than `reg2_card`. The two overlap at
24 576 and 49 152 and agree to within that row-count difference once
normalised per input row; the raw overlap should not be read as agreement.

The heap fixed-offset arm carries the same aggregate under a real source
(`source_ms` 32.8–48.7 today) and its aggregate column tracks the ZLFS arm's
shape; it is 1–12 ms higher throughout, so the two are not interchangeable
point by point.

### The 05-E comparison, withdrawn

05-E's fixed-capacity numbers were taken in a different session on a host that
has since drifted 1.6x on unchanged code. The per-point comparison table that
stood here has been removed rather than caveated:

> Cross-session measurements were not stable enough to quantify the
> steady-state overhead of dynamic capacity independently of rehash.

What is still established, because it does not depend on timing at all: at
12 288 groups the fixed table held load 0.75 and erred one group later; the
growing table holds the same 12 288 groups at load 0.375 with one growth, and
probes per lookup fall from 2.707 to 1.275. Those are deterministic counters,
identical in all 25 runs.

### Growth shape (section 20)

From the live counters, not inferred:

```
groups   10 240 →  capacity  32 768 after 1 growth,  load 0.313
groups   24 576 →  capacity  65 536 after 2 growths, load 0.375
groups   49 152 →  capacity 131 072 after 3 growths, load 0.375
groups   98 304 →  capacity 262 144 after 4 growths, load 0.375
groups  147 456 →  capacity 524 288 after 5 growths, load 0.281
```

Load settles into a 0.25–0.5 sawtooth, exactly as a doubling policy implies.

### Probe behaviour (section 21)

Two marks, because one number was answering two questions. `probe life` is the
worst chain ever walked, which happens just before a growth when the table is
at its densest; `probe cur` is reset on every growth and describes the table
that actually answered the query. Previously there was one counter, never
reset, reported as though it described the grown table.

| | 05-E fixed at its ceiling | growing, 10K–147K groups |
|---|---|---|
| probes/lookup, all lookups | 2.707 | **1.20 – 1.32** |
| probes/lookup, final table only | 2.707 | **1.205** |
| longest chain in the answering table | 91 | **10 – 17** |
| longest chain ever walked | 91 | 32 – 37 |

`probes_per_lookup` was **not** split the way the probe marks were, so the
1.20–1.32 column pools every lookup, including those made at smaller
capacities before each growth. It is therefore a figure about the *policy*, not
about the table that answered the query. The final-table figure comes from the
pre-sized control below, which does all its lookups at capacity 524 288:
**1.2048** against the grown table's pooled **1.2842**. Both are recorded in
`raw/hash-growth/2026-09-27-rehash-control.txt` and asserted by
`check-summaries.py`. Splitting the counter itself is the obvious follow-up and
was not done here, since the control already measures the number.

So the story is better than the single number allowed:

```
before grow   chain reaches 32-37
after grow    current mark falls back to 10-17
```

Rehash does not merely make room, it restores the table's quality. Holding
load at or below 0.5 keeps probes per lookup **flat across a 14x change in
cardinality**. The hash function was not touched and on this evidence did not
need to be.

### Memory (section 22)

| groups | capacity | current | peak | bytes/group |
|---|---|---|---|---|
| 12 288 | 32 768 | 1.00 MB | 1.50 MB | 85 |
| 49 152 | 131 072 | 4.00 MB | 6.00 MB | 85 |
| 98 304 | 262 144 | 8.00 MB | 12.00 MB | 85 |
| 147 456 | 524 288 | 16.00 MB | 24.00 MB | 114 |

Peak is **1.5x current at every growth point**, because old and new tables are
both live across the rehash. `grp_cxt_bytes`, measured by the memory system
rather than by this code's arithmetic, is the live table plus about 8 KB of
block overhead at every point — five growths leave one live table, not six.

### Rehash cost, with a control (section 23)

| growths | groups | rehashed | rehash ms | % of aggregate |
|---|---|---|---|---|
| 1 | 12 288 | 8 192 | 0.90 | 5% |
| 2 | 24 576 | 24 576 | 2.46 | 12% |
| 3 | 49 152 | 57 344 | 5.68 | 21% |
| 4 | 98 304 | 122 880 | 12.13 | 25% |
| 5 | 147 456 | 253 952 | 26.21 | **34%** |

The doubling series reinserts about 1.7x the final group count by the end.

`rehash_ms` is a counter, so it deserves an independent check. `run-rehash-control.sh`
pre-sizes the table to the final capacity through the test-only policy hook,
so both arms end with the **identical** final table — same capacity, same
147 456 groups, same load 0.281, same current probe mark of 11 — and differ
only in whether they got there by doubling. The arms alternate inside one
connection, 10 pairs:

| arm | growths | aggregate ms | reported rehash_ms | probes/lookup |
|---|---|---|---|---|
| natural, 16 384 → 524 288 | 5 | 40.1 | 17.34 | 1.2842 |
| pre-sized to 524 288 | 0 | 26.2 | 0.00 | 1.2048 |

Paired difference **+14.3 ms, 95% CI [+11.8, +16.8]**, natural slower in 10/10
pairs, two warm-up pairs discarded.

**The counter reads slightly high.** `rehash_ms` says 17.3 ms; the end-to-end
marginal cost of having doubled is 14.3, and the interval does not reach it.
The counter is therefore an upper bound on what doubling costs, not an estimate
of it: it times the whole grow function, including touching memory the pre-sized
arm pays for elsewhere when it first fills its table. So the accounting is
confirmed as *sound and conservative* — a 3 ms overstatement at the top of the
range — rather than exact. An earlier version of this control, with one warm-up
pair instead of two, gave +16.4 [9.8, 23.0] and did contain the counter; that
interval was carried by a single first-pair outlier and is not the one
published.

Two cautions about this table. The control was run separately from the ladder,
and this host moves even within a day: the ladder's 147 456-group cell reports
26.21 ms of rehash where the control's natural arm reports 18.26 ms for the
same configuration. So the comparison is the control's own two arms against
each other, paired inside one connection — not the control against the ladder.
And the difference is everything about starting at 16 384, which is the rehash
plus five allocate-and-zero passes, not rehash in isolation; that errs
conservative, since the natural arm also gets a cache-locality advantage while
the table is small, so it cannot inflate `rehash_ms`.

Pre-sizing is not a proposal — section 27 forbids sizing from a known
cardinality precisely because it hides what is being measured. It is used here
only as the control that measures it.

The important shape is next to the table: **`agg−rehash` grows more slowly
than `agg`, but not dramatically so** — 34.15 to 50.99 ms from 98 304 to
147 456 groups is close to linear in the group count. Rehash is what makes the
difference between the two curves, and it is the part that can be changed by
policy; steady-state aggregation scaling roughly with the number of groups is
not in itself a problem.

## PostgreSQL context (sections 24, 20)

Single core, same query, same session as everything above, plan captured at
each point.

| groups | xp_batch, heap arm | PostgreSQL | xp_batch memory | PostgreSQL memory |
|---|---|---|---|---|
| 8 192 | 72.2 | 450.1 | 0.50 MB | 21.0 MB |
| 49 152 | 75.2 | 494.2 | 4.00 MB | 34.0 MB |
| 98 304 | 103.0 | 906.6 | 8.00 MB (12 peak) | 86.1 MB |

PostgreSQL memory sums both hash aggregates of its two-stage plan, which is
what it actually occupies.

**This is an execution-path measurement, not a claim about PostgreSQL.** On
this controlled workload the hand-wired xp_batch pipeline completed in 103 ms
against 907 ms for the PostgreSQL plan below, at 98 304 groups. The two are
not the same computation arranged the same way:

```
PostgreSQL                          xp_batch
planner-selected plan               hand-wired pipeline, no planner
aggregate before the joins          join all rows, then aggregate
full executor, MVCC, expressions    one hard-coded query shape
```

PostgreSQL's plan shape is constant at every point — `HashAggregate`,
`Batches: 1`, no spill, no switch to `GroupAggregate` — so its own points are
comparable with each other. Nothing here supports a statement of the form
"xp_batch is N times faster than PostgreSQL".

## Limitations

* **The last successful cardinality was limited by the dimensions, not the
  group table.** 147 456 groups is 192 companies x 768 accounts, exactly what
  `V2_DIM1_CAP` (256) and `V2_DIM2_CAP` (1024) held at their 3/4 load limits —
  the same kind of compile-time constant this milestone removed from the group
  hash, and the binding one as of this milestone. *(Dimension Hash Growth v1,
  below, removed that limit too.)* The group table's own next boundary is
  `MaxAllocSize`, around 16.8M groups, untested.
* **The steady-state cost of dynamic capacity is not quantified.**
  Cross-session measurements were not stable enough to separate it from
  rehash, and it was not worth a measurement campaign of its own. What is
  quantified is the rehash cost, by the paired control above.
* Instrumentation is included in every number here: 05-E's counters, the
  growth counters, and the two checked adds per row. Earlier sections' timings
  include none of the latter.
* Every measured run builds its table from the initial capacity. Nothing is
  pre-sized from the known cardinality except the control, and nothing is
  carried across reports — so the full rehash chain is paid on every run,
  which is the pessimistic reading of the policy.
* No spill, no batches, no `work_mem` integration. No shrinking.
* Uniform distribution only. No skew. Warm cache only. One query shape.
* `reg2_card2`'s dimensions sit at their load limit to reach 147 456 groups,
  so its join phases are not comparable with `reg2_card`'s.
* The growth policy has not been varied. 0.5 and 2x were measured as
  preregistered; any alternative is a separate experiment.

## The decision after this milestone (section 39)

The old answer was "12 288 groups, because `#define`". The measured answer is:

> In-memory aggregation now scales to 147 456 groups at 16 MB with probe cost
> flat and load bounded at 0.5. The first genuine boundary is no longer the
> group table — it is the dimension hashes, at 192 and 768 keys. The first
> genuine *cost* is rehash, which the ladder puts at a third of aggregate time
> at the top of the range and which a paired control independently confirms
> rather than leaving to a counter.

*(That conclusion stood at this milestone. Dimension Hash Growth v1, below,
removed the dimension boundary it names; the 147 456 figure and everything
above it are unaffected, having been measured before either table grew.)*

The evidence points at **A and C together, in that order**:

1. **A — growth solved the boundary, so skew is the honest next test.** Every
   number here is from a uniform distribution, and the probe stability this
   result rests on is exactly what a hot-key distribution would attack.
2. **C — rehash policy is the one cost still growing**, and the control makes
   it measurable. Growing 4x instead of 2x, or growing earlier, are
   single-variable experiments against this preregistered baseline — not to be
   done by adjusting the constant that was just measured.

Not B: memory is 16 MB at 147K groups against PostgreSQL's 86 MB for two
thirds as many. Not D: probe behaviour did not degrade. Not E (10M rows) yet —
the dimension caps had to be raised first, which is the same conversation as C,
and is what Dimension Hash Growth v1 below did.

---

# Dimension Hash Growth v1

Hash Aggregate Growth v1 ended by naming its own successor boundary: 147 456
groups was reached only by holding both dimension hashes at their 3/4 load
limit — 192 of 256 slots and 768 of 1024. Skew attacks load factor. Run skew
against saturated dimension tables and a regression is not attributable,
because the group hash and the dimension hashes would degrade together.

> Give both dimension hashes the same controlled growth the group hash has, and
> measure that the saturation regime is gone.

**This is housekeeping, not a research milestone.** It removes a confound. The
policy is the same one already preregistered for the group hash — grow when the
table is half full, double, rehash, no spill — reused deliberately rather than
chosen again, so that nothing in this milestone is a new tuning decision.

## Read the timings within this section only

Every timing here was taken on 2026-09-27, in the same session as the Hash
Aggregate Growth numbers above. 05-A through 05-E were taken on 2026-09-24 and
are not comparable. The host's drift is documented above: 1.6x on unchanged
code between those two dates.

The two comparisons that carry weight are both internal to this section: the
ladder, where only dimension cardinality varies, and the paired pre-sized
control, where both arms alternate inside one connection.

## Answer

**Outcome A — the boundary moved, and nothing downstream noticed.**

Dimension cardinality reaches **4x the old fixed limit** (192 → 768 companies,
768 → 3072 accounts) with load never above 0.5, join probe cost flat across a
12x change in dimension size, and rehash below a quarter of a millisecond at the
widest point. The old ceiling was a `#define`, not a resource, exactly as 05-E
found for the group table.

## What changed, and what deliberately did not

| | |
|---|---|
| dim1 capacity | 256 fixed → 256 **initial**, doubling |
| dim2 capacity | 1024 fixed → 1024 **initial**, doubling |
| limit | `ERROR` at 3/4 load → grow at 1/2 load |
| remaining limit | `MaxAllocSize`, or a slot count that would overflow int |
| hash function | unchanged, `(uint32) key * 2654435761u` |
| probing | unchanged, open-addressed linear |
| slot layout | unchanged (`V2Dim1` 12 bytes, `V2Dim2` 24 bytes) |
| join semantics | unchanged |
| group hash | unchanged — already grown in the previous milestone |
| everything else | unchanged; no spill, no planner change, no new GUC |

Three tables in `xpb_v2_report.c` now grow and nothing else in the extension
does. `docs/TYPED_BATCH_CONTRACT.md` carries the per-table list, including two
fixed tables found during this update that have no capacity guard at all.

## Correctness

A rehash that lost a dimension entry would not raise an error. Every fact row
referencing that key would simply fail the join and vanish — a **wrong answer**,
not a failure. So the gate is per-group equality against PostgreSQL at every
ladder point, not grand totals:

```
point 1  batch  <md5>  groups=12288      point 1  sql  <md5>  groups=12288
...
NOTICE:  dim growth gate PASS: batch and PostgreSQL agree per group at every
         dimension cardinality
```

The rehash reinserts with the **new** mask rather than copying to the same slot
index; a slot index is a function of capacity, so a `memcpy` would leave entries
where the probe sequence for their key can no longer reach them. That is the
specific bug the per-group gate is built to catch.

`extension/xp_batch/test/contract_tests.sh` carries 15 further cases (105
total), including the one that matters most here: **a collision case**. The
ladder below uses sequential keys, which this hash maps collision-free at any
load, so the ladder cannot exercise collision behaviour at all — see the
limitation below. The contract test uses keys of stride 256 so that keys
differing by a multiple of the capacity collide exactly, and asserts that the
high-water probe mark resets on growth.

## Results (section 34)

Group cardinality pinned at 12 288, input rows 1 032 192, fixed-offset heap
source, every key present at every point. 5 passes x 5 runs = 25 measurements
per point; medians.

| comp | acct | groups | d1 cap | d1 ent | d1 load | d1 grow | d2 cap | d2 ent | d2 load | d2 grow |
|---|---|---|---|---|---|---|---|---|---|---|
| 64 | 256 | 12 288 | 256 | 64 | 0.2500 | 0 | 1024 | 256 | 0.2500 | 0 |
| 128 | 512 | 12 288 | 256 | 128 | 0.5000 | 0 | 1024 | 512 | 0.5000 | 0 |
| 129 | 513 | 12 255 | 512 | 129 | 0.2520 | 1 | 2048 | 513 | 0.2505 | 1 |
| 192 | 768 | 12 288 | 512 | 192 | 0.3750 | 1 | 2048 | 768 | 0.3750 | 1 |
| 384 | 1536 | 12 288 | 1024 | 384 | 0.3750 | 2 | 4096 | 1536 | 0.3750 | 2 |
| 768 | 3072 | 12 288 | 2048 | 768 | 0.3750 | 3 | 8192 | 3072 | 0.3750 | 3 |

Point 2 is the threshold itself: 128 of 256 and 512 of 1024, load exactly
0.5000, still no growth. Point 3 is one key past it and each table has grown
once. The 12 255 groups at point 3 is arithmetic, not loss: 129 x 95 = 12 255,
the closest the shape gets to 12 288 with 129 companies.

Timings, same points, milliseconds:

| comp | acct | total | source | d1 build | d2 build | join1 | join2 | aggregate | d1 rehash | d2 rehash |
|---|---|---|---|---|---|---|---|---|---|---|
| 64 | 256 | 62.8 | 40.9 | 0.150 | 0.032 | 3.4 | 3.5 | 13.4 | 0.000 | 0.000 |
| 128 | 512 | 60.3 | 39.1 | 0.151 | 0.046 | 3.2 | 3.4 | 13.1 | 0.000 | 0.000 |
| 129 | 513 | 60.7 | 38.9 | 0.153 | 0.072 | 3.3 | 3.4 | 13.5 | 0.004 | 0.023 |
| 192 | 768 | 64.3 | 41.0 | 0.162 | 0.098 | 3.3 | 3.6 | 14.4 | 0.004 | 0.025 |
| 384 | 1536 | 63.2 | 40.1 | 0.177 | 0.181 | 3.3 | 3.5 | 14.7 | 0.010 | 0.070 |
| 768 | 3072 | 61.5 | 38.7 | 0.242 | 0.396 | 3.3 | 3.6 | 14.0 | 0.023 | 0.175 |

What the timings say, and do not say:

* **Join cost is flat.** join1 3.2–3.4 ms and join2 3.4–3.6 ms across a 12x
  change in dimension cardinality and a 8x change in dimension table bytes:
  6.1% and 5.7% between the extreme medians. The baseline for calling that flat
  is this dataset's own noise, not an external band, and the two sides have to
  be the same statistic to be comparable. Within a *single* cardinality the five
  per-pass medians span 9.4% and 14.3% — 1.5x and 2.5x the variation across the
  whole ladder. The per-pass medians also show no ordering by cardinality: the
  narrowest point gives 3.30/3.30/3.40/3.40/3.60 and the widest
  3.30/3.20/3.50/3.30/3.20. So the honest reading is "no effect resolved", not
  "a small cost measured".

  `check-summaries.py` enforces that comparison — the across-ladder spread of
  medians must stay below the within-cardinality spread of per-pass medians —
  rather than a constant. Measured sensitivity, by scaling join1 at the widest
  point in both the raw passes and the CSV: it passes at 1.05x and fails from
  1.08x, so the 8% class of regression this claim rests on is caught.

  Secondary observation, not the baseline: the 25 *raw* runs inside one
  cardinality span 42% to 94% of their own median. That is an extreme value set
  by a single spike per cell, which is why it is not what the gate compares
  against — paired with an already-smoothed median it left roughly 15x headroom
  and would have passed a 60% regression.
* **Rehash is negligible here**, unlike in the group hash. 0.023 ms and
  0.175 ms at the widest point, against a 61.5 ms total: the dimension tables
  hold thousands of entries where the group table held a hundred thousand.
* **The total does not move.** 60.3–64.3 ms with no trend against dimension
  cardinality. Total is dominated by the 38.7–41.0 ms source phase, which this
  milestone does not touch.
* **Nothing here is a speed claim.** No arm was made faster. The result is that
  a boundary moved without a cost appearing.

## Proof that growth is what happened (section 35)

Four independent lines, none of which relies on the growth counter alone.

**1. The capacities are the doubling sequence from the declared initial
capacity.** Halving each final capacity once per recorded growth returns 256 and
1024 — `V2_DIM1_CAP` and `V2_DIM2_CAP` — at every point. Checked mechanically by
`check-summaries.py`, so a growth that was not a doubling would fail the build.

**2. The load bound holds, by construction and in the data.** An entry is only
added to a table strictly less than half full, so occupancy reaches at most
`capacity/2`. Measured maximum load is 0.5000, never above. The growth decision
reads only `entries` and `capacity`, never a probe count, so it is independent
of probe order and of the key distribution.

**3. The byte accounting closes, and no predecessor table survives.** Reported
bytes equal `capacity x sizeof(slot)` at every point, and peak equals
`current + current/2` at every growth — the two tables being briefly live
together during a rehash. Independent leak evidence comes from
`MemoryContextMemAllocated` on each table's own context, with AllocSet's
behaviour accounted for: an allocation above the 8192-byte chunk limit gets a
block of its own that *is* returned to malloc on `pfree`, so a surviving
predecessor of that size would show up. At the four freed *generations* whose
predecessor was above that limit — dim1's 12 288-byte table and all three of
dim2's — a leak would have been caught and was not. At dim1's two sub-limit
generations (3 072 and 6 144 bytes) it could not have been: AllocSet keeps those
chunks on a freelist by design and the block stays. There the per-group checksum
gate is the evidence instead. These counts are generations of a table, not ladder
points. `check-summaries.py` encodes exactly this and its blind
spot; every assertion in it was mutation-tested to confirm it fires.

**4. A paired pre-sized control, 50 pairs alternating in one connection.** The
test hook pre-sizes the tables to 2048 and 8192 so that both arms end with the
same entries and the same final capacities, differing only in whether they
doubled their way there.

That the two arms also produce the same *answer* is a separate check, because
capacities and growth counts cannot show it: a pre-sized table and a naturally
grown one reach 2048 slots by different insert orders, so their slot contents
differ, and only a per-group comparison rules out one of them having lost an
entry. Both arms return md5 `1680d9c7…` on 12 288 groups — the same checksum the
ladder's gate already matched against PostgreSQL at this point — so the pre-sized
arm agrees with the grown arm and with PostgreSQL. The runner asserts it
(`arm equivalence PASS`) and keeps it in its own artifact,
`raw/dim-growth/2026-09-27-dimgrowth-armcheck.txt`, so that it does not perturb
the five published measurement passes.

```
                              natural      pre-sized     delta
dim1 + dim2 build (median)     0.319 ms      0.176 ms    +0.143 ms
reported rehash, natural arm   0.136 ms            --
growths                        3 + 3         0 + 0
final capacities               2048 / 8192   2048 / 8192
```

Per pair, the extra build time in the natural arm is +0.130 ms median and the
rehash it reports is 0.136 ms median, leaving **−0.003 ms unaccounted** (range
−0.190 to +0.256, straddling zero). So the reported rehash counter fully
explains the cost of having grown; there is no additional steady-state penalty
at build-phase granularity.

The end-to-end total cannot resolve it and is not used to: the paired total
delta is +0.15 ms median, −1.13 ms mean, spread −23.9 to +22.1 ms, positive in
27 of 50 pairs. A 0.14 ms effect is two orders of magnitude below that spread.
The rehash cost is stated at the granularity that resolves it and at no other.

## Limitations

* **This ladder measures load factor and growth, not collision behaviour.**
  Keys are sequential from 1, and an odd multiplicative hash maps sequential
  keys to distinct slots at any load, so `max_probe` is 1 at every point in the
  table above and probes-per-lookup is exactly 1.0000. That is a property of the
  dataset, not a result. Collision behaviour is exercised only in
  `contract_tests.sh`, with stride-256 keys, and is the subject of the skew
  milestone.
* **The fact table is regenerated per point.** Unlike 05-E, which varied a
  dimension payload and left the fact table byte-identical, varying the number
  of distinct dimension *keys* requires the fact rows to reference them or they
  drop out of the join. Row count (1 032 192), group count (12 288), period
  range and fact table layout are all held constant instead; the reasoning is in
  the header of `schema-dim-cardinality.sql`.
* **4x is where the ladder stopped, not where the mechanism stops.** 3072
  accounts is 8192 slots and 196 608 bytes. Nothing was measured beyond it and
  no claim is made about it.
* **Pre-sizing is diagnostic only.** `xpb_dim_test_policy` exists for the
  control only — `contract_tests.sh` does not use it, forcing growth with real
  key counts instead. The production path always starts at the
  declared initial capacity. Being process-local statics, they must be set in
  the same connection as the report.
* **Still no spill.** `MaxAllocSize` is now the boundary for all three tables.
  A workload past it gets an `ERROR`, which is a development guard and not a
  final production policy.
* **Dimension hashes elsewhere are untouched.** `xpb_batch_hashjoin.c`,
  `xpb_batch_partition.c` and `xpb_typed_pipeline.c` still have fixed dimension
  tables that error at 3/4 load. This milestone changed the measured report path
  only.

## The decision after this milestone

The confound is gone, which was the whole point:

> Both dimension hashes and the group hash now grow under one preregistered
> policy with load bounded at 0.5. A skew result can now be attributed to the
> distribution rather than to whichever table happened to be saturated.

Next is **05-F Heap Block-Range Pruning**, the research milestone this series
left a hole in — 05-D concluded the residual gap to pgColumnar was scan
granularity and then every milestone since went downstream instead. Skew follows
it, and 10M scale last. The reasoning is in
`docs/roadmap/post-checkpoint-order.md`.

Two correctness questions are carried separately and must not be folded into
either: `docs/roadmap/groupagg2-int64-overflow.md` and
`docs/roadmap/fixed-hash-silent-drop.md`.
