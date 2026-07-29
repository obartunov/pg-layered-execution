#!/bin/bash
# ZLFS v0.1 persistence test
# Cross-session + server restart with full output
set -e

export PATH=/root/pginstall/bin:$PATH
export LD_LIBRARY_PATH=/root/pginstall/lib
PGDATA=/home/pguser/pgdata

run_sql() {
    su - pguser -c "export PATH=/root/pginstall/bin:\$PATH; export LD_LIBRARY_PATH=/root/pginstall/lib; psql testdb" <<PSQL
\timing on
LOAD 'xp_batch';
SET xp_batch.enabled = on;
SET xp_batch.groupagg2 = on;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
$1
PSQL
}

restart_pg() {
    su - pguser -c "export PATH=/root/pginstall/bin:\$PATH; export LD_LIBRARY_PATH=/root/pginstall/lib; pg_ctl -D $PGDATA restart -l $PGDATA/../pg.log" 2>&1 | tail -1
    sleep 2
}

# Clean
rm -rf $PGDATA/zlfs 2>/dev/null
run_sql "SELECT zlfs_drop_zone(25,36);" >/dev/null 2>&1 || true

echo '═══════════════════════════════════════════════════'
echo '  SESSION 1: Build zone, query, EXPLAIN ANALYZE'
echo '═══════════════════════════════════════════════════'
echo ''
run_sql "
SELECT zlfs_build_zone(25, 36);
SELECT * FROM zlfs_zone_info();

EXPLAIN (ANALYZE, SUMMARY ON)
SELECT period_key, company_key, sum(amount_dt) AS amt
FROM reg_buh WHERE period_key BETWEEN 25 AND 36
GROUP BY period_key, company_key;

SELECT count(*), sum(total_amt) FROM zlfs_group_sum(25, 36);
"

echo ''
echo '── Zone file on disk ──'
ls -lh $PGDATA/zlfs/
echo ''

echo '═══════════════════════════════════════════════════'
echo '  SESSION 2: New backend, auto-load from file'
echo '═══════════════════════════════════════════════════'
echo ''
run_sql "
SELECT * FROM zlfs_zone_info();

EXPLAIN (ANALYZE, SUMMARY ON)
SELECT period_key, company_key, sum(amount_dt) AS amt
FROM reg_buh WHERE period_key BETWEEN 25 AND 36
GROUP BY period_key, company_key;

SELECT count(*), sum(total_amt) FROM zlfs_group_sum(25, 36);
"

echo ''
echo '═══════════════════════════════════════════════════'
echo '  SESSION 3: Invalidate, verify STALE persists'
echo '═══════════════════════════════════════════════════'
echo ''
run_sql "
SELECT zlfs_invalidate_zone(25, 36);
SELECT * FROM zlfs_zone_info();
"

echo ''
echo '═══════════════════════════════════════════════════'
echo '  SESSION 4: New backend sees STALE → heap fallback'
echo '═══════════════════════════════════════════════════'
echo ''
run_sql "
SELECT * FROM zlfs_zone_info();

EXPLAIN (ANALYZE, SUMMARY ON)
SELECT period_key, company_key, sum(amount_dt) AS amt
FROM reg_buh WHERE period_key BETWEEN 25 AND 36
GROUP BY period_key, company_key;

SELECT count(*), sum(total_amt) FROM zlfs_group_sum(25, 36);
"

echo ''
echo '═══════════════════════════════════════════════════'
echo '  SESSION 5: Rebuild → VALID → ZLFS path'
echo '═══════════════════════════════════════════════════'
echo ''
run_sql "
SELECT zlfs_build_zone(25, 36);
SELECT * FROM zlfs_zone_info();

EXPLAIN (ANALYZE, SUMMARY ON)
SELECT period_key, company_key, sum(amount_dt) AS amt
FROM reg_buh WHERE period_key BETWEEN 25 AND 36
GROUP BY period_key, company_key;
"

echo ''
echo '═══════════════════════════════════════════════════'
echo '  SERVER RESTART'
echo '═══════════════════════════════════════════════════'
echo ''
restart_pg
echo ''

echo '═══════════════════════════════════════════════════'
echo '  SESSION 6: After restart, zone survives'
echo '═══════════════════════════════════════════════════'
echo ''
run_sql "
SELECT * FROM zlfs_zone_info();

EXPLAIN (ANALYZE, SUMMARY ON)
SELECT period_key, company_key, sum(amount_dt) AS amt
FROM reg_buh WHERE period_key BETWEEN 25 AND 36
GROUP BY period_key, company_key;

SELECT count(*), sum(total_amt) FROM zlfs_group_sum(25, 36);
"

echo ''
echo '═══════════════════════════════════════════════════'
echo '  SESSION 7: Drop → file removed'
echo '═══════════════════════════════════════════════════'
echo ''
run_sql "
SELECT zlfs_drop_zone(25, 36);
SELECT count(*) AS remaining_zones FROM zlfs_zone_info();
"
echo ''
echo '── Zone directory after drop ──'
ls -la $PGDATA/zlfs/ 2>&1
