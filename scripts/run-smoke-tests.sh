#!/bin/bash
set -e
PORT=${1:-5432}
PSQL="psql -p $PORT testdb -v ON_ERROR_STOP=1"

echo "=== Smoke tests ==="

$PSQL << 'SQL'
-- Setup
CREATE EXTENSION IF NOT EXISTS xp_batch;
LOAD 'xp_batch';
SET xp_batch.enabled = on;
SET xp_batch.groupagg2 = on;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

-- Create small test table
DROP TABLE IF EXISTS reg_buh;
CREATE TABLE reg_buh (
    k1 int NOT NULL, k2 int NOT NULL,
    k3 int NOT NULL, v1 int NOT NULL,
    v2 int NOT NULL, v3 int NOT NULL,
    v4 int NOT NULL, v5 int NOT NULL
);
INSERT INTO reg_buh
SELECT i/100+1, i%10+1, i%50+1, i*7%1000, i*13%1000, i*17%1000, i*19%1000, i*23%1000
FROM generate_series(1, 10000) i;
VACUUM ANALYZE reg_buh;

-- Test 1: ZLFS build with generalized API
SELECT zlfs_build_zone('reg_buh', '1,2,6', 1, 100);

-- Test 2: batch_groupby correctness (ZLFS vs heap vs vanilla)
DO $$
DECLARE
    c_zlfs text; c_heap text; c_vanilla text;
BEGIN
    -- total_ms is a timing column: exclude it from the checksum,
    -- otherwise no two paths can ever compare equal.
    SELECT md5(string_agg(t::text,',' ORDER BY period_key,company_key))
    INTO c_zlfs FROM (
        SELECT period_key, company_key, total_amt
        FROM xpb_batch_groupby(1, 100, 'zlfs')) t;

    SELECT md5(string_agg(t::text,',' ORDER BY period_key,company_key))
    INTO c_heap FROM (
        SELECT period_key, company_key, total_amt
        FROM xpb_batch_groupby(1, 100, 'heap')) t;

    -- xpb_batch_groupby aggregates attnos {1,2,6} = k1, k2, v3
    SELECT md5(string_agg(t::text,',' ORDER BY period_key,company_key))
    INTO c_vanilla FROM (
        SELECT k1 AS period_key, k2 AS company_key, sum(v3)::bigint AS total_amt
        FROM reg_buh WHERE k1 BETWEEN 1 AND 100 GROUP BY k1, k2) t;

    IF c_zlfs != c_heap OR c_zlfs != c_vanilla THEN
        RAISE EXCEPTION 'CHECKSUM MISMATCH: zlfs=% heap=% vanilla=%', c_zlfs, c_heap, c_vanilla;
    END IF;
    RAISE NOTICE 'Test 2 PASS: checksum=%', c_zlfs;
END $$;

-- Test 3: heap path remains available after ZLFS invalidation.
-- This is NOT a provider-fallback test: the mode is chosen explicitly.
-- Real fallback must be tested through partition routing.
SELECT zlfs_invalidate_zone(1, 100);
SELECT count(*) FROM xpb_batch_groupby(1, 100, 'heap');

-- Test 4: TODO — schema hash mismatch (needs ALTER TABLE, not covered here)

-- Test 5: invalid attno → clean ERROR
DO $$
DECLARE
    error_caught boolean := false;
    error_message text;
BEGIN
    -- Nested block: the outer handler must not swallow our own
    -- "did not error" exception and report it as a PASS.
    BEGIN
        PERFORM zlfs_build_zone('reg_buh', '1,2,99', 1, 100);
    EXCEPTION WHEN OTHERS THEN
        GET STACKED DIAGNOSTICS error_message = MESSAGE_TEXT;

        -- zlfs_compute_offsets(): "ZLFS: attno %d out of range (1..%d)"
        IF error_message NOT LIKE 'ZLFS: attno 99 out of range%' THEN
            RAISE;
        END IF;

        error_caught := true;
    END;

    IF NOT error_caught THEN
        RAISE EXCEPTION
            'Test 5 FAILED: zlfs_build_zone accepted invalid attno 99';
    END IF;

    RAISE NOTICE 'Test 5 PASS: %', error_message;
END $$;

-- Cleanup
DROP TABLE reg_buh;
SELECT zlfs_drop_zone(1, 100);

\echo 'All smoke tests PASSED'
SQL

echo "=== Done ==="
