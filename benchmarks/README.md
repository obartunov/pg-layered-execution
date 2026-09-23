# Benchmarks

```bash
# once per cluster
benchmarks/common/load.sh    <PGPORT> [PGHOST] [DBNAME]

# per experiment
benchmarks/02-batch-joins/run.sh <PGPORT> [PGHOST] [DBNAME]
```

`load.sh` regenerates the fact table from `common/gen_data.py` (seed 42, 10M
rows) and applies `common/schema.sql`, so the dataset is identical on every
machine. `run.sh` runs the correctness gate first and only then takes timings.

## Dataset `reconstructed-v1`

| table | provenance |
|---|---|
| `reg_buh` | generated, seeded, identical everywhere |
| `dim_period` | **recovered** from an older `bench/00_setup.sql` in a saved snapshot |
| `dim_account` | **reconstructed** to the documented shape; the original is lost |

Checksum `09e54f9a0108ff447c05f2cf63ca63da`, in
`02-batch-joins/expected-checksum.txt`. It is this dataset's checksum, not a
recovered v0.1.0 one — see [HISTORICAL_RESULTS.md](HISTORICAL_RESULTS.md) for
the v0.1.0 record and why its `b90fc574…` stays out of reach.

`dim_period` and `dim_account` are addressed BY ATTNUM in
`xpb_batch_hashjoin.c` and `xpb_batch_partition.c` — attnums 1 and 2 in every
reader — so their first two columns are part of the contract.
`xpb_dim_check_shape()` refuses anything else with an ERROR rather than reading
past `natts`. `dim_period` keeps a third column (`month`) because the recovered
schema has one; nothing reads it.

## Rows are comparable across machines; milliseconds are not

`results.csv` holds one median per path with the dataset in the first column;
the individual warm runs behind each median are in `raw/`. Numbers from
different machines are not to be compared. What carries across is the checksum,
the group count, and the shape of the phase breakdown.

## Experiment 2 — what the five paths measure

Same rows, same query, five whole execution paths:

| storage | native SQL | xp_batch pipeline |
|---|---|---|
| heap | 873.4 ms | 216.2 ms |
| pgColumnar | 568.8 ms | 46.0 ms |
| ZLFS | — | 8.9 ms |

Two readings, and both are about whole paths rather than about a decoder:

* On one pgColumnar storage, the specialised compact pipeline ran this query
  about 12x faster than the stock PostgreSQL plan (46.0 ms against 568.8 ms).
  That is the difference between two execution paths end to end, not the
  isolated speed of the decoder.
* End to end, `xp_batch + pgcolumnar` came out about 5.2x slower than
  `xp_batch + ZLFS` (46.0 ms against 8.9 ms). A source-cost ratio cannot be
  stated: ZLFS's source time is below the resolution of this measurement (it
  reads 0.0 ms, because the zone is already materialized in memory).

The operator costs move hardly at all across sources: `join1 + join2 + agg` is
about 8.5 ms in every pipeline path, whatever fed it.

## Leftover ZLFS zones

A zone file survives `DROP TABLE` of its source. `load.sh` recreates the fact
table, so drop the zones (`SELECT zlfs_drop_zone(lo, hi)`) or clear
`$PGDATA/zlfs/` when reloading, otherwise every later scan pays for the orphan
and warns about it.

## The pgcolumnar arm

`xpb_pgcolumnar` reads a real columnar table through pgcolumnar's fold reader
API. It needs two things a stock setup does not have: pgcolumnar built with
[`patches/pgcolumnar-alpha5/`](../patches/pgcolumnar-alpha5/) applied, and
`shared_preload_libraries = 'pgcolumnar,xp_batch'` in that order. See that
directory's README.

The run records `rowgroups=N copied=M copied_bytes=B`: a row group is served by
pointing at pgcolumnar's decoded stream when every row of it is present,
undeleted, in a decoded vector and inside the predicate range, and is
materialized otherwise. On this dataset 6 of 8 groups are borrowed and the two
boundary groups are copied, so the source time is pgcolumnar's own read and
decode rather than conversion into our batch layout.
