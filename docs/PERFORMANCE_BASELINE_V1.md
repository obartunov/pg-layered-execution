# Performance Baseline v1

## Goal

A reproducible cost map of layered execution as it stands, before any planner
admission or cost choice exists. Not an optimization phase: nothing was tuned,
and the three things this run found that look like bottlenecks are recorded as
findings rather than fixed.

The useful output is the **decomposition**, not a ranking. The headline is one
line of it:

> Across all five representations, the operator cost of the same 10 000 000-row
> join+aggregate is **59.5–86.7 ms**. The source cost over the same rows ranges
> from **0.0 ms to 10 495 ms**. Everything that differs between representations
> is the source; the operators barely notice which one it was.

## Base

```
origin/main = main = ed364ff
```

## Environment

```
Linux 6.18.44-fc-v64 x86_64, 2 cpus, 8031 MB RAM   (microVM)
PostgreSQL 20devel, gcc 13.3.0
shared_buffers 1280 MB   work_mem 256 MB   effective_cache_size 5120 MB
jit off   io_method worker   max_parallel_workers_per_gather 2
track_io_timing off
extensions: pgcolumnar 1.0-alpha5, xp_batch 1.0  (xpb_parquet is LOAD-only)
S3 endpoint: RustFS at http://127.0.0.1:9000, loopback
```

`shared_buffers` (1280 MB) exceeds the fact table (575 MB), so a warm run holds
the entire dataset in PostgreSQL's own cache. This matters for reading every
warm number below: **warm here means fully cached, not partially cached**, and
the difference between representations is correspondingly small when warm and
large when cold.

Two cpus, so nothing here says anything about scaling. `jit off` and
`max_parallel_workers_per_gather 2` mean the plain-SQL arms are single-process
hash aggregates.

## Dataset

One logical dataset in five physical representations, verified identical before
anything was timed:

| representation | identity | size |
|---|---|---|
| heap | `reg_buh`, 10 000 000 rows, `sum(amount_dt)=500103444582`, `period_key` 1..120 | 575 MB |
| pgcolumnar | `reg_buh_col`, same rows, same sum | 98 MB |
| Parquet file | `benchmarks/02-batch-joins/data/reg_buh.parquet`, md5 `0d8da64d…` | 180 143 345 B |
| Parquet object | `s3://xpb/reg_buh.parquet`, server ETag `"0d8da64d…"` | same |
| ZLFS zones | three, see below | 1 302 / 15 625 / 156 250 kB |

Dimensions: `dim_period` (120 rows), `dim_account` (200 rows), both 40 kB.

The Parquet md5 and the server's ETag coincide here because the object was
written as a single part. That is a coincidence of how it was uploaded and
**the ETag is still not a content hash** — a 22-part upload of these same bytes
carries `"c7333b8e…-22"`. See `docs/OBJECT_READER_V2_IDENTITY.md`.

Group cardinalities available on this dataset, all from the grouping rather
than from a different table: `period_key` 120, `(period_key, company_key)`
6 000, `(period_key, company_key, account_key)` 24 000.

## Execution paths

All six claimed paths exist at `ed364ff`. Two of them have limits that change
what can be measured, and those are stated here rather than in a footnote.

| path | entry point | source/provider | operators | counters | limits |
|---|---|---|---|---|---|
| heap (oracle) | plain SQL | PostgreSQL | PostgreSQL executor | `\timing`, EXPLAIN | the oracle, not an XPBatch arm |
| heap → XPBatch | `xpb_batch_join2_groupby(lo,hi,'heap')` | `xpb_src_heap.c`, `table_beginscan`/`heap_getnext` | 2 hash joins + hash group-by | stage NOTICE | **scans the whole table whatever the predicate** |
| pgColumnar → XPBatch | …`'pgcolumnar'` | `xpb_src_pgcolumnar.c` over `reg_buh_col` | same | stage NOTICE + `rowgroups`/`copied` | separate table, so the comparison is between storage layers |
| ZLFS → XPBatch | …`'zlfs'` | `xpb_src_zlfs.c`, zero-copy borrow from an in-memory zone | same | stage NOTICE | **zone selected by EXACT range match**; zone load is per backend and uncounted |
| Parquet local → XPBatch | …`'ext:parquet:<path>'` | `xpb_parquet` → `ObjectReader` → `FileReader` | same + a residual range filter | stage NOTICE + `filter`; `xpq_scan` for bytes/requests | row-group-granular pruning, hence the residual filter |
| Parquet remote → XPBatch | …`'ext:parquet:s3://…'` | → `S3Reader` | same | as above + HTTP counters | no TLS; loopback endpoint only |

