#!/bin/bash
#
# XpGroupAgg2 returns zero rows on descending-correlated data with an
# upper-bounded predicate on the leading key.
#
#   extension/xp_batch/test/reproducers/groupagg2_desc_stream_exit.sh <PGPORT> [PGHOST] [DB]
#
# Found while auditing the correlation-dependent cost gate (benchmark 07).
#
# Mechanism, from the node's own EXPLAIN counters:
#
#   Pages Total: 1082   Pages Skipped (STREAM exit): 1081   Pages Scanned: 2
#   Tuples Visited: 0   order_scope: STREAM
#
#   1. The applicability gate accepts the path on `fabs(correlation) >= 0.8`
#      (xpb_groupagg2.c, "STREAM feasibility gate"), so correlation -1.0 passes.
#   2. The node's STREAM contract requires (k1,k2) NON-DECREASING
#      (xpb_groupagg2.c header, order_scope = STREAM).
#   3. The Class-1 early exit stops the scan when a page's leading key exceeds
#      stream_key_hi. That rule is only valid for non-decreasing order.
#   4. On descending data the FIRST page holds the maximum key, so the scan
#      terminates before visiting a single tuple.
#   5. Runtime STREAM detection then reports order_scope=STREAM having seen zero
#      tuples, so nothing contradicts it.
#
# No error is raised. The result is silently empty.
#
# Scope: only predicates carrying an UPPER bound on the leading key are
# affected. Lower-bound-only predicates are correct, because prefix-skip is a
# per-page decision while early-exit terminates the whole scan.
#
# A note on why this took a while to see: when the inner aggregate's value is
# not consumed, the planner removes it from the subquery's target list, the
# node's "exactly one Aggref" guard then declines, and PostgreSQL answers
# correctly. So `SELECT count(*) FROM (SELECT k1,k2,sum(v) ...) q` passes while
# `SELECT count(*), sum(s) FROM (SELECT k1,k2,sum(v) AS s ...) q` fails. Any
# check of this node must consume the aggregate.
set -uo pipefail

PORT="${1:-5432}"; PGHOST_ARG="${2:-}"; DB="${3:-sdaudit}"
PSQL=(psql -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

if ! "${PSQL[@]}" -c 'SELECT 1' >/dev/null 2>&1; then
    echo "cannot connect: $("${PSQL[@]}" -c 'SELECT 1' 2>&1 | head -1)" >&2
    echo "usage: $0 <PGPORT> [PGHOST] [DB]" >&2
    exit 2
fi

pass=0; fail=0
check() {   # check <label> <want> <got>   -- both are "<rows>|<sum>"
    case "$2" in
        ''|*[!0-9'|'null]*) echo "  BROKEN  $1 — malformed reference [$2]"; fail=$((fail+1)); return ;;
    esac
    if [ "$2" = "$3" ]; then echo "  ok      $1 (= $3)"; pass=$((pass+1))
    else echo "  WRONG   $1 — want $2, got $3"; fail=$((fail+1)); fi
}

echo "############ XpGroupAgg2 descending-order STREAM early exit ############"
echo
echo "1000 groups, 200 rows each, 200 000 rows. Two tables, identical contents,"
echo "differing only in physical row order."
echo

"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
SET client_min_messages=warning;
DROP TABLE IF EXISTS ga_asc, ga_desc;
CREATE TABLE ga_asc  (k1 int4 NOT NULL, k2 int4 NOT NULL, v int8 NOT NULL);
CREATE TABLE ga_desc (k1 int4 NOT NULL, k2 int4 NOT NULL, v int8 NOT NULL);
INSERT INTO ga_asc  SELECT k1,0,1 FROM (SELECT (g/200)::int4 AS k1 FROM generate_series(0,199999) g) s ORDER BY k1;
INSERT INTO ga_desc SELECT k1,0,1 FROM (SELECT (g/200)::int4 AS k1 FROM generate_series(0,199999) g) s ORDER BY k1 DESC;
ANALYZE ga_asc; ANALYZE ga_desc;
SQL

echo "--- pg_stats correlation on the leading key ---"
"${PSQL[@]}" -c "SELECT '  ' || tablename || ': correlation=' || round(correlation::numeric,4)
                 FROM pg_stats WHERE tablename IN ('ga_asc','ga_desc') AND attname='k1' ORDER BY 1"
echo

GUC="SET jit=off; SET max_parallel_workers_per_gather=0;"
XPON="LOAD 'xp_batch'; SET xp_batch.enabled=on; SET xp_batch.groupagg2=on;"
XPOFF="LOAD 'xp_batch'; SET xp_batch.enabled=off;"

