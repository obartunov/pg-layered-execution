# Benchmark 02 — the two Parquet arms

Not a new benchmark. The same `run.sh`, the same logical query, the same
`[25..36]` slice, the same two dimension joins, the same `GROUP BY`, the same
checksum gate. Two arms were added:

| arm | what it is |
|---|---|
| `xpb_parquet` | the xp_batch pipeline, Parquet source reached through the provider registry |
| `duckdb_parquet` | DuckDB over the **same** `reg_buh.parquet` file, `threads = 1` |

Read `MEASUREMENT_DEFECTS.md` first if comparing against `results.csv` or
`results-2026-10-02.csv`: both are kept, and both timed a query whose aggregate
the planner removed.

## Running it

```sh
python3 export-parquet.py 5420 /tmp/xpb_sock testdb     # writes data/*.parquet
./run.sh 5420 /tmp/xpb_sock testdb
python3 pruning-control.py 5420 /tmp/xpb_sock testdb    # the dataset control
```

Both arms are conditional and skip **visibly** — the Parquet file missing, the
optional module failing to load, and `duckdb` not importable are three separate
messages, because a module that will not load must not be reported as a missing
file. `data/` is generated and not committed.

## What the pipeline needed in order to read Parquet

Nothing in the generic operators. `xpb_batch_join2_groupby` gained one branch
that names no format:

    ext:<provider>:<uri>        resolved through xpb_find_source_provider()

and one thing that is not plumbing: **who applies the predicate.** The heap and
zlfs sources drop non-matching rows themselves; the Parquet provider uses the
range only to exclude whole row groups and returns every row of the row groups it
reads. `XpbSourceProvider.filters_rows` says which, and when it is false the
caller applies the predicate with a generic range-select over the batch contract.

That is visible in the numbers: `filter_in=1050000 filter_out=1000008`. Row-group
granularity is 50 000 rows, the slice boundary falls inside two of them, and
49 992 rows arrive that the predicate then removes, at a cost of 1.3 ms.

## Timings

One session, medians. **Two boundaries, not comparable with each other.**

`total_ms` starts after the source is constructed and stops before emit, so it
excludes the provider's open (footer parse plus 200 row-group statistics) and the
tuplestore. Stage decomposition, 5 runs:

| arm | total | source | filter | join1 | join2 | agg |
|---|---|---|---|---|---|---|
| xpb_heap | 132.5 | 125.6 | | 1.8 | 2.3 | 3.0 |
| xpb_pgcolumnar | 33.8 | 26.7 | | 1.9 | 2.3 | 2.9 |
| **xpb_parquet** | **30.0** | 21.4 | 1.3 | 2.0 | 2.4 | 3.0 |
| xpb_zlfs | 7.2 | 0.0 | | 2.0 | 2.4 | 3.0 |

Whole statement, client-side — the only boundary that can be compared across
engines, because DuckDB's number is wall clock around bind, plan, execute and
fetch:

| arm | median | spread | runs |
|---|---|---|---|
| **xpb_parquet** | **35.0** | 33.5–56.1 | 15 |
| **duckdb_parquet** | **52.7** | 49.1–66.0 | 15 |
| native_columnar | 388.2 | | 5 |
| vanilla | 702.7 | | 5 |

Against DuckDB the defensible statement is: **on the same file, single-threaded,
with both consuming the aggregate, the whole statement is 35.0 ms against 52.7 ms,
and the spreads do not overlap.** It is not a claim about either engine's
execution layer — DuckDB's 52.7 ms includes its own planning, hash builds and
metadata reads of three Parquet files, and this pipeline is a hand-written
function for one query shape, not a planner. An earlier version of this file
claimed 39.2 against 43.1 and "10% faster"; that comparison timed a DuckDB query
whose aggregate had been planned away, and it is withdrawn.

`xpb_zlfs` reads a zone built in `run.sh`'s setup, before the timer. `source=0.0`
is work moved outside the measurement, not work that does not happen.

The stage numbers that matter here are the ones that did **not** move: join1,
join2 and agg are 1.8–2.0, 2.3–2.4 and 2.9–3.0 ms in every arm, Parquet included,
while `source` goes 125.6 → 26.7 → 21.4 → 0.0. Stated carefully, because 5 runs
and a ±0.5 ms per-stage scatter cannot resolve a small difference: these
measurements show no detectable change in operator cost across four sources, and
would not detect one below roughly ±1 ms. That the operators contain no
source-specific branch is a fact about the code, readable in
`xpb_batch_hashjoin.c`; the timings are consistent with it, and the input shape
does change (16 batches for heap and zlfs, 21 for pgcolumnar and Parquet).

## Where the bytes go — four effects, separated

The earlier version of this file reported "5.5 MB against the heap's 602 MB, 109×
fewer bytes for the same answer" under a heading about the Parquet source. Two
thirds of that factor in the logarithm have nothing to do with the execution
layer. The chain, every term from the footer or from a counter:

| what is being read | bytes | factor |
|---|---|---|
| heap, all rows, all columns (`pg_relation_size`) | 602 406 912 | |
| Parquet, all rows, all columns (sum of `total_compressed_size`) | 179 971 176 | **3.35× representation** |
| Parquet, all row groups, the 4 projected columns | 52 557 608 | **3.42× projection** |
| Parquet, the 21 selected row groups, 4 columns (= `data_bytes`) | 5 517 323 | **9.53× pruning** |

3.35 × 3.42 × 9.53 = 109.2. Only the last two are things the source layer does,
and the largest single term is pruning, which is a property of **how the file was
written**.

**Representation** (3.35×) breaks down further, and none of it is execution: the
raw payload is 10M × 8 × 4 = 320 000 000 bytes; the heap stores it in
602 406 912 (tuple headers, alignment, page overhead); Parquet encodes it to
196 295 730 uncompressed and Snappy takes that to 179 971 176. The heap arm also
has no index on `period_key`, so it reads the whole table — this row of the table
is "columnar file versus unindexed sequential scan", which is the comparison
benchmark 02 has always made, stated plainly.

**Pruning**, measured as a control rather than asserted (`pruning-control.py`
writes the same rows in a fixed-seed random order, same row-group size, same
statistics):

| file | row groups read | rows | data bytes | arm total |
|---|---|---|---|---|
| clustered (as generated) | 21 of 200 | 1 050 000 | 5 517 323 | 30.0 ms |
| shuffled, same rows | 200 of 200 | 10 000 000 | 77 785 581 | 242.9 ms |

Both produce `09e54f9a0108ff447c05f2cf63ca63da` at 200 groups, so it is the same
data and the same answer. Pruning is worth 14.1× in bytes and 8.1× in time on
this query — and it exists only because `gen_data.py` emits
`period_key = i / 83334 + 1`, in order. The control's 14.1× is larger than the
9.53× above because shuffling also destroys compressibility (205 MB against
180 MB for the same rows); the 9.53× is the same-file figure and the honest one
for pruning alone.

The clean row-level statement, no compression in it: the heap arm reads
10 000 000 rows to answer a query about 1 000 008 (**10.0×**), the Parquet arm
reads 1 050 000 (**1.05×**), and the difference is pruning granularity — the two
row groups that straddle the slice boundary.

`data_bytes` equalled `attributed_bytes` for this projection, so for these four
columns nothing was read that the footer did not attribute to them. That is not
general; see below.

**pgcolumnar's row** of the old table said "not exposed; 1 600 128 B copied" under
a *bytes read* heading. `copied_bytes` is materialisation of two boundary batches,
not I/O, and that arm's read volume is genuinely not instrumented. It is left out
of the comparison rather than represented by the wrong number.

## Read coalescing is gap-based, and it over-reads

0005 recorded `read_calls = 1 + row_groups_read × projected_columns` as exact, and
this file first restated the correction as "contiguous runs of selected column
chunks". Both are wrong about the mechanism. Measured on `reg_buh.parquet`, whose
column chunks are laid out contiguously (gap 0 between neighbours), with the
predicate pruning to 21 row groups:

| projection | intervening bytes | read calls | over-read |
|---|---|---|---|
| `period_key` | — | 23 | 0 |
| `period_key, company_key` | 0 (adjacent) | 23 | 0 |
| `period_key, account_key` | 2 933 | 23 | 61 593 |
| `period_key, debit_key` | 7 333 | 23 | 153 993 |
| `period_key, credit_key` | 51 808 | 44 | 0 |
| `period_key, amount_dt` | 96 283 | 44 | 0 |

Two ranges separated by a small enough gap are merged into one request **and the
bytes in between are transferred**: 61 593 = 21 × 2 933, exactly the skipped
`company_key` chunk in each of the 21 row groups. The mechanism is Arrow's own
`CacheOptions::hole_size_limit` — "the maximum distance in bytes between two
consecutive ranges; beyond this value, ranges are not combined"
(`arrow/io/caching.h`). On this file the threshold is somewhere in
(7 333, 51 808]; the exact default is in Arrow's source, not measured here.

So:

    read_calls = metadata_calls
               + row_groups_read × (merged ranges, which depend on the gaps)

and **projection pushdown is not byte-exact**: it over-reads by up to the hole
limit per gap per row group. Here that is 61 593 bytes on 5.5 MB, 1.1% — small,
bounded, and not zero. The 0005 statement "no over-read" holds for the
projections measured there, which happened to have large gaps, and not in general.

`metadata_calls` is 2 for this file and 1 for benchmark 08's. It is **derived, not
measured**: the shim counts bytes per phase and ReadAt calls in total, not calls
per phase. 2 is consistent with `meta_bytes` = 65 536 + 172 157, a 64 KiB probe
read followed by a re-read of the real footer.

A terminology error worth naming because it appeared in three places: 237 693 is
not the footer size. The footer of this file is **172 157** bytes
(`serialized_size`); 237 693 is `meta_bytes`, the probe plus the footer. The
earlier text said "its footer is 237 693 bytes".
