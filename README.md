# PostgreSQL Layered Execution Lab

**Status:** research prototype
**Base:** PostgreSQL 20devel commit `5713b43`

## Question

Can PostgreSQL preserve a compact physical representation from storage
through scans, joins, and aggregation, including queries that combine
heap and compact partitions?

## Architecture

```
PostgreSQL parent table
        |
        +── hot heap partitions
        |
        +── cold logical partitions
        |       ├── compact base segments
        |       ├── mutable delta + internal CDC
        |       └── min/max and Bloom pruning
        |
        └── selected providers
                 ↓
             BatchAppend
                 ↓
             BatchHashJoin
                 ↓
             BatchHashJoin
                 ↓
             BatchGroupAgg
```

The current experiments use one ZLFS zone as the first compact-segment
prototype. The intended cold layer is broader: PostgreSQL prunes partitions,
while the cold provider prunes internal segments and later merges an immutable
base with transactional delta.

See [docs/architecture.md](docs/architecture.md) and
[docs/cold-layer-architecture.md](docs/cold-layer-architecture.md).

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

## Next steps

- **planner-v1**: let PostgreSQL choose partitions, providers, join order, and
  batch-aware upper paths through standard hooks;
- **cold-provider-v1**: evolve the current single ZLFS zone into prunable
  compact segments with a transactional delta, internal CDC, and compaction.

See [docs/roadmap/planner-v1.md](docs/roadmap/planner-v1.md) and
[docs/cold-layer-architecture.md](docs/cold-layer-architecture.md).

## License

[PostgreSQL License](LICENSE)
