# PostgreSQL Layered Execution Lab

**Status:** research prototype
**Base:** PostgreSQL 20devel commit `5713b43`

## Question

Can PostgreSQL preserve a compact physical representation from storage
through scans, joins, and aggregation, including queries that combine
heap and compact partitions?

## Architecture

```
Partitioned PostgreSQL table
        |
        +── cold partition → ZLFS borrowed batches
        |
        +── hot partition  → heap-owned batches
                         |
                    BatchAppend
                         ↓
                    BatchHashJoin
                         ↓
                    BatchHashJoin
                         ↓
                    BatchGroupAgg
```

## Three experiments

| # | Experiment | Key result |
|---|-----------|-----------|
| 1 | Representation contract | Borrowed columns 2.8 ms, slots 8.2 ms, heap 248 ms — aggregate constant |
| 2 | Two hash joins | ZLFS → join → join → agg: 6.2 ms (PG vanilla: 1194 ms) |
| 3 | Partition layers | ZLFS cold + heap hot: 30.5 ms (PG vanilla: 691 ms) |

All paths produce identical results verified by MD5 checksum against vanilla PostgreSQL.

See [docs/experiment-summary.md](docs/experiment-summary.md) for detailed results and methodology.

## What this proves

- Compact provider contract preserves ZLFS performance class
- Compact representation passes through two joins without slot conversion
- Heap and ZLFS batches coexist in the same pipeline via BatchAppend
- Partitions can serve as lifecycle boundary for physical layers
- Slot conversion is measurable but works as a compatibility boundary

## What this does not prove

- Planner integration (pipelines are hand-wired SQL functions)
- Production MVCC for compact zones
- WAL integration or incremental maintenance
- General SQL types beyond fixed-width NOT NULL int32
- Production concurrency or large/spilling joins

## Build

```bash
# 1. Clone and patch PostgreSQL
git clone --depth 1 https://github.com/postgres/postgres.git pg20
cd pg20 && git checkout 5713b43
git apply ../patches/required/0001-executor-batch-api.patch

# 2. Build PostgreSQL
./configure --prefix=$HOME/pginstall --enable-debug --enable-cassert --without-icu
make -j$(nproc) && make install

# 3. Build extension
cd ../extension/xp_batch
make PG_CONFIG=$HOME/pginstall/bin/pg_config
make install PG_CONFIG=$HOME/pginstall/bin/pg_config
```

See [docs/development.md](docs/development.md) for detailed setup.

## Run experiments

```bash
scripts/run-all-experiments.sh
```

Or individually:
```bash
cd benchmarks/01-batch-representation && ./run.sh
cd benchmarks/02-batch-joins && ./run.sh
cd benchmarks/03-partition-layers && ./run.sh
```

## Next step

**planner-v1**: let the PostgreSQL planner choose partitions, providers,
join order, and batch-aware upper paths through standard hooks.
See [docs/roadmap/planner-v1.md](docs/roadmap/planner-v1.md).

## License

[PostgreSQL License](LICENSE)
