#!/bin/bash
#
# Benchmark 04 — 1C-like analytical register report v1
#
#   benchmarks/04-1c-like-register/run.sh [PGPORT] [PGHOST] [DBNAME]
#
# Assumes this directory's load.sh has run against the same database.
#
# Hypothesis under test: on one and the same annual section of the register, a
# compact batch pipeline lowers the cost of reading, decoding, joining and
# aggregating relative to the stock PostgreSQL executor.
#
# Every path reads the SAME rows: the whole annual section, periods 1..12.
# Nothing here compares a vanilla scan of ten years against a ZLFS zone holding
# one -- there is only one year in the dataset.
#
# Two stock baselines, kept apart on purpose:
#   vanilla_single   max_parallel_workers_per_gather=0, jit=off — an executor
#                    architecture comparison on one core
#   vanilla_default  planner left alone — the practical baseline
# Their numbers are never averaged together.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-onec}"
LO=1
HI=12
RUNS=5

PSQL=(psql -p "$PORT" -d "$DB" -v ON_ERROR_STOP=1 -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

HAVE_COL=$("${PSQL[@]}" -c "SELECT to_regclass('reg_buh_col') IS NOT NULL")
[ "$HAVE_COL" = "t" ] || echo "!! reg_buh_col absent: columnar paths skipped"

# total_ms is a timing column and stays out of the checksum. All three
# aggregates are in it, so a path that got debit right and credit wrong cannot
# pass.
CK="md5(string_agg(year||','||account_group||','||company_key||','||
                   debit_turnover||','||credit_turnover||','||net_turnover,
                   '|' ORDER BY year, account_group, company_key))"

SQL_BODY="SELECT p.year, a.account_group, r.company_key,
                 sum(r.amount_dt)::bigint AS debit_turnover,
                 sum(r.amount_kt)::bigint AS credit_turnover,
                 sum(r.amount_dt::bigint - r.amount_kt::bigint) AS net_turnover
          FROM %s r
          JOIN dim_period  p ON p.period_key  = r.period_key
          JOIN dim_account a ON a.account_key = r.account_key
          WHERE r.period_key BETWEEN $LO AND $HI
          GROUP BY p.year, a.account_group, r.company_key"

echo "=== setup: ZLFS annual zone [$LO..$HI] ==="
"${PSQL[@]}" -c "CREATE EXTENSION IF NOT EXISTS xp_batch" >/dev/null
"${PSQL[@]}" -c "SELECT zlfs_drop_zone($LO, $HI)" >/dev/null 2>&1 || true
# build cost and zone size are recorded, and kept out of query latency
"${PSQL[@]}" -c "SELECT zlfs_build_zone('reg_buh', '1,2,3,6,7', $LO, $HI)" 2>&1 \
    | sed -n 's/^NOTICE:  //p'

echo
echo "=== correctness gate ==="
{
cat <<SQL
SET max_parallel_workers_per_gather = 0;
SET jit = off;
CREATE TEMP TABLE ck(path text, ck text, groups bigint,
                     debit bigint, credit bigint, net bigint, ord int);

INSERT INTO ck SELECT 'vanilla_single', $CK, count(*),
       sum(debit_turnover), sum(credit_turnover), sum(net_turnover), 1
FROM ($(printf "$SQL_BODY" reg_buh)) s;

INSERT INTO ck SELECT 'xpb_heap', $CK, count(*),
       sum(debit_turnover), sum(credit_turnover), sum(net_turnover), 3
FROM xpb_1c_register_report($LO, $HI, 'heap');

INSERT INTO ck SELECT 'xpb_zlfs', $CK, count(*),
       sum(debit_turnover), sum(credit_turnover), sum(net_turnover), 4
FROM xpb_1c_register_report($LO, $HI, 'zlfs');

INSERT INTO ck SELECT 'xpb_zlfs_again', $CK, count(*),
       sum(debit_turnover), sum(credit_turnover), sum(net_turnover), 6
FROM xpb_1c_register_report($LO, $HI, 'zlfs');
SQL
if [ "$HAVE_COL" = "t" ]; then
cat <<SQL
INSERT INTO ck SELECT 'native_columnar', $CK, count(*),
       sum(debit_turnover), sum(credit_turnover), sum(net_turnover), 2
FROM ($(printf "$SQL_BODY" reg_buh_col)) s;

INSERT INTO ck SELECT 'xpb_pgcolumnar', $CK, count(*),
       sum(debit_turnover), sum(credit_turnover), sum(net_turnover), 5
FROM xpb_1c_register_report($LO, $HI, 'pgcolumnar');

INSERT INTO ck SELECT 'xpb_pgcolumnar_again', $CK, count(*),
       sum(debit_turnover), sum(credit_turnover), sum(net_turnover), 7
FROM xpb_1c_register_report($LO, $HI, 'pgcolumnar');
SQL
fi
cat <<'SQL'
SELECT rpad(path, 22) || ck || '  groups=' || groups FROM ck ORDER BY ord;

DO $$
DECLARE n int; bad int;
BEGIN
    SELECT count(DISTINCT ck) INTO n FROM ck;
    IF n <> 1 THEN RAISE EXCEPTION 'CHECKSUM MISMATCH across paths'; END IF;
    SELECT count(DISTINCT groups) INTO n FROM ck;
    IF n <> 1 THEN RAISE EXCEPTION 'GROUP COUNT MISMATCH across paths'; END IF;
    SELECT count(*) INTO bad FROM ck WHERE net <> debit - credit;
    IF bad <> 0 THEN RAISE EXCEPTION 'net <> debit - credit in % paths', bad; END IF;
    RAISE NOTICE 'gate PASS: one checksum, one group count, net = debit - credit';
END $$;
SQL
} | "${PSQL[@]}"

