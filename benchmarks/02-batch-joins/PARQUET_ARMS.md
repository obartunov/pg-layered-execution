# Benchmark 02 — the two Parquet arms

Not a new benchmark. The same `run.sh`, the same logical query, the same
`[25..36]` slice, the same two dimension joins, the same `GROUP BY`, the same
checksum gate. Two arms were added:

| arm | what it is |
|---|---|
| `xpb_parquet` | the xp_batch pipeline, Parquet source reached through the provider registry |
| `duckdb_parquet` | DuckDB over the **same** `reg_buh.parquet` file, `threads = 1` |

## Running it

```sh
python3 export-parquet.py 5420 /tmp/xpb_sock testdb     # writes data/*.parquet
./run.sh 5420 /tmp/xpb_sock testdb
```

Both arms are conditional and skip **visibly** — the Parquet file missing, the
optional module failing to load, and `duckdb` not importable are three separate
messages, because a module that will not load must not be reported as a missing
file.

`data/` is generated and not committed. It is a re-encoding of `reg_buh` in its
physical order, not a second dataset.

## What the pipeline needed in order to read Parquet

Nothing in the generic operators. `xpb_batch_join2_groupby` gained one branch
that names no format:

    ext:<provider>:<uri>        resolved through xpb_find_source_provider()

and one thing that is not plumbing: **who applies the predicate.** The heap and
zlfs sources drop non-matching rows themselves; the Parquet provider uses the
range only to exclude whole row groups and returns every row of the row groups it
reads. A caller that assumed the first and got the second would return rows
outside the predicate. `XpbSourceProvider.filters_rows` now says which, `false`
is the zero value and therefore the safe default, and when it is false the caller
applies the predicate with a generic range-select over the batch contract.

That is visible in the numbers: `filter_in=1050000 filter_out=1000008`. Row-group
granularity is 50 000 rows, the slice boundary falls inside two of them, and
49 992 rows arrive that the predicate then removes. The filter costs 1.8 ms.

## Timings — one session, 5 warm runs each, medians

`results-2026-10-02.csv`, raw in `raw/2026-10-02-*`.

| arm | total | source | filter | join1 | join2 | agg |
|---|---|---|---|---|---|---|
| vanilla (PostgreSQL executor) | 892.1 | | | | | |
| native_columnar | 433.7 | | | | | |
| xpb_heap | 144.5 | 136.0 | | 2.2 | 2.9 | 3.1 |
| xpb_pgcolumnar | 58.7 | 46.9 | | 3.4 | 3.4 | 4.4 |
| **xpb_parquet** | **39.2** | 27.8 | 1.8 | 2.5 | 3.0 | 3.7 |
| **duckdb_parquet** | **43.1** | | | | | |
| xpb_zlfs | 8.7 | 0.0 | | 2.3 | 2.9 | 3.6 |

**`results.csv` (2026-09-23) and `results-2026-10-02.csv` must not be compared
with each other.** Cross-session timings on this host drift by up to 1.6× and
the untouched arms show it: `xpb_heap` is 216.2 there and 144.5 here, with no
change to its code path. Within one session the arms are comparable; across
sessions they are not.

The join, aggregate and emit stages are the same 8–9 ms in every xp_batch arm,
Parquet included. That is the result: the operators do not know what the source
was, and their cost does not move when it changes.

`duckdb_parquet` is an engine, not a stage decomposition: its 43.1 ms includes
its own planning, hash builds and aggregation. It is here as a third party on the
same file, and the comparison it licenses is total-to-total.

## Read amplification

Measured with the provider's own counters (`xpq_scan` on the same file and
predicate) and `pg_relation_size`:

| arm | bytes the source read | granularity |
|---|---|---|
| xpb_heap | 602 406 912 (whole table, no index) | 8 192 B page |
| xpb_pgcolumnar | not exposed; 8 row groups touched, 1 600 128 B copied | row group |
| xpb_parquet | **5 517 323** data + 237 693 metadata | 50 000-row row group × column chunk |

The Parquet arm reads 21 of 200 row groups and 4 of 8 columns: 5.5 MB against the
heap's 602 MB, 109× fewer bytes for the same answer. Snappy is part of that, so
the figure is bytes moved and not a decode-work comparison — the row-level
amplification is the clean number, and it is 1 050 000 rows read for 1 000 008
needed, 1.05×, entirely from the two boundary row groups being read whole.

`data_bytes` equalled `attributed_bytes` (what the footer attributes to the
selected chunks) exactly, as in benchmark 08. No over-read.

## One correction this run produced

0005 measured `read_calls = 1 + row_groups_read × projected_columns`. That
formula is **wrong in general** and held in benchmark 08 only because its
projections happened to select non-adjacent columns. Measured here on the same
file, pruned to 21 row groups:

| projection | file column indices | read calls |
|---|---|---|
| `period_key` | 0 | 23 |
| `period_key, company_key` | 0,1 | 23 |
| `period_key, company_key, account_key` | 0,1,2 | 23 |
| `period_key, amount_dt` | 0,5 | 44 |
| `period_key, company_key, account_key, amount_dt` | 0,1,2,5 | 44 |
| `period_key, amount_kt, payload` | 0,6,7 | 44 |

So:

    read_calls = metadata_calls + row_groups_read × (contiguous RUNS of selected
                 column chunks)

Arrow already coalesces adjacent column chunks within a row group into one range
read. `metadata_calls` is 2 here and 1 in benchmark 08, because this file's footer
is 237 693 bytes and does not fit the initial 64 KiB read.

This matters for `docs/PARQUET_OBJECT_STORAGE.md`, where request count is the
whole argument: one layer of coalescing exists already, and a projection's
request count depends on how its columns are laid out in the file.