# run <table> <predicate> <setting>  ->  "<rows>|<sum of group sums>"
#
# sum(s) is CONSUMED deliberately. With only count(*) outside, the planner
# removes the unused inner aggregate, the node's one-Aggref guard declines, and
# PostgreSQL answers -- so the check would pass without ever running the node.
# This script did exactly that before this comment existed.
run() {
    "${PSQL[@]}" -c "SET client_min_messages=warning; $GUC $3
        SELECT count(*) || '|' || coalesce(sum(s)::text,'null')
        FROM (SELECT k1,k2,sum(v) AS s FROM $1 WHERE $2 GROUP BY k1,k2) a" 2>&1 | tail -1
}

for tbl in ga_asc ga_desc; do
    echo "=== $tbl ==="
    for pred in "k1 <= 500" "k1 BETWEEN 200 AND 300" "k1 >= 500" "k1 >= 0"; do
        want=$(run "$tbl" "$pred" "$XPOFF")
        got=$(run  "$tbl" "$pred" "$XPON")
        label="$(printf '%-24s' "$pred")"
        if [ "$tbl" = ga_desc ] && { [ "$pred" = "k1 <= 500" ] || [ "$pred" = "k1 BETWEEN 200 AND 300" ]; }; then
            label="$label (was 0|null of $want)"
        fi
        check "$label" "$want" "$got"
    done
    echo
done

echo "=== exact-sorted control (j=0) and near-sorted, admitted with no forcing ==="
echo "The gate's test is |correlation| >= 0.8. The early exit needs MONOTONIC."
echo "Those are not the same condition, and the gap loses rows:"
echo
for j in 0 20 100 400; do   # j=0 is the exact-sorted control
    "${PSQL[@]}" >/dev/null 2>&1 <<SQL
SET client_min_messages=warning;
DROP TABLE IF EXISTS ga_j;
CREATE TABLE ga_j (k1 int4 NOT NULL, k2 int4 NOT NULL, v int8 NOT NULL);
INSERT INTO ga_j SELECT k1,0,1 FROM (SELECT (g/200)::int4 AS k1 FROM generate_series(0,199999) g) s
  ORDER BY k1 + (random()*$j - $j/2.0);
ANALYZE ga_j;
SQL
    corr=$("${PSQL[@]}" -c "SELECT round(correlation::numeric,4) FROM pg_stats WHERE tablename='ga_j' AND attname='k1'")
    want=$(run ga_j "k1 <= 500" "$XPOFF")
    got=$(run  ga_j "k1 <= 500" "$XPON")
    check "$(printf 'jitter=%-4s correlation=%-8s' "$j" "$corr")" "$want" "$got"
done
"${PSQL[@]}" -c "DROP TABLE IF EXISTS ga_j" >/dev/null 2>&1
echo

echo "=== the node must still RUN (correctness must not come from declining) ==="
echo "A repair that silently stopped selecting XpGroupAgg2 would make every"
echo "comparison above green while delivering nothing. Assert the plan."
echo
node_used() {   # node_used <table> <predicate>
    "${PSQL[@]}" -c "SET client_min_messages=warning; $GUC $XPON
        EXPLAIN (COSTS OFF) SELECT k1,k2,sum(v) FROM $1 WHERE $2 GROUP BY k1,k2" 2>&1 \
      | grep -c 'Custom Scan (XpGroupAgg2)'
}
"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
SET client_min_messages=warning;
DROP TABLE IF EXISTS ga_asc, ga_desc;
CREATE TABLE ga_asc  (k1 int4 NOT NULL, k2 int4 NOT NULL, v int8 NOT NULL);
CREATE TABLE ga_desc (k1 int4 NOT NULL, k2 int4 NOT NULL, v int8 NOT NULL);
INSERT INTO ga_asc  SELECT k1,0,1 FROM (SELECT (g/200)::int4 AS k1 FROM generate_series(0,199999) g) s ORDER BY k1;
INSERT INTO ga_desc SELECT k1,0,1 FROM (SELECT (g/200)::int4 AS k1 FROM generate_series(0,199999) g) s ORDER BY k1 DESC;
ANALYZE ga_asc; ANALYZE ga_desc;
SQL
check "XpGroupAgg2 chosen, ascending  " "1" "$(node_used ga_asc  'k1 <= 500')"
check "XpGroupAgg2 chosen, descending " "1" "$(node_used ga_desc 'k1 <= 500')"
echo

echo "--- the node's own counters on the failing case ---"
"${PSQL[@]}" -c "SET client_min_messages=warning; $GUC $XPON
    EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF)
    SELECT k1,k2,sum(v) FROM ga_desc WHERE k1 <= 500 GROUP BY k1,k2" 2>&1 \
  | grep -E "actual rows|order_scope|Pages|Tuples Visited" | sed 's/^/  /'

"${PSQL[@]}" -c "DROP TABLE IF EXISTS ga_asc, ga_desc" >/dev/null 2>&1

echo
echo "############ $pass correct, $fail wrong ############"
exit $(( fail > 0 ? 1 : 0 ))
