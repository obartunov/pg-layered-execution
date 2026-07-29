-- ZLFS v0.1 regression tests
-- File-backed persistent analytical zones

LOAD 'xp_batch';
SET client_min_messages = warning;

-- ═══ Setup ═══
-- Assumes reg_buh exists with 10M rows

-- Clean any leftover zones
SELECT zlfs_drop_zone(25, 36);
SELECT zlfs_drop_zone(1, 120);

-- ═══ T1: No zone → heap fallback ═══
SELECT count(*) AS groups,
       sum(total_amt) AS total,
       round(avg(scan_ms)::numeric, 0) > 100 AS heap_path_slow
FROM zlfs_group_sum(25, 36);

-- ═══ T2: Build zone ═══
SELECT zlfs_build_zone(25, 36);

-- ═══ T3: Zone info shows VALID ═══
SELECT period_lo, period_hi, nrows, freshness,
       filepath LIKE '%zone_%_25_36.zlfs' AS file_pattern_ok
FROM zlfs_zone_info()
WHERE period_lo = 25 AND period_hi = 36;

-- ═══ T4: ZLFS scan works and is fast ═══
SELECT count(*) AS groups,
       sum(total_amt) AS total,
       round(avg(scan_ms)::numeric, 0) < 50 AS zlfs_path_fast
FROM zlfs_group_sum(25, 36);

-- ═══ T5: Correctness — ZLFS matches heap truth ═══
WITH zlfs AS (
    SELECT period_key, company_key, total_amt
    FROM zlfs_group_sum(25, 36)
),
heap AS (
    SELECT period_key, company_key, sum(amount_dt) AS total_amt
    FROM reg_buh
    WHERE period_key BETWEEN 25 AND 36
    GROUP BY period_key, company_key
)
SELECT count(*) AS matching_groups,
       (SELECT count(*) FROM zlfs) AS zlfs_groups,
       (SELECT count(*) FROM heap) AS heap_groups
FROM (SELECT * FROM zlfs INTERSECT SELECT * FROM heap) x;

-- ═══ T6: Invalidate → STALE ═══
SELECT zlfs_invalidate_zone(25, 36);

SELECT freshness FROM zlfs_zone_info()
WHERE period_lo = 25 AND period_hi = 36;

-- ═══ T7: STALE → heap fallback (slow) ═══
SELECT count(*) AS groups,
       sum(total_amt) AS total,
       round(avg(scan_ms)::numeric, 0) > 100 AS fallback_slow
FROM zlfs_group_sum(25, 36);

-- ═══ T8: Rebuild → VALID again ═══
SELECT zlfs_build_zone(25, 36);

SELECT freshness FROM zlfs_zone_info()
WHERE period_lo = 25 AND period_hi = 36;

-- ═══ T9: Rebuilt zone produces same results ═══
SELECT count(*) AS groups, sum(total_amt) AS total
FROM zlfs_group_sum(25, 36);

-- ═══ T10: Uncovered range → heap fallback ═══
SELECT count(*) AS groups,
       round(avg(scan_ms)::numeric, 0) > 100 AS uncovered_uses_heap
FROM zlfs_group_sum(1, 120);

-- ═══ T11: Multiple zones can coexist ═══
SELECT zlfs_build_zone(1, 12);

SELECT count(*) AS zone_count FROM zlfs_zone_info();

SELECT count(*) AS groups, sum(total_amt) AS total
FROM zlfs_group_sum(1, 12);

-- ═══ T12: Drop zone removes file ═══
SELECT zlfs_drop_zone(1, 12);

SELECT count(*) AS zone_count FROM zlfs_zone_info();

-- ═══ T13: EXPLAIN ANALYZE shows ZLFS path ═══
SET xp_batch.enabled = on;
SET xp_batch.groupagg2 = on;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT period_key, company_key, sum(amount_dt) AS amt
FROM reg_buh WHERE period_key BETWEEN 25 AND 36
GROUP BY period_key, company_key;

-- ═══ T14: Invalidate → EXPLAIN shows heap fallback ═══
SELECT zlfs_invalidate_zone(25, 36);

EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT period_key, company_key, sum(amount_dt) AS amt
FROM reg_buh WHERE period_key BETWEEN 25 AND 36
GROUP BY period_key, company_key;

-- ═══ Cleanup ═══
SELECT zlfs_drop_zone(25, 36);
SELECT count(*) AS remaining_zones FROM zlfs_zone_info();
