#!/bin/bash
#
# Regression guards for two defects that both presented as a SIGSEGV in a
# LATER, unrelated call in the same backend — the class a data-only check
# cannot see, because the data was right every time.
#
#   test/regression_guards.sh [PGPORT] [PGHOST] [DBNAME]
#
# G1  dim_build/dim2_build address dimension columns by fixed attnum. Given a
#     table without them, heap_getattr() reaches getmissingattr() for an
#     attribute that has no missing-value entry and the backend dies. They must
#     now refuse it with an ERROR -- and require only the attnums they actually
#     read, which is 1 and 2. A third column is not needed: the original
#     dim_period has `month` there and nothing ever consumed it.
#
# G2  zlfs_scan_directory() validated a zone's schema inside PG_TRY and left the
#     block with `continue`, skipping PG_END_TRY. PG_exception_stack was then
#     left pointing into a dead frame, so the next error path in that backend
#     crashed it. A zone file whose source table has been dropped is enough to
#     arm it; the guard is that the backend still works after the warning.
set -euo pipefail

PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-testdb}"

PSQL=(psql -p "$PORT" -d "$DB" -v ON_ERROR_STOP=1 -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

echo "=== G1: dimension of the wrong shape must ERROR, not crash ==="
# TEMP tables shadow the benchmark ones: RelnameGetRelid() resolves through
# search_path, and pg_temp precedes public.
"${PSQL[@]}" <<'SQL'
CREATE EXTENSION IF NOT EXISTS xp_batch;

CREATE TEMP TABLE reg_buh (period_key int NOT NULL, company_key int NOT NULL,
                           account_key int NOT NULL, d int NOT NULL, c int NOT NULL,
                           amount_dt int NOT NULL, k int NOT NULL, p int NOT NULL);
INSERT INTO reg_buh SELECT 25, g%50+1, g%200+1, 1, 1, g, 1, 1
FROM generate_series(1, 1000) g;
CREATE TEMP TABLE dim_account (account_key int NOT NULL, account_group int NOT NULL);
INSERT INTO dim_account SELECT g, (g-1)/50+1 FROM generate_series(1,200) g;

-- two columns is the real requirement, and it must WORK
CREATE TEMP TABLE dim_period (period_key int NOT NULL, year int NOT NULL);
INSERT INTO dim_period SELECT id, (id-1)/12+2015 FROM generate_series(1,120) id;
DO $$
DECLARE n bigint;
BEGIN
    SELECT count(*) INTO n FROM xpb_batch_join_groupby(25, 25, 'heap');
    IF n <> 50 THEN
        RAISE EXCEPTION 'G1a FAILED: two-column dim_period gave % groups, want 50', n;
    END IF;
    RAISE NOTICE 'G1a PASS: two columns are enough';
END $$;

-- one column is not
DROP TABLE dim_period;
CREATE TEMP TABLE dim_period (period_key int NOT NULL);
INSERT INTO dim_period SELECT id FROM generate_series(1,120) id;
DO $$
DECLARE msg text; caught boolean := false;
BEGIN
    BEGIN
        PERFORM count(*) FROM xpb_batch_join_groupby(25, 25, 'heap');
    EXCEPTION WHEN datatype_mismatch THEN
        GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT;
        caught := true;
    END;
    IF NOT caught THEN
        RAISE EXCEPTION 'G1b FAILED: a one-column dim_period was accepted';
    END IF;
    RAISE NOTICE 'G1b PASS: %', msg;
END $$;

-- neither is a dropped column in a read position
DROP TABLE dim_period;
CREATE TEMP TABLE dim_period (period_key int NOT NULL, gone int NOT NULL, year int NOT NULL);
INSERT INTO dim_period SELECT id, 0, (id-1)/12+2015 FROM generate_series(1,120) id;
ALTER TABLE dim_period DROP COLUMN gone;
DO $$
DECLARE msg text; caught boolean := false;
BEGIN
    BEGIN
        PERFORM count(*) FROM xpb_batch_join_groupby(25, 25, 'heap');
    EXCEPTION WHEN datatype_mismatch THEN
        GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT;
        caught := true;
    END;
    IF NOT caught THEN
        RAISE EXCEPTION 'G1c FAILED: a dropped dimension column was accepted';
    END IF;
    RAISE NOTICE 'G1c PASS: %', msg;
END $$;
SQL

echo "=== G2: arm an orphan zone, then run two pipelines in one backend ==="
# Step 1, its own session: leave a zone file whose source table is gone.
"${PSQL[@]}" <<'SQL'
CREATE TABLE zg2_fact (period_key int NOT NULL, company_key int NOT NULL,
                       account_key int NOT NULL, d int NOT NULL, c int NOT NULL,
                       amount_dt int NOT NULL, k int NOT NULL, p int NOT NULL);
INSERT INTO zg2_fact SELECT 25, g%50+1, g%200+1, 1, 1, g, 1, 1
FROM generate_series(1, 5000) g;
SELECT zlfs_build_zone('zg2_fact', '1,2,3,6', 991, 992);
DROP TABLE zg2_fact;
SQL

# Step 2, ONE session: the scan meets the orphan and warns, then the pipeline
# runs inside INSERT ... SELECT. Both halves are needed to fire it. The warning
# alone is survivable and `SELECT count(*) FROM srf` alone is survivable; the
# crash (SIGSEGV in pfree) needs the orphan to have armed it AND a statement
# that allocates through the context the catch arm left current. Keep the
# INSERT form: plain SELECT here passes on the broken code too.
"${PSQL[@]}" <<'SQL'
CREATE TEMP TABLE reg_buh (period_key int NOT NULL, company_key int NOT NULL,
                           account_key int NOT NULL, d int NOT NULL, c int NOT NULL,
                           amount_dt int NOT NULL, k int NOT NULL, p int NOT NULL);
INSERT INTO reg_buh SELECT 25 + g%12, g%50+1, g%200+1, 1, 1, g, 1, 1
FROM generate_series(1, 20000) g;
CREATE TEMP TABLE dim_period (period_key int NOT NULL, year int NOT NULL);
INSERT INTO dim_period SELECT id, (id-1)/12+2015 FROM generate_series(1,120) id;
CREATE TEMP TABLE dim_account (account_key int NOT NULL, account_group int NOT NULL);
INSERT INTO dim_account SELECT g, (g-1)/50+1 FROM generate_series(1,200) g;

SELECT zlfs_build_zone('reg_buh', '1,2,3,6', 25, 36);

CREATE TEMP TABLE g2_ck(path text, ck text, groups bigint);
INSERT INTO g2_ck SELECT 'zlfs',
       md5(string_agg(year||','||account_group||','||company_key||','||total_amt,
                      '|' ORDER BY year, account_group, company_key)), count(*)
FROM xpb_batch_join2_groupby(25, 36, 'zlfs');
INSERT INTO g2_ck SELECT 'heap',
       md5(string_agg(year||','||account_group||','||company_key||','||total_amt,
                      '|' ORDER BY year, account_group, company_key)), count(*)
FROM xpb_batch_join2_groupby(25, 36, 'heap');

DO $$
DECLARE n int;
BEGIN
    SELECT count(DISTINCT ck) INTO n FROM g2_ck;
    IF n <> 1 THEN RAISE EXCEPTION 'G2 FAILED: paths disagree'; END IF;
    RAISE NOTICE 'G2 PASS: backend survived the orphan warning and both pipelines';
END $$;
SELECT zlfs_drop_zone(25, 36);
SQL

echo "All regression guards PASSED"