echo
echo "=== per-group identity: net = debit - credit for every group ==="
"${PSQL[@]}" -c "
SELECT 'groups where net <> debit - credit: ' || count(*)
FROM xpb_1c_register_report($LO, $HI, 'heap')
WHERE net_turnover <> debit_turnover - credit_turnover" 2>/dev/null

echo
echo "=== empty range and boundary range leave no state behind ==="
"${PSQL[@]}" <<SQL 2>&1 | sed -n 's/^NOTICE:  //p;/rows/p'
SET max_parallel_workers_per_gather = 0;
SET jit = off;
\\echo -- empty range (periods 90..91): expect 0 groups
SELECT count(*) AS rows FROM xpb_1c_register_report(90, 91, 'heap');
\\echo -- single boundary period 12
SELECT count(*) AS rows FROM xpb_1c_register_report(12, 12, 'heap');
\\echo -- full section again in the same backend: must be 200 again
SELECT count(*) AS rows FROM xpb_1c_register_report($LO, $HI, 'heap');
SQL

echo
echo "=== EXPLAIN, under the same GUCs as each timed run ==="
for tbl in reg_buh reg_buh_col; do
    [ "$tbl" = "reg_buh_col" ] && [ "$HAVE_COL" != "t" ] && continue
    echo "-- $tbl, vanilla_single"
    "${PSQL[@]}" <<SQL | grep -E "Scan|Aggregate|Join|Gather" | head -8
SET max_parallel_workers_per_gather = 0;
SET jit = off;
EXPLAIN (COSTS OFF) $(printf "$SQL_BODY" $tbl);
SQL
done
echo "-- reg_buh, vanilla_default (planner left alone)"
"${PSQL[@]}" -c "EXPLAIN (COSTS OFF) $(printf "$SQL_BODY" reg_buh)" \
    | grep -E "Scan|Aggregate|Join|Gather" | head -8
echo "-- GUCs in force for vanilla_default:"
"${PSQL[@]}" -c "SELECT name||'='||setting FROM pg_settings
                 WHERE name IN ('max_parallel_workers_per_gather','jit',
                                'work_mem','shared_buffers')"

echo
echo "=== $RUNS warm runs per xp_batch path ==="
MODES="heap zlfs"
[ "$HAVE_COL" = "t" ] && MODES="$MODES pgcolumnar"
for mode in $MODES; do
    for i in $(seq $RUNS); do
        "${PSQL[@]}" -c "SET max_parallel_workers_per_gather=0; SET jit=off;
                         SELECT count(*) FROM xpb_1c_register_report($LO,$HI,'$mode')" 2>&1 >/dev/null \
        | sed -n 's/^NOTICE:  1c_register_report //p'
    done
done

echo
echo "=== $RUNS warm runs per SQL path ==="
for tbl in reg_buh reg_buh_col; do
    [ "$tbl" = "reg_buh_col" ] && [ "$HAVE_COL" != "t" ] && continue
    echo "-- $tbl vanilla_single"
    for i in $(seq $RUNS); do
        "${PSQL[@]}" <<SQL | grep -i '^time'
SET max_parallel_workers_per_gather = 0;
SET jit = off;
\\timing on
SELECT count(*) FROM ($(printf "$SQL_BODY" $tbl)) s;
SQL
    done
done
echo "-- reg_buh vanilla_default"
for i in $(seq $RUNS); do
    "${PSQL[@]}" <<SQL | grep -i '^time'
\\timing on
SELECT count(*) FROM ($(printf "$SQL_BODY" reg_buh)) s;
SQL
done