`xpb_batch_join2_groupby` is the **only** entry point every representation
reaches. `xpb_batch_groupby` dispatches on a mode string of its own and reaches
heap and ZLFS only; `xpb_v2_register_report` needs the benchmark-05 fixtures
(`reg2`, `reg2_card`, `reg2_col`, …), **none of which exist in this database**;
`xpq_scan` is Parquet-only; `xpb_source_conformance` and `xpb_contract_probe`
drive any provider but report correctness, not time.

### ZLFS zone selection is exact-match

Source, four places in `xpb_zlfs.c`: `z->pred_lo == lo && z->pred_hi == hi`.
Never containment. A zone built for `[1..120]` therefore does **not** serve
`[25..25]`, although it holds those rows; the query fails with
*no valid ZLFS zone for [25..25]*. Found by the correctness gate, which refused
to time an arm that returned nothing.

That is the right failure — a silent fall back to the heap would have produced a
"ZLFS" measurement of the heap — but it means **the ZLFS arm exists only for
ranges that were materialized in advance**. Three zones were built for this
run; their build cost is part of the results.

## Workloads

Six shapes were asked for. What exists is a join+aggregate entry point that all
paths reach, plus a scan entry point that only Parquet has, so the shapes map
on as follows. Empty cells are holes in the tree, not measurements that were
skipped.

| shape | heap SQL | heap→XPB | pgcol→XPB | ZLFS→XPB | pq local | pq remote |
|---|---|---|---|---|---|---|
| full scan + join + agg, `[1..120]`, 10 M rows | oracle | ✓ | ✓ | ✓ | ✓ | ✓ |
| selective, `[25..36]`, 1 M rows | oracle | ✓ | ✓ | ✓ | ✓ | ✓ |
| narrow, `[25..25]`, 83 334 rows | oracle | ✓ | ✓ | ✓ | ✓ | ✓ |
| scan, projection, pruning (no join) | SQL | — | — | — | `xpq_scan` | `xpq_scan` |
| group aggregate | ✓ | ✓ (fixed grouping) | SQL only | ✓ (fixed grouping) | — | — |
| high-cardinality group aggregate | ✓ | — | SQL only | — | — | — |

"Full scan" and "pruning" are therefore measured as **ranges within the
join+aggregate shape**, because that is the only shape in which the
representations are comparable. A pure scan shape exists for Parquet alone, and
is reported separately rather than placed in a table beside representations
that have no equivalent.

## Counters

Three things are measured, and they are not interchangeable.

**Wall time** — psql's own `\timing`. This is what a query costs.

**Stage decomposition** — the `NOTICE` that `xpb_batch_join2_groupby` emits:
`total`, `build`, `source`, `join1`, `join2`, `agg`, and `filter` where a
residual filter runs. Each is an `instr_time` interval around the call it names,
so these are counted, not derived.

**Byte and request accounting** — `xpq_scan` only: `read_calls`, `meta_calls`,
`data_calls`, `bytes_requested`, `bytes_returned`, `logical_calls`,
`logical_bytes`, `row_groups_read`/`skipped`, `decoded_values`, and for the
remote arm `s3_get_calls`, `s3_http_attempts`, `s3_bytes_transferred`,
`s3_retries`, `s3_connections`, `s3_reconnects`.

### total_ms is not wall time, and the gap is large

The modules' `total_ms` covers the instrumented region only. It excludes source
**construction** — the ZLFS zone load, the Parquet file open and footer read,
the HTTP connect — and it excludes result materialization.

Measured, not assumed: the first ZLFS query in a fresh backend took **159.9 ms
of wall time while reporting `total=6.9 ms`**. With the library already loaded
by a previous heap query it was still **151.0 ms wall against `total=6.2 ms`**,
so the gap is the zone load and not module loading.

