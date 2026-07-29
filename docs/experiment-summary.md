---
title: "Compact Batch Execution: Three Experiments"
base: PG 20devel 5713b43
date: 2026-07-28
status: experiments complete, prototype code
---

# What we set out to test

Can a compact columnar representation survive an entire analytical
pipeline — source, join, join, aggregate — without converting back
to PostgreSQL's tuple-at-a-time slot interface, and does it matter
for performance?

# Experiment 1 — batch source contract

**Question.** Can ZLFS compact columns be delivered to an independent
aggregate consumer through a generic provider contract without losing
their performance class?

**Setup.** One table (reg_buh, 10M rows), one query (GROUP BY
period_key, company_key WHERE BETWEEN 25 AND 36), four delivery modes
over the same ZLFS zone data:

| Mode | What it does |
|------|-------------|
| borrowed | Consumer reads ZLFS column pointers directly |
| copied | memcpy columns into consumer-owned buffers |
| slot | Fill TupleTableSlot per row, read back, aggregate |
| heap | Decode heap tuples into column arrays, then aggregate |

**Result** (5 warm runs, median, 1M rows, 600 groups):

| Path | Total | Materialize | Aggregate |
|------|-------|-------------|-----------|
| Borrowed columns | 2.8 ms | <0.1 ms | 2.8 ms |
| Copied columns | 4.2 ms | 1.1 ms | 3.1 ms |
| TupleTableSlot | 8.2 ms | 5.6 ms | 2.4 ms |
| Heap decode | 248 ms | 246 ms | 1.9 ms |
| PG vanilla | 969 ms | — | — |

**What this shows.** Aggregate cost is constant (~2–3 ms) regardless
of source. The difference is materialization: borrowed = free, memcpy
= cheap, slot = 3× overhead, heap decode = dominant cost. The generic
XpBatchSourceOps contract preserves ZLFS performance class.

Checksum: `171653effb5086ed160c67ff2ce1588d` (all paths match vanilla).

# Experiment 2 — batch hash join

**Question.** Does the compact batch survive a hash join, or does the
join boundary force conversion to rows?

**Setup.** Same fact table, two dimension tables (dim_period: 120 rows,
dim_account: 200 rows). Pipeline:

    Source → BatchHashJoin(dim_period) → BatchHashJoin(dim_account) → Agg

GROUP BY year, account_group, company_key. 200 result groups.

**Result** (5 warm runs, median, 1M rows):

| Path | Total | Source | Join1 | Join2 | Agg |
|------|-------|--------|-------|-------|-----|
| ZLFS → 2J → Agg | 6.2 ms | <0.1 ms | 1.8 ms | 1.5 ms | 2.9 ms |
| Heap → 2J → Agg | 282 ms | 275 ms | 1.9 ms | 1.5 ms | 3.0 ms |
| PG vanilla | 1194 ms | — | — | — | — |

**What this shows.** Operator costs are additive: join1 + join2 + agg
≈ 6.2 ms. No hidden materialization between operators. The batch
stayed compact through both joins. Source cost determines the total:
ZLFS <0.1 ms vs heap 275 ms.

Checksum: `b90fc574dab20f03c7a295f8e74fcb83` (ZLFS = heap = vanilla).

# Experiment 3 — partition-based layered execution

**Question.** Can ZLFS and heap batches coexist in the same pipeline
when the data comes from different partitions of the same table?

**Setup.** Partitioned table reg_buh_layered:
- reg_buh_cold (periods 13–24): ZLFS zone, borrowed columns
- reg_buh_hot (periods 25–36): heap scan, owned columns

BatchAppendSource switches between child sources. Same two-join
pipeline as experiment 2. 2M total rows, 400 result groups.

**Result** (5 warm runs, median):

| Path | Total | Source | Join1 | Join2 | Agg |
|------|-------|--------|-------|-------|-----|
| ZLFS(cold) + heap(hot) | 30.5 ms | 14 ms | 5.7 ms | 4.6 ms | 5.4 ms |
| Both heap (fallback) | 44.5 ms | 28 ms | 5.9 ms | 4.5 ms | 5.3 ms |
| PG vanilla | 691 ms | — | — | — | — |

