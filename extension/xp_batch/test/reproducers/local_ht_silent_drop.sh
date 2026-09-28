#!/bin/bash
#
# Reproducer: xpb_groupagg2.c local_ht drops groups silently
#
#   extension/xp_batch/test/reproducers/local_ht_silent_drop.sh <PGPORT> [PGHOST] [DB]
#
# THIS SCRIPT IS EXPECTED TO FAIL while the defect is open. It is deliberately
# NOT called by contract_tests.sh: it demonstrates a live wrong answer, and the
# reachability milestone requires the broken behaviour to stay observable until
# the question "are any published results affected" has been answered. It turns
# into a regression test when the fix lands, by inverting its verdict.
#
# Site 3 of docs/roadmap/fixed-hash-silent-drop.md.
#
# What makes it reachable, which is the part that is not obvious from the code:
#
#   xpga2_add_path() requires |correlation(k1)| >= 0.8, on the reasoning that
#   XpGroupAgg2 only wins when STREAM activates.  But local_ht is only used on
#   the hash fallback (`if (!is_sorted)`, xpb_groupagg2.c:419), and is_sorted is
#   decided by an OPTIMISTIC PROBE OF PAGE 0 ALONE (:362-390).  So the reachable
#   shape is a table that is globally clustered -- which is what the planner gate
#   asks for -- but whose first page is locally out of order.  A few rows
#   inserted ahead of a clustered body is enough; here it is exactly one.
#
# The fill threshold is per BATCH, not per query: local_ht is memset at :627 and
# :738, so what has to exceed local_cap is the number of distinct (k1,k2) pairs
# inside one ScalarBatch of at most SB_CAPACITY = 65536 rows.  These datasets are
# under 2000 rows and go through the final partial flush as a single batch.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-onec2}"

