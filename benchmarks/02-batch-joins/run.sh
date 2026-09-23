#!/bin/bash
#
# Experiment 2 — source -> BatchHashJoin(dim_period) -> BatchHashJoin(dim_account) -> Agg
#
#   benchmarks/02-batch-joins/run.sh [PGPORT] [PGHOST] [DBNAME]
#
# Assumes benchmarks/common/load.sh has run against the same database.
#
# Three paths over the same rows:
#
#   vanilla    plain SQL over the heap table (the PostgreSQL executor)
#   xpb_heap   xp_batch pipeline, heap source
#   xpb_zlfs   xp_batch pipeline, ZLFS source
#
# The correctness gate runs first. Timings are taken only once it passes, and
# only from warm runs.
set -euo pipefail

PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-testdb}"
LO=25
HI=36

PSQL=(psql -p "$PORT" -d "$DB" -v ON_ERROR_STOP=1 -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

# total_ms is a timing column and must stay out of the checksum, or no two runs
# of the same path can ever compare equal.
CK="md5(string_agg(year||','||account_group||','||company_key||','||total_amt,
    '|' ORDER BY year, account_group, company_key))"

SQL_BODY="SELECT d.year, a.account_group, r.company_key,
                 sum(r.amount_dt)::bigint AS total_amt
          FROM %s r
          JOIN dim_period  d ON d.period_key  = r.period_key
          JOIN dim_account a ON a.account_key = r.account_key
          WHERE r.period_key BETWEEN $LO AND $HI
          GROUP BY d.year, a.account_group, r.company_key"

echo "=== setup: ZLFS zone [$LO..$HI] ==="
"${PSQL[@]}" -c "CREATE EXTENSION IF NOT EXISTS xp_batch" >/dev/null
"${PSQL[@]}" -c "SELECT zlfs_build_zone('reg_buh', '1,2,3,6', $LO, $HI)" >/dev/null

echo "=== correctness gate ==="
{
cat <<SQL
SET max_parallel_workers_per_gather = 0;
SET jit = off;
CREATE TEMP TABLE ck(path text, ck text, groups bigint, ord int);

INSERT INTO ck SELECT 'vanilla', $CK, count(*), 1
FROM ($(printf "$SQL_BODY" reg_buh)) s;

INSERT INTO ck SELECT 'xpb_heap', $CK, count(*), 3
FROM xpb_batch_join2_groupby($LO, $HI, 'heap');

INSERT INTO ck SELECT 'xpb_zlfs', $CK, count(*), 4
FROM xpb_batch_join2_groupby($LO, $HI, 'zlfs');

-- repeat in the same backend: state left behind shows up here, not in the data
INSERT INTO ck SELECT 'xpb_zlfs_again', $CK, count(*), 6
FROM xpb_batch_join2_groupby($LO, $HI, 'zlfs');
SQL
cat <<'SQL'
SELECT rpad(path, 22) || ck || '  groups=' || groups FROM ck ORDER BY ord;
DO $$
DECLARE n int;
BEGIN
    SELECT count(DISTINCT ck) INTO n FROM ck;
    IF n <> 1 THEN RAISE EXCEPTION 'CHECKSUM MISMATCH across paths'; END IF;
    SELECT count(DISTINCT groups) INTO n FROM ck;
    IF n <> 1 THEN RAISE EXCEPTION 'GROUP COUNT MISMATCH across paths'; END IF;
    RAISE NOTICE 'gate PASS';
END $$;
SQL
} | "${PSQL[@]}"

echo "=== EXPLAIN: each SQL path takes the plan it is named for ==="
# same GUCs as the timed runs, or the plan shown is not the plan measured
for tbl in reg_buh; do
    echo "-- $tbl"
    "${PSQL[@]}" <<SQL | grep -E "Scan|Aggregate|Join" | head -6
SET max_parallel_workers_per_gather = 0;
SET jit = off;
EXPLAIN (COSTS OFF) $(printf "$SQL_BODY" $tbl);
SQL
done

echo "=== 5 warm runs per xp_batch path ==="
for mode in heap zlfs; do
    for i in 1 2 3 4 5; do
        "${PSQL[@]}" -c "SET max_parallel_workers_per_gather=0; SET jit=off;
                         SELECT count(*) FROM xpb_batch_join2_groupby($LO,$HI,'$mode')" 2>&1 >/dev/null \
        | sed -n 's/^NOTICE:  batch_join2_groupby //p'
    done
done

echo "=== 5 warm runs per SQL path ==="
for tbl in reg_buh; do
    echo "-- $tbl"
    for i in 1 2 3 4 5; do
        "${PSQL[@]}" <<SQL | grep -i '^time'
SET max_parallel_workers_per_gather = 0;
SET jit = off;
\\timing on
SELECT count(*) FROM ($(printf "$SQL_BODY" $tbl)) s;
SQL
    done
done
