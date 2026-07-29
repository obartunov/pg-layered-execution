# Cold-layer architecture

## Why a cold partition is not one ZLFS file

The current experiments deliberately use a simple model:

```text
cold PostgreSQL partition
    + immutable heap truth
    + one VALID ZLFS zone
```

This is sufficient to test the execution question: can a compact physical
representation pass through `BatchAppend`, two joins, and aggregation without
being converted back to rows?

It is not the intended storage architecture.

A real cold partition can be large, can receive late corrections, and must
support both analytical scans and historical OLTP lookups. Treating the whole
partition as one immutable columnar file would make pruning too coarse and
updates too expensive.

The intended architecture has two levels of data organization:

```text
PostgreSQL parent table
        |
        +-- hot heap partitions
        |      normal PostgreSQL MVCC and DML
        |
        +-- cold logical partitions
               |
               +-- immutable compact base segments
               +-- mutable transactional delta
               +-- internal change stream
               +-- segment directory: min/max + Bloom metadata
               +-- background compaction and generation switch
```

PostgreSQL partitions remain the visible relational structure. The internal
segments of a cold partition are provider-managed physical objects, not more
SQL-visible partitions.

## Responsibilities of each layer

### PostgreSQL partitioning

PostgreSQL owns the coarse lifecycle and planning boundary:

- one parent relation remains the SQL-visible table;
- partition bounds describe large ranges, usually time or another stable key;
- ordinary partition pruning removes unrelated hot and cold partitions;
- each surviving child can offer heap, cold, or fallback access paths;
- permissions, row types, joins, grouping, and the rest of SQL remain attached
  to the logical relation.

A partition answers the question:

> Which large ranges of the logical table can contain rows for this query?

It should not be used to represent every compact storage segment.

### Cold provider

The cold provider owns the physical organization inside one selected cold
partition:

- immutable base generations;
- compact segments within a generation;
- segment metadata and pruning;
- the mutable delta for late changes;
- visibility of corrections and tombstones;
- compaction into a new generation;
- production of compact batches for the executor.

A cold provider answers the question:

> Which physical segments and delta records can contribute rows, and how can
> they be delivered without reconstructing heap tuples?

### Batch executor

The batch executor is independent of the storage lifecycle. It receives a
common compact contract from either provider:

```text
hot heap partition
    -> HeapBatchSource -> owned compact batch

cold partition
    -> ColdBatchSource -> borrowed or owned compact batch
```

Both paths can feed:

```text
BatchAppend
    -> BatchFilter
    -> BatchHashJoin
    -> BatchHashJoin
    -> BatchGroupAgg
```

The provider may change. The executor contract does not.

## Internal structure of a cold partition

A cold partition contains one active base generation and a mutable delta:

```text
cold partition
    |
    +-- generation N
    |     +-- segment 0001
    |     +-- segment 0002
    |     +-- ...
    |     +-- segment 00NN
    |
    +-- delta heap
    |
    +-- change metadata / CDC position
```

### Base generation

A base generation is immutable after publication. It is made of independently
prunable compact segments.

A segment contains:

- a bounded number of rows;
- compact column vectors for a selected schema projection;
- row identity needed to reconcile base rows with updates and deletes;
- schema and format identifiers;
- statistics used for pruning;
- checksums and generation metadata.

ZLFS v0.2 should be understood as the current prototype of this compact
segment format. The current implementation stores one zone for the whole test
range; the architecture must not freeze that one-zone assumption.

### Segment directory

Each segment has a small metadata entry that can be read without loading the
segment columns.

The directory should support at least:

```text
segment id
row count
byte size
min/max for ordered or range-filtered columns
null presence where nullable types are added
Bloom filters for selected equality keys
physical column locations
base generation id
```

The directory is BRIN-like in spirit: one summary record describes a large
physical range. It is not required to use the existing PostgreSQL BRIN on-disk
format.

Bloom filters complement min/max summaries for historical point and equality
lookups. They answer only "definitely absent" or "possibly present"; residual
checks still happen after reading candidate segments.

## Two levels of pruning

A layered query should avoid work twice.

### Level 1: PostgreSQL partition pruning

Given:

```sql
WHERE period_key BETWEEN 25 AND 36
```

PostgreSQL removes partitions whose bounds cannot match. A pruned child must
not appear in the batch plan.

### Level 2: cold-segment pruning

For each selected cold partition, the cold path passes supported predicates to
the provider. The provider consults the segment directory:

```text
partition selected by PostgreSQL
        |
        +-- min/max pruning on period_key
        +-- min/max pruning on account_key where useful
        +-- Bloom pruning on company_key/account_key
        +-- residual predicate evaluation on candidate batches
```

Example:

```sql
WHERE period_key BETWEEN 25 AND 27
  AND company_key = 42
  AND account_key = 137
```

The desired path is:

```text
partition pruning
    -> segment min/max pruning
    -> Bloom rejection
    -> read a small candidate set
    -> exact predicate check
```

The current prototype performs neither true partition-bound pruning in its
hand-wired function nor query-level pruning inside ZLFS. Those are known
experimental limitations, not the target architecture.

## Historical OLTP

Cold data is not used only by full analytical scans. Applications also issue
selective historical queries:

```sql
SELECT ...
FROM reg_buh
WHERE period_key = 18
  AND company_key = 42
  AND account_key = 137;
```

A sequential scan of an entire cold partition is unacceptable for this class
of workload.

The historical OLTP path uses the same hierarchy:

```text
PostgreSQL partition pruning
    -> BRIN-like segment directory
    -> Bloom filters for equality keys
    -> candidate compact segments
    -> exact row lookup / residual predicate
    -> merge with delta
```