Consequently:

* every wall time here is client-side;
* `wall − total` is labelled **uninstrumented**, never named as the time of a
  layer;
* where that gap matters it is measured directly instead. The ZLFS zone load is
  the difference between the **first and second call in one backend** — two wall
  times, not two counters subtracted.

No layer time anywhere in this document is reconstructed from a difference of
counters.

## Raw artifacts

```
benchmarks/perf_v1/run.sh        the join2 matrix and the Parquet scan shapes
benchmarks/perf_v1/cold.sh       the cold series (restart + drop_caches)
benchmarks/perf_v1/groupagg.sh   shapes 5 and 6
benchmarks/perf_v1/pg-restart-for-cold.sh
benchmarks/perf_v1/raw/<UTC stamp>-<commit>[-cold|-groupagg]/
benchmarks/perf_v1/summary/<UTC stamp>-<commit>[...].csv
```

Each raw file carries its arm, shape, range, state, commit and the command, then
the unedited psql output including every `NOTICE`. Each run gets its own
timestamped directory; nothing is overwritten. `00-environment.txt` records the
host, PostgreSQL settings, extension versions, dataset identity with the Parquet
md5 and the server's ETag, and the zone inventory.

Two full runs of the matrix are kept (`170221Z`, `170734Z`). The warm medians
below pool both, which is why their n is 10.

## Results

Medians. Four states, never averaged together:

* **cold** — postmaster restarted *and* OS page cache dropped, fresh backend;
* **newconn-1st** — warm caches, fresh backend, first call: pays per-backend
  source construction;
* **newconn-2nd** — the second call in that same backend;
* **warm** — same backend after a warm-up, 5 measured runs per series.

### Shape: selective `[25..36]`, 1 000 008 rows, 200 groups

| arm | cold | newconn-1st | newconn-2nd | warm | warm source |
|---|---|---|---|---|---|
| heap→XPB | 722.7 | 209.8 | 133.2 | **126.3** | 118.2 |
| pgcolumnar→XPB | 54.8 | 38.6 | 29.3 | **27.4** | 21.0 |
| ZLFS→XPB | 288.3 | 275.2 | 7.0 | **8.8** | 0.0 |
| Parquet local→XPB | 62.5 | 32.2 | 28.6 | **23.9** | 14.5 |
| Parquet s3→XPB | 1188.7 | 1151.3 | 1174.6 | **1122.3** | 1066.3 |

### Shape: full `[1..120]`, 10 000 000 rows, 2 000 groups

| arm | newconn-1st | newconn-2nd | warm | warm source | warm operators |
|---|---|---|---|---|---|
| heap→XPB | 269.5 | 219.0 | **241.1** | 169.5 | 71.6 |
| pgcolumnar→XPB | 279.6 | 292.7 | **270.9** | 219.9 | 59.5 |
| ZLFS→XPB | 340.2 | 68.9 | **68.1** | 0.0 | 61.5 |
| Parquet local→XPB | 238.7 | 228.5 | **206.0** | 143.2 | 64.4 |
| Parquet s3→XPB | 10 907.5 | 10 622.1 | **10 604.5** | 10 495.7 | 86.7 |

`operators` is `join1 + join2 + agg + filter`, all counted.

### Shape: narrow `[25..25]`, 83 334 rows

| arm | newconn-1st | newconn-2nd | warm |
|---|---|---|---|
| heap→XPB | 174.0 | 122.2 | **118.2** |
| pgcolumnar→XPB | 13.4 | 10.9 | **9.6** |
| ZLFS→XPB | 250.9 | 1.9 | **1.8** |
| Parquet local→XPB | 15.4 | 6.9 | **6.7** |
| Parquet s3→XPB | 150.1 | 137.1 | **141.7** |

### Parquet scan shapes (`xpq_scan`; no equivalent for the other paths)

