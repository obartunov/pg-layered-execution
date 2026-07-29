# Planner v1 roadmap

## Goal

Replace hand-wired SQL functions with planner-chosen paths:

```
Custom Scan (XpBatchAggregate)
  └─ Custom Scan (XpBatchHashJoin)
       └─ Custom Scan (XpBatchHashJoin)
            └─ Custom Scan (XpBatchAppend)
                 ├─ Custom Scan (XpZlfsScan) on cold_partition
                 └─ Custom Scan (XpHeapBatchScan) on hot_partition
```

## Steps

1. **Scan paths** via `set_rel_pathlist_hook`
   - XpZlfsBatchPath for partitions with valid ZLFS zones
   - XpHeapBatchPath as alternative to SeqScan
   - Planner chooses by cost

2. **Partition pruning** — use PostgreSQL's existing mechanism
   - Do not call `find_inheritance_children()` manually
   - Pruned partitions should not appear in the plan

3. **XpBatchAppendPath** — alternative to core AppendPath
   - Only when all children have batch-capable paths
   - Core Append converts to slots; XpBatchAppend preserves columns

4. **XpBatchHashJoinPath** via join path hook
   - Planner chooses join order
   - Outer must have batch representation
   - Inner dimension built as compact hash table

5. **XpBatchAggregatePath** via `create_upper_paths_hook`
   - Input must have batch representation
   - Output: result tuples via standard slots

6. **Compact subtree execution**
   - Internal: `ExecXpBatch(node, batch)` between batch-aware nodes
   - External: top node returns slots to standard executor
   - Only the root of the batch subtree crosses the slot boundary

## Verification

- `WHERE 25..36` must exclude cold partition from plan
- `WHERE 13..24` must exclude hot partition
- `WHERE 13..36` must include both and match hand-wired checksum
- Timing should be comparable to hand-wired pipeline

## Control

Keep current SQL functions as correctness and performance baseline.