**What this shows.** Two different physical representations flow
through the same pipeline without conversion. BatchAppendSource
switches from ZLFS borrowed pointers to heap owned buffers with a
pointer reset — no slot conversion, no data copy at the boundary.

The lower heap-source time compared to experiments 1–2 is expected:
experiments 1 and 2 scan the original 10-million-row heap to select
one million rows, while each partition in experiment 3 physically
contains only one million rows. Partitioning itself already brings
the heap closer to the relevant data range, but compact representation
still halves the source cost: 28 ms → 14 ms.

When ZLFS is invalidated, the system falls back to heap automatically.
Correctness is preserved in all modes.

Checksum: `cb03f8722641a06bb8bc2284a39ea7ee` (mixed = heap = vanilla).

# What the three experiments prove together

1. **The physical access path dominates the cost.** In every experiment,
   aggregate and join costs are small and constant (~2–6 ms per
   operator). The difference between a fast query and a slow query
   is how the data reaches the first operator. This includes
   representation format, tuple decoding, visibility processing,
   and the ability to avoid irrelevant rows.

2. **Compact batch survives the pipeline.** ZLFS columns passed
   through BatchAppend, two hash joins, and a group aggregate without
   converting to slots or copying data. The same compact batch contract
   is preserved from storage to the final operator: fact columns remain
   in compact vectors, while joins add compact payload vectors without
   rematerializing the rows as slots.

3. **The slot boundary is measurable.** Experiment 1 showed that
   converting the same ZLFS data to TupleTableSlots costs 3× more
   than keeping it as compact columns. This cost would be paid
   wherever a compact pipeline crosses a slot-materialization boundary.

4. **Mixed representations work.** Different partitions can use
   different physical formats (ZLFS columnar, heap row-store) and
   feed the same pipeline through a generic provider contract.

The experiments separate three costs that are often mixed together:

1. Obtaining an analytical representation from storage.
2. Preserving that representation across executor boundaries.
3. Performing the analytical operators themselves.

On this workload, the operators are cheap once the data reaches them
in compact form. Most of the cost is paid before or at representation
boundaries.

# What the experiments do not prove

- **Planner integration.** All pipelines are hand-wired SQL functions,
  not planner-chosen plans. The planner does not know about ZLFS zones,
  BatchHashJoin, or BatchAppend.

- **Write path.** ZLFS zones are read-only snapshots built from heap
  data. There is no incremental maintenance, no WAL integration, no
  MVCC visibility for zone contents beyond build-time snapshot.

- **General schema.** ZLFS v0.2 supports any fixed-width NOT NULL
  columns, but the benchmark uses only int32. Variable-length and
  nullable columns are not supported.

- **Concurrent access.** Zone freshness uses file-level locking.
  Multi-backend concurrent read/write is not tested beyond basic
  file persistence.

- **Large dimension joins.** Dimension tables are small (120, 200
  rows) and fit in L1 cache. The hash join implementation uses
  static-capacity open-addressing tables, not the general-purpose
  resizable hash tables that a production join would need.

# Architecture summary

```
XpBatchSourceOps (generic provider contract)
├── ZlfsBatchSource     zero-copy borrowed column pointers
├── HeapBatchSource     decode tuples into column arrays
└── BatchAppendSource   switches between child sources

    ↓ XpColumnBatch (dense int32 column arrays)

BatchHashJoin           probe batch against dimension hash table
                        output: batch with dimension column appended

    ↓ XpColumnBatch

BatchGroupAgg           hash aggregate over column arrays
                        output: result tuples
```

ZLFS v0.2:
- Generalized: any table, any fixed-width NOT NULL columns
- API: `zlfs_build_zone('table', 'attno1,attno2,...', lo, hi)`
- Schema hash (FNV-1a) stored in zone file, validated on load
- Alignment-aware offset computation via att_align_nominal
- File-level locking on freshness updates, fsync on writes

# Identifiers

| Item | Value |
|------|-------|
| PG base | 20devel commit 5713b43 |
| Experiment 1 checksum | 171653effb5086ed160c67ff2ce1588d |
| Experiment 2 checksum | b90fc574dab20f03c7a295f8e74fcb83 |
| Experiment 3 checksum | cb03f8722641a06bb8bc2284a39ea7ee |
| ZLFS schema hash | 481be3ef |
| Data generator | gen_data.py seed=42 |