| arm | shape | rows | physical reads | bytes requested | row groups | warm ms |
|---|---|---|---|---|---|---|
| local | full, 4 cols | 10 000 000 | 402 | 52 795 301 | 200 read | **163.2** |
| local | projection, 1 col | 10 000 000 | 202 | 280 895 | 200 read | **48.7** |
| local | pruned, 4 cols | 1 050 000 | 44 | 5 755 016 | 21 read / 179 skipped | **22.4** |
| local | pruned to nothing | 0 | 2 | 237 693 | 0 read / 200 skipped | **2.7** |
| s3 | full, 4 cols | 10 000 000 | 402 GET | 52 795 301 | 200 read | **10 579.2** |
| s3 | projection, 1 col | 10 000 000 | 202 GET | 280 895 | 200 read | **4 844.3** |
| s3 | pruned, 4 cols | 1 050 000 | 44 GET | 5 755 016 | 21 / 179 | **1 141.0** |
| s3 | pruned to nothing | 0 | 2 GET | 237 693 | 0 / 200 | **48.7** |

Every remote shape ran on **1 TCP connection with 0 retries and 0 reconnects**
(`s3_connections=1`, one GET per logical read plus one HEAD). `rows=1 050 000`
for the pruned shape against 1 000 008 after the join's residual filter is
row-group granularity, not an error.

### Group aggregate, by group cardinality (10 M rows throughout)

| arm | 120 groups | 6 000 groups | 24 000 groups |
|---|---|---|---|
| heap, plain SQL | 412.0 | 578.0 | **1 124.3** |
| pgcolumnar, plain SQL | 597.3 | 743.4 | **1 642.9** |
| heap → `xpb_batch_groupby` | — | 171.8 | — |
| ZLFS → `xpb_batch_groupby` | — | 35.3 | — |

### ZLFS zone build, the cost that buys the query time above

| zone | rows | build | size |
|---|---|---|---|
| `[25..25]` | 83 334 | 435 and 421 ms (built twice) | 1 302 kB |
| `[1..120]` | 10 000 000 | 804 ms | 156 250 kB |

Build cost is **not** proportional to rows: 120× the rows cost 1.9× the time, so
it is dominated by the scan of the source table rather than by the zone written.
Two points only; stated as what was measured, not as a law.

## Repeatability

Five measured runs per warm series after a warm-up, two independent runs of the
whole matrix, medians pooled (n = 10). Spread within a warm series is a few per
cent for every arm except two worth naming:

* **ZLFS warm, full shape**: 58.6–94.2 ms across one series. The arm is so cheap
  (the source contributes 0.0 ms) that the scheduler noise of a 2-cpu box is a
  third of the number.
* **heap warm, full shape**: 224–295 ms.

Cold runs are n = 1 by construction — a second run is not cold.

**The cold numbers are guest-cold, not machine-cold.** `drop_caches` empties the
guest page cache and the postmaster restart empties `shared_buffers`, but this
is a microVM and the host's own cache cannot be dropped from inside it. One
truly cold observation exists, from the first query after the container itself
restarted: the heap arm at `[25..36]` reported `total=22 082.7 ms,
source=22 074.8 ms`, against 722.7 ms for the same arm in the cold series here.
That single number is **30× the guest-cold figure** and it is not repeatable, so
it is recorded as an observation and nothing is built on it. It does bound how
far the guest-cold column understates a genuinely cold read.

## Correctness gates

No timing was recorded for an arm that had not first matched the PostgreSQL
oracle on that shape. The oracle is plain SQL over the heap — two dimension
joins and a group-by, the same semantics the batch pipeline implements — and it
is compared on **group count, aggregate checksum and row count**, the last from
the arm's own NOTICE.

A/B equality between two experimental arms was never used as an oracle.

Results: 15 of 15 join2 cells pass at the three ranges; all 5 group-aggregate gate checks pass
(pgcolumnar through SQL at each of the three cardinalities, and the heap and
ZLFS batch arms at the single cardinality `xpb_batch_groupby` can express); the cold series re-checks the row count
per arm and refuses to record a run that does not match.

The gate earned its place once: the ZLFS arm at `[25..25]` returned no rows at
all, and the gate refused to time it. That is how the exact-match zone
limitation above was found.

## Cost decomposition

### What changes with representation

**The source, and only when the data is not already in RAM.** At `[25..36]`,
cold:

| | cold | warm | cold / warm |
|---|---|---|---|
| heap | 722.7 | 126.3 | 5.7 |
| pgcolumnar | 54.8 | 27.4 | 2.0 |
| Parquet local | 62.5 | 23.9 | 2.6 |
| Parquet s3 | 1188.7 | 1122.3 | 1.06 |

