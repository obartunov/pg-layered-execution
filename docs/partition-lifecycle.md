# Partition lifecycle

## States

```
OPEN heap partition
    ↓ seal (build zone)
VALID compact representation + immutable heap truth
    ↓ invalidation, schema change, or DML
STALE
    ↓ fallback to heap or rebuild zone
```

## Current model

- Hot partition: mutable heap, no ZLFS zone
- Cold partition: immutable heap + VALID ZLFS zone
- Zone is a read-only snapshot built from heap at a point in time
- Heap remains the source of truth in all states

## Transitions

| From | To | Trigger |
|------|----|---------|
| OPEN | VALID | `zlfs_build_zone()` |
| VALID | STALE | `zlfs_invalidate_zone()`, schema change, DML |
| STALE | VALID | `zlfs_build_zone()` (rebuild) |
| STALE | OPEN | `zlfs_drop_zone()` |

## Guarantees

- STALE zone is never read for query results
- Heap fallback is automatic and transparent
- Zone file is atomically replaced (tmp + rename)
