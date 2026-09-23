-- xp_batch extension: registers via shared_preload or LOAD
-- No SQL objects needed — all functionality via GUCs and hooks
-- This file intentionally minimal.

DO $$
BEGIN
  RAISE NOTICE 'xp_batch 1.0 installed. Use: LOAD ''xp_batch''; SET xp_batch.enabled = on;';
END $$;

-- Columnar TR experiment: measures row vs columnar carrier cost
CREATE OR REPLACE FUNCTION ctr_experiment(ngroups bigint, iterations int DEFAULT 10)
RETURNS text
LANGUAGE C STRICT
AS '$libdir/xp_batch', 'ctr_experiment';

-- Columnar TR pipeline: end-to-end two-stage pipeline comparison
CREATE OR REPLACE FUNCTION ctr_pipeline(lo int, hi int)
RETURNS TABLE(period_key int, total_amt bigint, path text, stage_ms float8)
LANGUAGE C STRICT
AS '$libdir/xp_batch', 'ctr_pipeline';

CREATE OR REPLACE FUNCTION projection_experiment(lo int, hi int)
RETURNS TABLE(period_key int, total_amt bigint, path text, scan_ms float8)
LANGUAGE C STRICT
AS '$libdir/xp_batch', 'projection_experiment';

-- ZLFS v0: derived analytical zones
CREATE OR REPLACE FUNCTION zlfs_build_zone(relname text, cols_csv text, lo int, hi int)
RETURNS text LANGUAGE C STRICT
AS '$libdir/xp_batch', 'zlfs_build_zone';

CREATE OR REPLACE FUNCTION zlfs_group_sum(lo int, hi int)
RETURNS TABLE(period_key int, company_key int, total_amt bigint, scan_ms float8)
LANGUAGE C STRICT
AS '$libdir/xp_batch', 'zlfs_group_sum';

CREATE OR REPLACE FUNCTION zlfs_zone_info()
RETURNS TABLE(period_lo int, period_hi int, nrows bigint, size_kb bigint,
              freshness text, build_time timestamptz, filepath text)
LANGUAGE C STRICT
AS '$libdir/xp_batch', 'zlfs_zone_info';

CREATE OR REPLACE FUNCTION zlfs_invalidate_zone(lo int, hi int)
RETURNS text LANGUAGE C STRICT
AS '$libdir/xp_batch', 'zlfs_invalidate_zone';

CREATE OR REPLACE FUNCTION zlfs_drop_zone(lo int, hi int)
RETURNS void LANGUAGE C STRICT
AS '$libdir/xp_batch', 'zlfs_drop_zone';


/* batch_source_v1: generic compact-batch group aggregate */
CREATE OR REPLACE FUNCTION xpb_batch_groupby(lo int, hi int, source_mode text)
RETURNS TABLE(period_key int, company_key int, total_amt bigint, total_ms float8)
LANGUAGE C STRICT
AS '$libdir/xp_batch', 'xpb_batch_groupby';

CREATE OR REPLACE FUNCTION xpb_batch_join_groupby(lo int, hi int, source_mode text)
RETURNS TABLE(year int, company_key int, total_amt bigint, total_ms float8)
LANGUAGE C STRICT
AS '$libdir/xp_batch', 'xpb_batch_join_groupby';

CREATE OR REPLACE FUNCTION xpb_batch_join2_groupby(lo int, hi int, source_mode text)
RETURNS TABLE(year int, account_group int, company_key int, total_amt bigint, total_ms float8)
LANGUAGE C STRICT
AS '$libdir/xp_batch', 'xpb_batch_join2_groupby';

CREATE OR REPLACE FUNCTION xpb_partition_join2_groupby(
    parent_table text, period_from int, period_to int)
RETURNS TABLE(year int, account_group int, company_key int, total_amt bigint, total_ms float8)
LANGUAGE C STRICT
AS '$libdir/xp_batch', 'xpb_partition_join2_groupby';

-- 1C-like analytical register report (benchmarks/04-1c-like-register):
-- two resources carried through the pipeline, three aggregates in one pass.
CREATE OR REPLACE FUNCTION xpb_1c_register_report(lo int, hi int, source_mode text)
RETURNS TABLE(year int, account_group int, company_key int,
              debit_turnover bigint, credit_turnover bigint,
              net_turnover bigint, total_ms float8)
LANGUAGE C STRICT
AS '$libdir/xp_batch', 'xpb_1c_register_report';

-- Contract test harness: read a relation through the batch contract and hand
-- back what the batch actually held, per column, including type and
-- NULLability. Not part of any pipeline; used by test/contract_tests.sh to
-- compare the batch layer against PostgreSQL's own view of the same table.
CREATE OR REPLACE FUNCTION xpb_contract_probe(relname text, attnos int[], path text)
RETURNS TABLE(rownum bigint, col int, coltype text, is_null bool, val text)
LANGUAGE C STRICT
AS '$libdir/xp_batch', 'xpb_contract_probe';
