# Known limitations

## Data types
- Only fixed-width NOT NULL columns supported
- Benchmark uses int32 exclusively
- No varlena, nullable, or composite types

## ZLFS zones and cold layer
- Current ZLFS is one read-only zone, not yet a segmented cold provider
- No internal segment directory, min/max pruning, or Bloom filters
- No transactional delta, internal CDC, base-plus-delta merge, or compaction
- No WAL integration for cold-layer maintenance
- Backend-local registry, not shared across connections
- Schema hash validation on load, but no runtime monitoring
- Zone files must be rebuilt after DML or the query must fall back to heap

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
