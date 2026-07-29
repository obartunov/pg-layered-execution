# Known limitations

## Data types
- Only fixed-width NOT NULL columns supported
- Benchmark uses int32 exclusively
- No varlena, nullable, or composite types

## ZLFS zones
- Read-only snapshots, no incremental maintenance
- No WAL integration
- Backend-local registry, not shared across connections
- Schema hash validation on load, but no runtime monitoring
- Zone files must be rebuilt after any DML on the source table

## Execution
- Static-capacity hash tables (GRP_CAP=16384, DIM_CAP=256/512)
- No spill-to-disk for large aggregates or joins
- Small dimension tables only (must fit in memory)
- No parallel execution
- No NUMA awareness

## Planning
- Hand-wired SQL functions, not planner-chosen paths
- No partition pruning (all children scanned)
- ZLFS source emits entire zone without query-level predicate filter
- Join order hardcoded in pipeline functions

## Concurrency
- File-level locking on freshness updates
- No multi-backend zone build coordination
- Zone validity assumes sealed (immutable) partition data