Cold, the heap arm costs **13× pgcolumnar and 11.6× Parquet-local** for the
same answer, because it reads 575 MB where they read 98 MB and 5.8 MB. Warm,
that advantage shrinks to 4.6× and 5.3× — still real, but now it is deform and
tuple-at-a-time overhead rather than I/O.

The remote arm is the exception in both directions: it has essentially **no cold
penalty** (1.06×), because its cost was never in the cache.

### What stays operator-bound

The operators, almost exactly. For the 10 M-row full shape:

```
heap        71.6 ms      source  169.5
pgcolumnar  59.5 ms      source  219.9
ZLFS        61.5 ms      source    0.0
pq local    64.4 ms      source  143.2
pq s3       86.7 ms      source 10495.7
```

**59.5–86.7 ms across a source range of 0 to 10 495 ms.** Two dimension joins
and a hash group-by over 10 M rows cost what they cost; the representation
changes what it costs to *get* the rows, not to process them. The 1.5× spread
that remains tracks how the batch was produced — zero-copy borrow (ZLFS) and
decoded columns (pgcolumnar, Parquet) are a little cheaper to join over than
deformed heap tuples — and the remote arm's extra is its residual range filter
(11.4 ms) plus noise over a 10-second query.

Group cardinality moves cost into the operator and nothing else: 10 M rows
grouped 120 / 6 000 / 24 000 ways cost 412 / 578 / 1 124 ms on the heap through
plain SQL. 200× the groups for 2.7× the time, with the row count fixed.

### What becomes request-bound

The remote Parquet arm, unambiguously:

| shape | GETs | bytes | ms | ms / request | MB / s |
|---|---|---|---|---|---|
| full | 402 | 52 795 301 | 10 579 | 26.3 | 5.0 |
| projection | 202 | 280 895 | 4 844 | 24.0 | 0.06 |
| pruned | 44 | 5 755 016 | 1 141 | 25.9 | 5.0 |
| pruned to nothing | 2 | 237 693 | 48.7 | 24.4 | 4.9 |

**Cost per request is 24–26 ms in every shape while bytes per request vary by a
factor of 100.** The projection shape moves 188× fewer bytes than the full scan
and takes only 2.2× less time, because it still issues 202 requests instead of
402. On this endpoint the request count is what the clock follows.

No coefficient is fitted and no formula is offered; the four points are the
evidence.

### What becomes decode-bound

Parquet local, warm. Source is 143.2 of 206.0 ms for the full shape, with 402
reads of 52.8 MB out of a 180 MB file — all of it from page cache, so what the
143 ms buys is decompression and decoding of 40 000 000 values, not I/O. The
same work cold costs 62.5 ms at `[25..36]`, of which only 35.1 ms is source: the
file is small enough that reading it is cheaper than decoding it.

`xpq_scan` separates the shapes further: 4 columns cost 163.2 ms and 1 column
48.7 ms over the identical 200 row groups and the identical 402-vs-202 read
pattern, which is a decode cost of roughly 38 ms per column over 10 M values.

## Observed bottlenecks

Recorded, not fixed. Phase 7 of the task forbids touching them here, and none of
them was in the way of the measurement.

1. **The heap source scans the whole table whatever the predicate.** Confirmed
   in `xpb_src_heap.c`: the scan is `table_beginscan`/`heap_getnext` and the
   predicate is applied per tuple (`if (key < st->pred_lo || key > st->pred_hi)
   continue`). The measurement agrees — source is 118.2 ms for 1 M emitted rows
   and 169.5 ms for 10 M, so nine million extra rows cost 51 ms while the first
   million cost 118: the fixed part is the scan of all 10 M. **A selective
   predicate buys the heap arm nothing on the read side.**
2. **The ZLFS zone load is invisible and per backend.** ~270 ms for the 156 MB
   zone, ~250 ms for the 1.3 MB one, charged to no counter and paid again by
   every new backend. For the narrow shape that is 250.9 ms of setup for a
   1.8 ms query.
3. **ZLFS zones are selected by exact range match**, so each distinct predicate
   needs its own materialized zone, and a zone that holds the rows will not
   serve a sub-range of itself.
