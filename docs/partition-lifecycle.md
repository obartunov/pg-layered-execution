# Partition lifecycle

## Coarse lifecycle: PostgreSQL partitions

A PostgreSQL partition is the large lifecycle boundary. It starts as ordinary
mutable heap and can later acquire a cold-provider representation.

```text
OPEN
  mutable heap partition
    |
    +-- range closes / seal requested
    v
COOLING
  build and verify compact base
    |
    v
COLD
  compact base + controlled late-change path
```

The SQL-visible object remains a partition of the same parent relation.
Queries continue to address the parent table.

## Current v0.1 lifecycle

The experiments implement a smaller, conservative state machine:

```text
OPEN heap partition
    |
    +-- zlfs_build_zone()
    v
VALID
  immutable heap truth + one ZLFS zone
    |
    +-- DML, schema change, explicit invalidation
    v
STALE
    |
    +-- heap fallback or rebuild
```

Current rules:

- hot partition: mutable heap, no ZLFS zone;
- cold experimental partition: heap remains the source of truth;
- a VALID zone is a read-only snapshot built from heap;
- STALE zones are never used for query results;
- fallback reads heap through the same compact batch contract;
- zone replacement uses a temporary file and rename.

| From | To | Trigger |
|------|----|---------|
| OPEN | VALID | `zlfs_build_zone()` |
| VALID | STALE | `zlfs_invalidate_zone()`, schema change, DML |
| STALE | VALID | `zlfs_build_zone()` rebuild |
| STALE | OPEN | `zlfs_drop_zone()` |

## Target cold lifecycle

A real cold partition is not frozen forever. It contains an immutable base
generation and a small transactional delta:

```text
COLD generation N
    +-- immutable compact segments
    +-- mutable delta
    +-- internal CDC position
```

Late `INSERT`, `UPDATE`, and `DELETE` operations enter delta rather than
rewriting base segments synchronously.

When maintenance thresholds are reached:

```text
COLD generation N
    |
    +-- base N + delta through position L
    v
COMPACTING
  build and verify generation N+1
    |
    +-- atomic generation publication
    v
COLD generation N+1
  remaining delta starts after L
```

Old generations remain readable until no active reader can reference them.
Reclamation happens after publication, not in the critical switch.

## Lifecycle invariants

- PostgreSQL partition bounds remain true for every physical generation.
- A published compact generation is immutable.
- A query sees a coherent base-plus-delta view for its snapshot.
- Compaction never exposes a partially built generation.
- Schema or metadata mismatch causes rejection or fallback, never silent reads.
- A cold representation is an optimization only while its correctness contract
  is valid.

## Relationship to pruning

Lifecycle and pruning use different granularities:

```text
partition lifecycle and PostgreSQL pruning
        large time/range boundaries

segment lifecycle and cold-provider pruning
        compact physical ranges inside one partition
```

Creating a PostgreSQL partition per compact segment would expose maintenance
objects to SQL and overload the planner with a storage detail. Segments remain
inside the cold provider.

See [cold-layer-architecture.md](cold-layer-architecture.md) for delta, CDC,
segment metadata, historical OLTP, and compaction details.
