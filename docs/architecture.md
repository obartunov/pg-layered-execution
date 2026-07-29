# Architecture

## Four layers

### Physical representation
- **ZLFS**: file-backed compact columnar zones (int32 column arrays)
- **PostgreSQL heap**: standard row store with MVCC

ZLFS is a representation provider. It is not a replacement for
heap — it is a read-only analytical snapshot built from heap data.

### Provider contract
- **XpBatchSourceOps**: `next_batch()` / `rescan()` / `end()`
- **XpColumnBatch**: dense int32 column arrays with ownership flag
- **Borrowed batches**: ZLFS points directly into zone memory (zero copy)
- **Owned batches**: heap source allocates and fills column arrays

### Execution operators
- **BatchAppend**: combines multiple child sources sequentially
- **BatchHashJoin**: probe batch against small dimension hash table
- **BatchGroupAgg**: hash aggregate over column arrays

All operators consume and produce XpColumnBatch. No slot conversion
between operators.

### PostgreSQL integration
- **Current**: hand-wired SQL functions for routing and pipeline
- **Next (planner-v1)**: planner hooks for scan, join, and aggregate paths

## Attribution

The measured speedup comes from:
1. ZLFS compact physical representation
2. XpGroupAgg2 / BatchGroupAgg fused analytical operators
3. Avoiding tuple-at-a-time slot materialization between operators

It is not attributable to `execBatch.h` alone — the ZLFS path does
not use the slot-batch transport API.