4. **Orphan zone files accumulate.** `/tmp/pgdata20/zlfs` holds 274 files and
   368 MB, of which 3 files and 173 MB belong to live zones: ~195 MB of zone
   files whose source relation was dropped and which nothing reclaims. Every
   `zlfs_zone_info()` call also emits a WARNING per orphan. This is an
   orphan-storage boundary, not a tidiness problem.
5. **pgcolumnar through plain SQL is slower than the heap through plain SQL** at
   every cardinality (597 vs 412, 743 vs 578, 1 643 vs 1 124 ms), despite the
   table being 98 MB against 575 MB. Warm, the access method's per-tuple cost
   exceeds what the smaller read saves. Through XPBatch the same table beats the
   heap arm 27.4 vs 126.3 ms at `[25..36]`, so this is about the row-at-a-time
   executor, not the storage.
6. **The remote arm pays 24–26 ms per request on loopback.** Whatever that is —
   RustFS is a debug build — it is not bytes, and it dominates every remote
   shape.

## What this can support in a future planner

Observed dependencies, at the level they were measured. Not a cost model.

* **heap ~ table size, not selectivity.** The read cost is the full scan; the
  predicate changes only how many rows leave the source. A planner cannot
  assume a narrow range is cheap on this path.
* **pgcolumnar and Parquet-local ~ projected bytes.** Both drop by an order of
  magnitude cold when the range is narrow, and Parquet's read pattern is
  explicit: 402 → 44 → 2 physical reads as pruning tightens.
* **Parquet-remote ~ request count**, with bytes a second-order term over the
  range measured here. 24–26 ms per request across a 100× spread in bytes per
  request.
* **ZLFS ~ a zone existing for exactly this predicate**, plus ~270 ms of
  per-backend load, plus a 421–804 ms build. Query cost after that is near
  zero. This is a materialization trade, and a planner would need the build and
  load costs, not just the query cost.
* **Operators ~ rows and group cardinality, independent of representation.**
  59.5–86.7 ms per 10 M rows for this join+aggregate; group cardinality is the
  other term (120 → 24 000 groups costs 2.7× on the heap through SQL).
* **Projection reduces** physical reads (402 → 202), bytes (52.8 MB → 281 kB)
  and decode (163 → 49 ms), at the same 200 row groups.
* **Pruning reduces** row groups (200 → 21 → 0) and with them reads and bytes,
  but leaves a residual filter because pruning is row-group granular
  (1 050 000 rows read for 1 000 008 wanted).

## What this does not prove

* **Nothing about scale.** Two cpus, 8 GB, one dataset of 10 M rows at 575 MB,
  everything fitting in `shared_buffers` when warm. Every warm number is a
  fully-cached number.
* **Nothing about a real network.** The remote arm talks to a loopback RustFS
  **debug** build with no TLS. Its 24–26 ms per request is a property of this
  endpoint; on a real network latency would add to it and TLS would add a
  handshake. The direction of the request-bound finding is safe; the magnitude
  is not transferable.
* **Nothing about genuinely cold I/O.** The cold column is guest-cold. The one
  machine-cold observation available is 30× larger.
* **Nothing about concurrency.** Every measurement is one query at a time in one
  backend. No arm was measured under contention, and the ZLFS zone's
  per-backend residency means a many-backend workload would multiply its memory
  by the number of backends.
* **Nothing about shapes the tree cannot express.** There is no scan arm for
  heap, pgcolumnar or ZLFS, and no group-aggregate or high-cardinality arm for
  either Parquet path. Those cells are empty above and no substitute was
  measured in their place.
* **Nothing about writes, updates, or freshness.** Every arm reads a static
  dataset. The ZLFS zone and the pgcolumnar table were built once; the cost of
  keeping them current under writes is not in scope and is not measured.
* **`total_ms` is not a query's cost** and no number here treats it as one. The
  uninstrumented region is real and was 153 ms in the worst case found.
* **No optimization was attempted**, and the six findings above are untouched.
  In particular the 24–26 ms per remote request was not investigated, the heap
  scan was not given an index, and no zone, batch size, coalescing or
  connection setting was changed from what `ed364ff` does by default.
