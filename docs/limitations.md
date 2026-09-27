# Known limitations

## Data types
- Fixed-offset heap source needs a fixed-width NOT NULL prefix; the deform,
  projected and early-predicate sources carry varlena, NULL and numeric
- Typed batch contract carries int4, int8, numeric and varlena (05-A onward);
  benchmark 04 and earlier were int32 only
- No composite types; varlena is carried but never aggregated

## ZLFS zones and cold layer
- Current ZLFS is one read-only zone, not yet a segmented cold provider
- No internal segment directory, min/max pruning, or Bloom filters
- No transactional delta, internal CDC, base-plus-delta merge, or compaction
- No WAL integration for cold-layer maintenance
- Backend-local registry, not shared across connections
- Schema hash validation on load, but no runtime monitoring
- Zone files must be rebuilt after DML or the query must fall back to heap

## Execution
- Static-capacity hash tables everywhere except the benchmark report's group
  hash, which grows at load 0.5 (V2_DIM1_CAP=256, V2_DIM2_CAP=1024 are now the
  binding ceiling, at 192 and 768 keys); nothing spills
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
