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
int8     (debit_cents, credit_cents)    73.9 ms
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
| heap, deform (05-A's arm) | 53.0 | 74.0 |
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
