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
- Static-capacity hash tables everywhere except three, all in
  `xpb_v2_report.c`: the group hash and both dimension hashes grow at load 0.5
  by doubling. `V2_GRP_CAP`, `V2_DIM1_CAP` and `V2_DIM2_CAP` are initial
  capacities, not ceilings; the remaining ceiling is `MaxAllocSize`. Every
  other table is still fixed, most of them erroring at 3/4 load before the
  insert -- including the dimension hashes in `xpb_batch_hashjoin.c`,
  `xpb_batch_partition.c` and `xpb_typed_pipeline.c`. Nothing spills anywhere.
  See `docs/TYPED_BATCH_CONTRACT.md` for the per-table list; three rows there
  do not follow the 3/4 rule and are called out separately
- `xpb_groupagg2.c`'s global group hash takes its capacity from the planner
  (`custom_private[6]`, default 16384), not a `#define`, and has no pre-insert
  guard; a single post-scan check errors above 0.95 load. Sound against row loss
  **for that table** -- a full table reports load 1.0 -- but coarser than every
  other table, and the only one reachable through a planner hook
- The same file's per-batch local hash (`local_ht`) is NOT covered by that
  check. With `xp_batch.groupagg2_local_partial` on (a `PGC_USERSET` boolean,
  default off), it is a fixed `xp_batch.groupagg2_local_hash_cap` table
  (default 2048) with no guard, `memset` per batch, so the threshold is 2048
  distinct keys within a single batch of up to 65 536 rows. A row dropped there
  never reaches the global hash, so the post-scan check cannot see it -- a
  quietly short sum, reachable by setting one GUC. Fourth site in
  `docs/roadmap/fixed-hash-silent-drop.md`
- Two fixed tables have no capacity guard at all and silently drop rows when
  full (`xpb_projection.c` `AGG_CAP`, `xpb_columnar_pipeline.c` `WHASH_CAP`) --
  a wrong answer rather than an error. Reachability unproven; tracked in
  `docs/roadmap/fixed-hash-silent-drop.md`
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