PSQL=(psql -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

# LOAD is per-session: the GUCs do not exist until the library is loaded.
PRE="LOAD 'xp_batch'; SET jit=off; SET max_parallel_workers_per_gather=0;"
fail=0

echo "############ local_ht silent drop -- reproducer ############"
echo

"${PSQL[@]}" -v ON_ERROR_STOP=1 >/dev/null <<'SQL'
DROP TABLE IF EXISTS sd_local;
CREATE TABLE sd_local (k1 int4 NOT NULL, k2 int4 NOT NULL, v int8 NOT NULL);

-- Rebuild for one ladder point.  One out-of-order row first so the page-0
-- probe fails and execution takes the hash fallback; then a clustered body of
-- n distinct k1 values, three rows each, so every group's true sum is 3.
CREATE OR REPLACE FUNCTION sd_build(ndistinct int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    TRUNCATE sd_local;
    INSERT INTO sd_local VALUES (1000000, 0, 1);
    INSERT INTO sd_local SELECT g / 3, 0, 1 FROM generate_series(0, ndistinct * 3 - 1) g;
    ANALYZE sd_local;
END $$;
SQL

echo "=== the shape is reachable through the planner, not forced ==="
echo "Global correlation passes the >= 0.8 gate; page 0 is out of order, so"
echo "execution still takes the hash fallback that owns local_ht."
"${PSQL[@]}" -c "SELECT sd_build(300)" >/dev/null
"${PSQL[@]}" -c "SELECT '  k1 correlation = ' || correlation
                 FROM pg_stats WHERE tablename='sd_local' AND attname='k1'"

echo
echo "=== source-mode proof: both xp_batch arms really run XpGroupAgg2 ==="
echo "Without this the comparison below could be PostgreSQL against itself."
for lp in off on; do
    plan=$("${PSQL[@]}" -c "$PRE
            SET xp_batch.enabled = on; SET xp_batch.groupagg2 = on;
            SET xp_batch.groupagg2_local_partial = $lp;
            SET xp_batch.groupagg2_local_hash_cap = 256;
            EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF)
            SELECT k1, k2, sum(v) FROM sd_local GROUP BY k1, k2" 2>&1 | grep -v '^NOTICE')
    node=$(grep -c 'Custom Scan (XpGroupAgg2)' <<<"$plan")
    scope=$(sed -n 's/.*Batch Grouping: //p' <<<"$plan")
    batches=$(sed -n 's/.*LP Batches: \([0-9]*\).*/\1/p' <<<"$plan")
    if [ "$node" -eq 1 ]; then
        echo "  PASS  local_partial=$lp uses XpGroupAgg2 (Batch Grouping: ${scope:-NONE}${batches:+, $batches batch})"
    else
        echo "  FAIL  local_partial=$lp did NOT use XpGroupAgg2 -- the comparison would be vacuous"
        fail=1
    fi
done

echo
echo "=== three-way comparison, 301 distinct groups against a 256-slot local hash ==="
"${PSQL[@]}" -v ON_ERROR_STOP=1 <<SQL 2>&1 | grep -v '^NOTICE'
$PRE
SELECT sd_build(300);
CREATE TEMP TABLE arm(name text, groups bigint, total bigint, ck text, ord int);

SET xp_batch.enabled = off; SET xp_batch.groupagg2 = off;
INSERT INTO arm SELECT 'PostgreSQL', count(*), sum(s),
       md5(string_agg(k1||':'||k2||'='||s, '|' ORDER BY k1, k2)), 1
FROM (SELECT k1, k2, sum(v) s FROM sd_local GROUP BY k1, k2) q;

SET xp_batch.enabled = on; SET xp_batch.groupagg2 = on;
SET xp_batch.groupagg2_local_partial = off;
INSERT INTO arm SELECT 'xp_batch local_partial=off', count(*), sum(s),
       md5(string_agg(k1||':'||k2||'='||s, '|' ORDER BY k1, k2)), 2
FROM (SELECT k1, k2, sum(v) s FROM sd_local GROUP BY k1, k2) q;

SET xp_batch.groupagg2_local_partial = on;
SET xp_batch.groupagg2_local_hash_cap = 256;
INSERT INTO arm SELECT 'xp_batch local_partial=on', count(*), sum(s),
       md5(string_agg(k1||':'||k2||'='||s, '|' ORDER BY k1, k2)), 3
FROM (SELECT k1, k2, sum(v) s FROM sd_local GROUP BY k1, k2) q;

SELECT '  ' || rpad(name, 28) || 'groups=' || lpad(groups::text, 4)
       || '  sum(v)=' || lpad(total::text, 5) || '  ' || left(ck, 16)
FROM arm ORDER BY ord;

DO \$\$
DECLARE pg_ck text; off_ck text; on_ck text;
BEGIN
    SELECT ck INTO pg_ck  FROM arm WHERE ord = 1;
    SELECT ck INTO off_ck FROM arm WHERE ord = 2;
    SELECT ck INTO on_ck  FROM arm WHERE ord = 3;
    IF off_ck <> pg_ck THEN
        RAISE EXCEPTION 'local_partial=off already disagrees with PostgreSQL -- '
                        'the defect is not isolated to the local hash';
    END IF;
    RAISE NOTICE 'local_partial=off reproduces PostgreSQL exactly (same md5)';
    IF on_ck = pg_ck THEN
        RAISE NOTICE 'local_partial=on also agrees -- NOT reproduced on this build';
    ELSE
        RAISE NOTICE 'local_partial=on diverges: this is the silent drop';
    END IF;
END \$\$;
SQL

echo
echo "=== saturation boundary: output pins at local_cap and never grows ==="
"${PSQL[@]}" <<SQL 2>&1 | grep -E '^NOTICE:  (distinct|  )' | sed 's/^NOTICE:  //'
$PRE
SET xp_batch.enabled = on; SET xp_batch.groupagg2 = on;
SET xp_batch.groupagg2_local_partial = on; SET xp_batch.groupagg2_local_hash_cap = 256;
DO \$\$
DECLARE d int; g bigint; s bigint; tg bigint; ts bigint;
BEGIN
    RAISE NOTICE 'distinct | true_grp | xpb_grp | true_sum | xpb_sum | verdict';
    FOREACH d IN ARRAY ARRAY[100, 200, 254, 255, 256, 257, 300, 500] LOOP
        PERFORM sd_build(d);
        SELECT count(DISTINCT (k1, k2)), sum(v) INTO tg, ts FROM sd_local;
        SELECT count(*), sum(x) INTO g, s
          FROM (SELECT k1, k2, sum(v) x FROM sd_local GROUP BY k1, k2) q;
        RAISE NOTICE '  % | % | % | % | % | %',
              lpad(d::text, 6), lpad(tg::text, 8), lpad(g::text, 7),
              lpad(ts::text, 8), lpad(s::text, 7),
              CASE WHEN g = tg AND s = ts THEN 'ok'
                   ELSE 'LOST ' || (tg - g) || ' groups / ' || (ts - s) || ' rows' END;
    END LOOP;
END \$\$;
SQL

echo
echo "=== it is not an artifact of the 256 minimum: same at the 2048 default ==="
"${PSQL[@]}" <<SQL 2>&1 | grep -E '^NOTICE:  cap' | sed 's/^NOTICE:  /  /'
$PRE
SET xp_batch.enabled = on; SET xp_batch.groupagg2 = on;
SET xp_batch.groupagg2_local_partial = on;
DO \$\$
DECLARE g bigint; tg bigint; dummy bigint;
BEGIN
    PERFORM sd_build(2100);
    SET xp_batch.groupagg2_local_hash_cap = 2048;
    SELECT count(DISTINCT (k1, k2)) INTO tg FROM sd_local;
    -- the outer query must CONSUME the aggregate: with count(*) alone the
    -- planner prunes sum(v) from the subquery tlist, the naggs != 1 gate at
    -- xpb_groupagg2.c:1558 then declines, and the measurement would silently
    -- be stock HashAggregate rather than XpGroupAgg2.
    SELECT count(*), sum(x) INTO g, dummy
      FROM (SELECT k1, k2, sum(v) x FROM sd_local GROUP BY k1, k2) q;
    RAISE NOTICE 'cap=2048  true_groups=%  returned=%  %',
          tg, g, CASE WHEN g = tg THEN 'ok' ELSE 'LOST ' || (tg - g) END;
END \$\$;
SQL

echo
echo "=== failure mode: pure loss, or corrupted survivors? ==="
"${PSQL[@]}" <<SQL 2>&1 | grep -v '^NOTICE'
$PRE
SELECT sd_build(300);
SET xp_batch.enabled = on; SET xp_batch.groupagg2 = on;
SET xp_batch.groupagg2_local_partial = on; SET xp_batch.groupagg2_local_hash_cap = 256;
CREATE TEMP TABLE got AS SELECT k1, k2, sum(v) s FROM sd_local GROUP BY k1, k2;
SET xp_batch.groupagg2 = off; SET xp_batch.enabled = off;
CREATE TEMP TABLE want AS SELECT k1, k2, sum(v) s FROM sd_local GROUP BY k1, k2;
SELECT '  groups returned with a wrong sum : ' || count(*)
  FROM got g JOIN want w USING (k1, k2) WHERE g.s <> w.s;
SELECT '  groups missing entirely          : ' || count(*)
  FROM want w LEFT JOIN got g USING (k1, k2) WHERE g.k1 IS NULL;
SELECT '  groups invented                  : ' || count(*)
  FROM got g LEFT JOIN want w USING (k1, k2) WHERE w.k1 IS NULL;
SQL

echo
echo "=== the backend survives, and nothing is raised ==="
"${PSQL[@]}" <<SQL 2>&1 | grep -v '^NOTICE' | sed 's/^/  /'
$PRE
SELECT sd_build(300);
SET xp_batch.enabled = on; SET xp_batch.groupagg2 = on;
SET xp_batch.groupagg2_local_partial = on; SET xp_batch.groupagg2_local_hash_cap = 256;
SELECT 'query returned ' || count(*) || ' rows summing to ' || sum(x)
       || ', no error raised (true: '
       || (SELECT count(DISTINCT (k1, k2)) FROM sd_local) || ' groups, '
       || (SELECT sum(v) FROM sd_local) || ')'
  FROM (SELECT k1, k2, sum(v) x FROM sd_local GROUP BY k1, k2) q;
SELECT 'backend alive: ' || (pg_backend_pid() > 0);
SQL

echo
echo "############ verdict ############"
"${PSQL[@]}" <<SQL 2>&1 | grep -E '^(R1|R0|  )'
$PRE
SELECT sd_build(300);
SET xp_batch.enabled = on; SET xp_batch.groupagg2 = on;
SET xp_batch.groupagg2_local_partial = on; SET xp_batch.groupagg2_local_hash_cap = 256;
WITH got AS (SELECT count(*) g, sum(x) sx
               FROM (SELECT k1, k2, sum(v) x FROM sd_local GROUP BY k1, k2) q),
     tru AS (SELECT count(DISTINCT (k1, k2)) t, sum(v) ts FROM sd_local)
SELECT CASE WHEN g < t
            THEN 'R1 REACHABLE: ' || (t - g) || ' of ' || t
                 || ' groups lost and ' || (ts - sx)
                 || ' rows unaccounted, with no error, from SQL alone'
            ELSE 'R0 NOT REPRODUCED on this build (returned ' || g
                 || ' groups summing to ' || sx || ')' END
FROM got, tru;
SQL

echo
# Leave nothing behind in the benchmark database.
"${PSQL[@]}" -c "DROP TABLE IF EXISTS sd_local CASCADE;
                 DROP FUNCTION IF EXISTS sd_build(int);" >/dev/null 2>&1

echo
echo "Observability note: LP Max Groups In Batch stays 0 here. The final partial"
echo "flush (xpb_groupagg2.c:745-783) never updates lp_max_groups_in_batch -- only"
echo "the full-batch path at :681-682 does -- so on a single-batch query that"
echo "counter cannot be used to detect saturation. LP Partials Emitted does pin at"
echo "local_cap and is the usable signal."
exit $fail
