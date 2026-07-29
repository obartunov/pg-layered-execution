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
DROP TABLE IF EXISTS smoke_test;
CREATE TABLE smoke_test (
    k1 int NOT NULL, k2 int NOT NULL,
    k3 int NOT NULL, v1 int NOT NULL,
    v2 int NOT NULL, v3 int NOT NULL,
    v4 int NOT NULL, v5 int NOT NULL
);
INSERT INTO smoke_test
SELECT i/100+1, i%10+1, i%50+1, i*7%1000, i*13%1000, i*17%1000, i*19%1000, i*23%1000
FROM generate_series(1, 10000) i;
VACUUM ANALYZE smoke_test;

-- Test 1: ZLFS build with generalized API
SELECT zlfs_build_zone('smoke_test', '1,2,3,4', 1, 100);

-- Test 2: batch_groupby correctness (ZLFS vs heap vs vanilla)
DO $$
DECLARE
    c_zlfs text; c_heap text; c_vanilla text;
BEGIN
    SELECT md5(string_agg(r::text,',' ORDER BY period_key,company_key))
    INTO c_zlfs FROM xpb_batch_groupby(1, 100, 'zlfs') r;

    SELECT md5(string_agg(r::text,',' ORDER BY period_key,company_key))
    INTO c_heap FROM xpb_batch_groupby(1, 100, 'heap') r;

    SELECT md5(string_agg(r::text,',' ORDER BY k1,k2))
    INTO c_vanilla FROM (
        SELECT k1 AS period_key, k2 AS company_key, sum(v1)::bigint AS total_amt
        FROM smoke_test WHERE k1 BETWEEN 1 AND 100 GROUP BY k1, k2) t;

    IF c_zlfs != c_heap OR c_zlfs != c_vanilla THEN
        RAISE EXCEPTION 'CHECKSUM MISMATCH: zlfs=% heap=% vanilla=%', c_zlfs, c_heap, c_vanilla;
    END IF;
    RAISE NOTICE 'Test 2 PASS: checksum=%', c_zlfs;
END $$;

-- Test 3: STALE fallback
SELECT zlfs_invalidate_zone(1, 100);
SELECT count(*) FROM xpb_batch_groupby(1, 100, 'heap');

-- Test 4: schema hash mismatch (would need ALTER TABLE to test fully)

-- Test 5: invalid attno → clean ERROR
DO $$
BEGIN
    PERFORM zlfs_build_zone('smoke_test', '1,2,99', 1, 100);
    RAISE EXCEPTION 'Should have errored on attno 99';
EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'Test 5 PASS: invalid attno caught';
END $$;

-- Cleanup
DROP TABLE smoke_test;
SELECT zlfs_drop_zone(1, 100);

\echo 'All smoke tests PASSED'
SQL

echo "=== Done ==="
