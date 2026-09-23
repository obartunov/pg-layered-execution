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

`results.csv` holds one median per path with the dataset in the first column.
Numbers from different machines are not to be compared. What carries across is
the checksum, the group count, and the shape of the phase breakdown.

## Leftover ZLFS zones

A zone file survives `DROP TABLE` of its source. `load.sh` recreates the fact
table, so drop the zones (`SELECT zlfs_drop_zone(lo, hi)`) or clear
`$PGDATA/zlfs/` when reloading, otherwise every later scan pays for the orphan
and warns about it.
