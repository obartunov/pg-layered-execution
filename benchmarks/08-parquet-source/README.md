# Benchmark 08 — Parquet source for XPBatch

Tests one architectural question: can a new physical source sit behind the
existing XPBatch source boundary without Parquet-specific branches in the
generic operators?

## Running it

```sh
python3 gen_data.py data                       # deterministic, ~12 MB
python3 check.py 5420 /tmp/xpb_sock pqtest     # correctness, three arms
```

`data/` is generated and not committed: `gen_data.py` contains no randomness, so
the file is reproducible byte for byte.

## Oracle

PostgreSQL over imported rows, plus pyarrow over the same Parquet file.

**DuckDB was the task's suggested third party and is not available in this
container** — no distribution package, no binary. It was not added merely to
satisfy the original wording. The substitution is two independent
implementations, one of which reads the actual Parquet file, and it is recorded
in the harness output itself rather than only here.

## Dataset shape

8 row groups × 25 000 rows = 200 000 rows, 10 columns, uncompressed, PLAIN
encoded, statistics written.

| column | type | notes |
|---|---|---|
| `order_date_key` | int32 NOT NULL | monotonic across row groups, each covering a disjoint closed range |
| `order_id` | int64 NOT NULL | sequential |
| `customer_id` | int32 NOT NULL | 1000 distinct |
| `amount` | int64 NULL | row group 3 is **entirely NULL**; elsewhere every 97th row |
| `quantity` | int32 NULL | carries INT32_MIN and INT32_MAX |
| `status` | int32 NOT NULL | |
| `pad_a`..`pad_d` | int64 NOT NULL | never projected by any test query; `pad_d` carries INT64_MIN/MAX |

The `pad_*` columns are what give "only projected columns were decoded" any
force: 4 of 10 columns exist solely to be absent from every query.

Types are int32/int64 only because the batch contract's type set is closed and
small (`xpb_colbatch.h`: INT4, INT8, NUMERIC, VARLENA). There is no float and no
date, so the date is carried as an int32 `yyyymmdd` key rather than a Parquet
DATE32 — mapping DATE32 into a batch column would mean inventing a type mapping
before anything needs one.
