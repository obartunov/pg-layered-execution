# Planner v1 roadmap

## Goal

Replace hand-wired SQL functions with planner-chosen paths while preserving the
working compact batch pipeline:

```text
XpBatchAggregate
  -> XpBatchHashJoin
       -> XpBatchHashJoin
            -> XpBatchAppend
                 -> XpColdScan on cold_partition
                 -> XpHeapBatchScan on hot_partition
```

The planner operates at PostgreSQL-relation granularity. A cold provider may
later prune many internal compact segments behind one `XpColdScan` path.

## Planning boundary

Two planning decisions must remain separate:

```text
PostgreSQL planner
    chooses partitions, providers, join order, and upper paths

Cold provider
    chooses internal segments and estimates base-plus-delta work
```

Do not model each compact segment as a PostgreSQL child partition.

## Steps

### 1. Child scan alternatives

Use `set_rel_pathlist_hook` to add batch-capable paths for surviving child
relations.

For a hot or fallback child:

```text
SeqScanPath
XpHeapBatchPath
```

For a cold child with a valid provider representation:

```text
SeqScanPath / fallback heap path
XpColdPath
```

The first implementation of `XpColdPath` may wrap the existing single-zone
ZLFS source. Its API and costing must allow a later multi-segment provider.

A cold path is offered only when:

- representation metadata is valid;
- schema fingerprint matches;
- required columns are available;
- requested snapshot semantics are supported;
- quals can either be pushed down or evaluated correctly above the source.

### 2. PostgreSQL partition pruning

Use PostgreSQL's existing partition-bound machinery.

- do not call `find_inheritance_children()` from the executor pipeline;
- a statically pruned partition must not appear in the plan;
- preserve runtime pruning opportunities where parameterized predicates allow
  them;
- verify plans for cold-only, hot-only, and mixed ranges.

This is level-one pruning.

### 3. Cold-provider segment pruning

`XpColdPath` passes supported predicates to the cold provider for level-two
pruning.

The provider should estimate and later report:

```text
segments total
segments selected by min/max
segments rejected by Bloom filters
base rows and bytes expected
delta rows expected
residual quals
```

Planner v1 can start with one ZLFS zone and no internal pruning, but the path
contract must not identify one partition with one zone permanently.

### 4. Representation capability on paths

Batch capability must be an explicit property of a path, not inferred from a
node name.

At minimum distinguish:

```text
slot rows
compact owned columns
compact borrowed columns
```

Upper batch paths can be built only when their children provide a compatible
compact representation or an explicit conversion path is costed.

### 5. XpBatchAppendPath

Add an alternative to core `AppendPath` when selected children can provide
compact batches.

- consume the child path list already produced after partition pruning;
- preserve borrowed and owned lifetime rules;
- do not combine batches by copying unless required;
- reset pointers correctly when switching providers;
- expose the same logical row type above all children.

Core `Append` remains the ordinary slot-based alternative.

### 6. XpBatchHashJoinPath

Add batch join alternatives through the join path hook.

- planner chooses join order;
- outer path must provide a compatible compact representation;
- first version supports inner equi-joins against small in-memory dimensions;
- join adds compact payload vectors or selection/reference vectors;
- avoid rematerializing unchanged fact columns;
- retain an ordinary PostgreSQL join path as fallback.

Consolidate the current experimental join variants around the side-vector
pattern used by the partition pipeline.

### 7. XpBatchAggregatePath

Use `create_upper_paths_hook` for grouping and aggregation.

- input must provide compact batches;
- cost source, join, and aggregate phases separately;
- first version supports the aggregate forms already verified by the tests;
- output result rows through standard slots at the batch-subtree boundary.

### 8. Compact subtree execution

Standard `CustomScan` callbacks return one `TupleTableSlot`. The internal batch
subtree therefore needs its own call contract, for example:

```c
bool ExecXpBatch(PlanState *node, XpColumnBatch *batch);
```

Batch-aware nodes call this contract downward. Only the root converts final
result rows to the standard executor interface.

```text
inside subtree: XpColumnBatch
above subtree:  TupleTableSlot result rows
```

## Cost model

The experiments show that source cost and representation boundaries dominate
this workload. Costing should therefore separate:

```text
partition/segment pruning
physical source read and decoding
compact-to-slot or slot-to-compact conversion
join build
join probe
aggregate
```

For a cold path, estimates must eventually include selected segment bytes and
delta merge cost, not only partition row count.

## EXPLAIN

A useful plan should make both layers visible without exposing every segment as
a plan node:

```text
XpBatchAggregate
  XpBatchHashJoin
    XpBatchHashJoin
      XpBatchAppend
        XpColdScan on reg_buh_2024
          Segments: 6 of 128
          Base rows: ...
          Delta rows: ...
        XpHeapBatchScan on reg_buh_2026
```

The single-zone prototype can initially print:

```text
Segments: 1 of 1
Representation: ZLFS v2
```

## Verification

### Partition pruning

- `WHERE 25..36` excludes the cold child from the plan;
- `WHERE 13..24` excludes the hot child;
- `WHERE 13..36` includes both;
- all plans match vanilla checksums.

### Provider choice and fallback

- VALID cold representation allows `XpColdPath`;
- missing, STALE, or schema-mismatched representation selects heap fallback;
- unsupported quals do not produce partial or incorrect cold reads.

### Continuous representation

- `XpCold/Heap -> BatchAppend -> Join -> Join -> Aggregate` contains no hidden
  slot conversion;
- timing remains comparable to the hand-wired control;
- EXPLAIN shows where any explicit conversion is inserted and costed.

### Future cold-provider verification

When multi-segment storage is added:

- selected segment counts match predicate expectations;
- Bloom filters never create false negatives;
- base-plus-delta result matches heap truth;
- compaction generation switch preserves checksums under concurrent readers.

## Control

Keep the current hand-wired SQL functions as correctness and performance
baselines. Planner work should reproduce their result before replacing them.

See [../cold-layer-architecture.md](../cold-layer-architecture.md) for the
storage contract that planner paths must eventually expose.