This intermediate metadata layer is important. The design is not a choice
between a heap index lookup and a full columnar scan. A cold provider needs a
selective path that can locate a few historical rows without reconstructing or
scanning the full partition.

The first prototype may implement only segment rejection. Later versions can
add per-segment row indexes for the key combinations that historical OLTP
actually uses.

## Mutable delta and internal CDC

A partition becomes cold because most of its data is stable, not because
historical data can never change.

Late activity includes:

- delayed inserts;
- corrections to old values;
- cancellations or deletes;
- source-system reconciliation;
- schema-compatible enrichment.

Rewriting a compact segment for every such change would destroy the purpose of
the cold layer. Changes therefore enter a small transactional delta:

```text
INSERT / UPDATE / DELETE against cold range
        |
        +-- PostgreSQL transactional delta heap
        +-- internal change record
```

"Internal CDC" here means a change stream maintained inside the PostgreSQL
storage lifecycle. It is not an external logical-replication product. Its jobs
are to:

- identify which base generation and row identity a change affects;
- preserve operation order and transaction visibility;
- expose inserts, replacement values, and tombstones to cold scans;
- provide the compactor with a bounded sequence of changes to merge;
- record the point up to which a new generation includes the delta.

The exact implementation is deliberately left open. Candidate sources include
WAL-derived records, a provider-owned change queue, or transactional delta
metadata. The correctness contract matters more than the first mechanism.

## Querying base plus delta

A cold scan is logically a merge of two representations:

```text
immutable compact base
        +
transactional delta
        =
visible rows of the cold partition
```

The scan must account for:

- new rows present only in delta;
- updated base rows whose replacement is in delta;
- deleted base rows represented by tombstones;
- multiple changes to the same row;
- the query snapshot.

For batch execution, the preferred shape is:

```text
pruned base segments -> compact batches
pruned delta heap    -> compact batches
                         |
                    ColdMerge
                         |
                  compact output batches
```

`ColdMerge` should preserve vectors where possible. It must not turn the whole
cold partition back into `TupleTableSlot` rows merely to apply a small delta.

The current repository does not implement this merge. In v0.1, a ZLFS zone is
valid only while the source partition is treated as immutable, and invalidation
causes heap fallback.

## Compaction and generations

When delta reaches a size, age, or read-amplification threshold, the provider
builds a new immutable generation:

```text
generation N base
    + delta through CDC position L
        |
        +-- build generation N+1 segments
        +-- build new segment summaries and Bloom filters
        +-- verify row counts and checksums
        +-- publish generation N+1 atomically
        +-- retain N while old readers can still reference it
        +-- reclaim N and consumed delta later
```

Generation publication should be metadata-atomic. Readers see either a complete
old generation plus its delta or a complete new generation plus the remaining
delta; never a partially built base.

This gives the cold layer an LSM-like maintenance cycle without exposing LSM
semantics to SQL:

```text
immutable base + mutable delta + merge
```

It is still a PostgreSQL relation from the user's point of view.

## Planner contract

The PostgreSQL planner should not plan every compact segment as a child
relation. It should plan one cold path for one PostgreSQL partition and ask the
provider for estimates.

A cold path should report:

```text
segments total
segments expected after pruning
base rows expected
base bytes expected
delta rows expected
ordered properties, if any
batch capability
supported quals
```

The resulting plan may look like:

```text
XpBatchAggregate
  -> XpBatchHashJoin
       -> XpBatchHashJoin
            -> XpBatchAppend
                 -> XpColdScan on reg_buh_2024
                      Segments: 6 of 128
                      Base rows: ...
                      Delta rows: ...
                 -> XpHeapBatchScan on reg_buh_2026
```

The boundary is intentional:

- PostgreSQL chooses partitions, join order, and upper paths;
- the cold provider chooses internal segments and reconciles base with delta;
- the batch executor preserves the selected representation through operators.

## Lifecycle

At the PostgreSQL partition level:

```text
OPEN
  mutable heap partition
    |
    +-- seal candidate
    v
COOLING
  build and verify first compact generation
    |
    v
COLD
  compact base + transactional delta
    |
    +-- delta/maintenance thresholds
    v
COMPACTING
  old generation remains readable while a new one is built
    |
    v
COLD
```

A representation can also enter a degraded state:

```text
COLD -> STALE / INVALID -> heap fallback or rebuild
```

The current v0.1 experiment implements only a smaller subset:

```text
OPEN heap
    -> build one ZLFS zone
VALID immutable zone
    -> invalidate on DML/schema change
STALE
    -> heap fallback or rebuild
```

## Failure and fallback

The compact layer is an optimization only when its correctness contract is
satisfied.

The provider must reject or fall back when:

- schema fingerprint does not match;
- segment or generation metadata is incomplete;
- checksums fail;
- the required snapshot cannot be reconstructed from base plus delta;
- the internal CDC position is inconsistent;
- a requested column or predicate is unsupported.

The v0.1 fallback is the source heap. A later cold provider may use a verified
previous generation plus delta as its fallback. Silent partial reads are never
acceptable.

## What the current experiments establish

The repository already demonstrates the execution half of this architecture:

1. compact columns can be exposed through a generic provider contract;
2. the compact contract survives two joins and aggregation;
3. heap-owned and ZLFS-borrowed batches coexist behind `BatchAppend`;
4. a PostgreSQL partition can act as the coarse boundary between providers.

It does not yet implement:

- multiple segments per cold partition;
- segment directory or BRIN/Bloom pruning;
- transactional delta;
- internal CDC;
- base-plus-delta visibility;
- background compaction;
- planner-selected cold paths.

These are the next storage and planning stages. They should extend the current
provider contract rather than replace the working batch pipeline.
