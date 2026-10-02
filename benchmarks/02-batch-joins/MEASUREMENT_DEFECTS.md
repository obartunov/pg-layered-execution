# Benchmark 02 — a measurement defect, and which result sets it invalidates

Found 2026-10-02 while reviewing the Parquet arms. It is older than those arms and
affects every result set this benchmark has produced before today.

## The defect

The timed query was

```sql
SELECT count(*) FROM ( …GROUP BY… sum(r.amount_dt) … ) s
```

Nothing consumes `total_amt`, so the planner is free to drop the inner aggregate,
and **both engines do**.

PostgreSQL, `EXPLAIN (VERBOSE)` of that exact query:

```
HashAggregate  Output: d.year, a.account_group, r.company_key, NULL::bigint
```

`sum(r.amount_dt)` is replaced by `NULL::bigint`. The grouping happens, the
summation does not.

DuckDB, `EXPLAIN` of the same shape: the plan mentions neither `sum` nor
`amount_dt`, and `HASH_GROUP_BY` carries only the three grouping keys. It reads 3
of 4 projected columns and adds nothing.

The `xpb_*` arms are not affected: `xpb_batch_join2_groupby` is a C function that
always aggregates, whatever the caller does with its rows.

So the comparison was between arms that summed 1 000 008 values and arms that did
not. The `vanilla` and `native_columnar` numbers are understated against every
`xpb_*` number they were printed beside, and the `duckdb_parquet` number was
understated against `xpb_parquet`. Measured after the fix, DuckDB's own cost of
the difference is 40.9 ms without the aggregate against 49.4 ms with it on the
same file — roughly 20%.

The correctness gate was never affected: it checksums `md5(string_agg(… ||
total_amt …))`, which consumes the aggregate by construction. Every published
checksum stands.

## Why it survived three runs

The `EXPLAIN` section printed the plan of the **bare query body**, not of the
query that was timed. The bare body has nothing above the `GROUP BY`, so its plan
computes the sum and looks correct. The wrapper that removed the sum was only
ever in the timing loop, where no plan was printed.

An aggregate that is planned away is invisible in a timing: the numbers are
self-consistent, stable across runs, and wrong.

## What changed

- Every timed query now consumes the aggregate: `SELECT sum(total_amt) FROM (…)`,
  for the SQL arms, the `xpb_*` arms and DuckDB alike.
- `EXPLAIN` prints the plan of the **timed** query.
- The harness asserts the aggregate is in the plan and exits non-zero if it is
  not: `NULL::bigint` in a PostgreSQL plan, or `amount_dt` missing from DuckDB's.
  That assertion is cheap and would have caught this on the first run.
- A second measurement boundary was added. `total_ms` starts after the source is
  constructed and stops before emit, so it was never comparable with DuckDB's
  wall clock around execute+fetch. `xpb_parquet` is now also measured as a whole
  statement, client-side, and that is the only pair compared across engines.

## Which files are what

| file | status |
|---|---|
| `results.csv`, `raw/2026-09-23-*` | **historical, flawed.** `vanilla` and `native_columnar` timed without the aggregate. The `xpb_*` rows are unaffected by this defect but were measured in a different session and cannot be compared with anything here. |
| `results-2026-10-02.csv`, `raw/2026-10-02-seven-paths.txt`, `raw/2026-10-02-runs.csv` | **historical, flawed.** Same defect, and additionally `duckdb_parquet` timed without the aggregate. This is the set whose "39.2 against 43.1" claim does not hold. |
| `results-2026-10-02-corrected.csv`, `raw/2026-10-02-corrected-*` | current. Every arm consumes the aggregate; plan asserted. |

The flawed sets are kept rather than deleted. They are the evidence for what the
defect did, and `PARQUET_ARMS.md` cites them for exactly that.

## Corrected numbers

One session, medians. Two boundaries, not comparable with each other.

Stage decomposition, `total_ms` (source construction and emit excluded), 5 runs:

| arm | total | source | filter | join1 | join2 | agg |
|---|---|---|---|---|---|---|
| xpb_heap | 132.5 | 125.6 | | 1.8 | 2.3 | 3.0 |
| xpb_pgcolumnar | 33.8 | 26.7 | | 1.9 | 2.3 | 2.9 |
| xpb_parquet | 30.0 | 21.4 | 1.3 | 2.0 | 2.4 | 3.0 |
| xpb_zlfs | 7.2 | 0.0 | | 2.0 | 2.4 | 3.0 |

Whole statement, client-side, 15 runs for the cross-engine pair and 5 for the SQL
arms:

| arm | median | spread |
|---|---|---|
| xpb_parquet | 35.0 | 33.5–56.1 |
| duckdb_parquet | 52.7 | 49.1–66.0 |
| native_columnar | 388.2 | |
| vanilla | 702.7 | |

`xpb_zlfs` reads a zone built in setup, before the timer; `source=0.0` is work
moved outside the measurement, not work that does not happen. See
`PARQUET_ARMS.md` for what the Parquet numbers do and do not attribute to the
execution layer.
