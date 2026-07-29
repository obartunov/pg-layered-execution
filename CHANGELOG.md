# Changelog

## v0.1.0 — Three compact batch experiments (2026-07-28)

Initial release of three reproducible experiments on PostgreSQL 20devel.

### Experiment 1: Batch representation contract
- XpColumnBatch + XpBatchSourceOps provider contract
- ZlfsBatchSource (zero-copy borrowed), HeapBatchSource (tuple decode)
- Slot and copy negative controls
- Checksum: `171653effb5086ed160c67ff2ce1588d`

### Experiment 2: Two batch hash joins
- BatchHashJoin with dimension tables
- Pipeline: source → join → join → aggregate
- Compact batch preserved through both joins
- Checksum: `b90fc574dab20f03c7a295f8e74fcb83`

### Experiment 3: Partition-based layered execution
- Partitioned table with cold (ZLFS) and hot (heap) partitions
- BatchAppendSource combines different physical representations
- Automatic heap fallback on zone invalidation
- Checksum: `cb03f8722641a06bb8bc2284a39ea7ee`

### ZLFS v0.2
- Generalized: any table, any fixed-width NOT NULL columns
- Attno-based provider interface
- Schema hash validation on load
- Alignment-aware offset computation
- fsync on writes, file locking on freshness updates

### Infrastructure
- PG 20devel port (3 fixes in xpb_groupagg2.c)
- Reproducible benchmark harness
- Five-way correctness verification
