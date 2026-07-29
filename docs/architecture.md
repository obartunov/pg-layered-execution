# Architecture

## One logical table, several physical layers

The project explores one PostgreSQL relation whose data can live in different
physical representations over its lifetime:

```text
PostgreSQL parent table
        |
        +-- hot heap partitions
        |
        +-- cold logical partitions
                +-- compact base segments
                +-- mutable delta
                +-- internal CDC
                +-- BRIN/Bloom-like segment metadata
```

PostgreSQL partitioning provides the coarse relational and lifecycle boundary.
The cold provider manages physical segments inside a selected cold partition.
The batch executor preserves a common compact representation above both heap
and cold providers.

The current v0.1 experiments implement a deliberately smaller form:

```text
cold partition -> immutable heap truth + one VALID ZLFS zone
hot partition  -> mutable PostgreSQL heap
```

ZLFS is currently one compact representation provider. It is not intended to
be the whole cold-partition architecture.

See [cold-layer-architecture.md](cold-layer-architecture.md) for the complete
storage, pruning, delta, CDC, and compaction model.

## Four layers

### 1. Logical partition layer

PostgreSQL owns:

- the parent relation and SQL-visible schema;
- coarse range partitioning;
- partition pruning;
- planner-visible child relations;
- the choice among access paths for each surviving child.

A hot partition normally uses heap. A cold partition offers a cold-provider
path and a correctness fallback.

### 2. Physical representation layer

Current providers:

- **ZLFS**: file-backed compact column arrays loaded into backend-local memory;
- **PostgreSQL heap**: standard row storage with MVCC.

Target cold provider:

- multiple immutable compact segments per partition;
- a segment directory with min/max and Bloom metadata;
- transactional delta for late changes;
- internal CDC and generation compaction.

### 3. Provider contract

- **XpBatchSourceOps**: `next_batch()` / `rescan()` / `end()`;
- **XpColumnBatch**: dense column vectors with explicit lifetime;
- **borrowed batches**: source retains ownership, as in the current ZLFS path;
- **owned batches**: source allocates and fills vectors, as in the heap path.

Both hot and cold providers expose the same logical attributes through this
contract. Consumers do not know heap offsets, ZLFS physical column numbers, or
segment layout.

### 4. Execution operators

- **BatchAppend**: combines child providers without slot conversion;
- **BatchHashJoin**: adds compact payload vectors while preserving fact vectors;
- **BatchGroupAgg**: aggregates directly over column vectors.

```text
XpColdBatchSource ----\
                       BatchAppend
XpHeapBatchSource ----/     |
                             v
                       BatchHashJoin
                             |
                             v
                       BatchHashJoin
                             |
                             v
                       BatchGroupAgg
```

Internal boundaries use `XpColumnBatch`. Only the top of an experimental batch
subtree needs to return ordinary result slots to the standard executor.

## Two levels of pruning

```text
SQL predicates
    |
    +-- PostgreSQL partition pruning
    |       removes unrelated hot/cold partitions
    |
    +-- cold-provider segment pruning
            min/max summaries
            Bloom filters
            residual batch filters
```

The current hand-wired partition experiment does not yet implement either
level fully: it discovers all children and a ZLFS source can emit its entire
zone. Planner and cold-provider work must close both gaps independently.

## Current PostgreSQL integration

- **Current**: hand-wired SQL entry points construct sources and operators;
- **planner-v1**: PostgreSQL chooses partitions, access paths, join order, and
  batch-aware upper paths;
- **cold-provider-v1**: one cold partition expands internally into pruned
  segments plus delta, without exposing every segment as a PostgreSQL child.

See [roadmap/planner-v1.md](roadmap/planner-v1.md).

## Attribution

The measured results separate three costs:

1. obtaining an analytical representation from storage;
2. preserving that representation across executor boundaries;
3. performing joins and aggregation once compact vectors are available.

The result is not attributable to `execBatch.h` alone. The fastest ZLFS paths
use compact column batches rather than the slot-batch transport API.
