#!/bin/bash
#
# R1-13 — XpGroupAgg2 read a ZLFS zone that did not answer the query
#
#   extension/xp_batch/test/reproducers/groupagg2_zlfs_zone_mismatch.sh <PGPORT> [PGHOST] [DB]
#
# xpga2_exec() had a ZLFS fast path that took zz->cols[0], [1] and [2] as the two
# group keys and the summed column, POSITIONALLY. zlfs_lookup_valid_zone() matches
# on relid, on the predicate bounds and on freshness, and on nothing else -- so
# any query over the same range took the zone's third column whatever column it
# asked to sum.
#
# Measured before the fix, on reg_buh with a zone over
# (period_key, company_key, amount_dt) for [25..36] -- the zone benchmark 02
# builds in its own setup:
#
#   sum(amount_dt)   50 004 205 035   = PostgreSQL
#   sum(amount_kt)   50 004 205 035   PostgreSQL: 50 004 441 948
#   sum(payload)     50 004 205 035   PostgreSQL: 500 390 842 330
#
# Right group count, right keys, wrong values, no error. 10x out on payload.
#
# This test builds its OWN fixture rather than using reg_buh: heap_layout_guard.sh
# and silent_drop_map.sh both DROP reg_buh, dim_period and dim_account in whatever
# database they are given, so a reproducer that depended on the benchmark dataset
# would pass or fail depending on suite order. Verified to reproduce the same
# defect on this fixture before the fix.
#
# THE TEST MUST ALSO SHOW THE NODE STILL RUNS. Correctness reached by declining
# the zone for every query would agree with PostgreSQL and prove nothing, which is
# the failure mode this project has hit four times. So the ZLFS path is asserted
# PRESENT for the query the zone answers and ABSENT for the one it does not.
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"

PSQL=("$PGBIN/psql" -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

XPON="LOAD 'xp_batch'; SET xp_batch.enabled=on; SET xp_batch.groupagg2=on;
      SET max_parallel_workers_per_gather=0; SET jit=off;"
OFF="SET max_parallel_workers_per_gather=0; SET jit=off;"
LO=25; HI=36

pass=0; fail=0
ok()   { echo "  ok      $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL    $1"; fail=$((fail+1)); }

PRE=$("${PSQL[@]}" -c "SELECT 'alive'" 2>&1 | tail -1)
[ "$PRE" = "alive" ] || { echo "!! cannot query $DB on port $PORT: $PRE"; exit 2; }
PRE=$("${PSQL[@]}" -c "$XPON SELECT 'loaded'" 2>&1 | tail -1)
[ "$PRE" = "loaded" ] || { echo "!! xp_batch will not load: $PRE"; exit 2; }

echo "=== fixture: r13_fact(k1, k2, v1, v2), 240 000 rows in k1 order ==="
# In k1 order because the node's cost gate wants a correlated leading key, and
# the point here is the zone path, not the gate.
"${PSQL[@]}" -v ON_ERROR_STOP=1 >/dev/null 2>&1 <<SQL || { echo "!! fixture failed"; exit 2; }
DROP TABLE IF EXISTS r13_fact;
CREATE TABLE r13_fact (k1 int NOT NULL, k2 int NOT NULL, v1 int NOT NULL, v2 int NOT NULL);
INSERT INTO r13_fact
SELECT i / 2000 + 1, i % 20, i % 97, i % 31
FROM generate_series(0, 239999) i;
ANALYZE r13_fact;
SQL

# Zone over attnos 1,2,3 = (k1, k2, v1). v2 is attno 4 and is in no zone.
BUILT=$("${PSQL[@]}" -c "$XPON SELECT zlfs_build_zone('r13_fact','1,2,3',$LO,$HI)" 2>&1 | tail -1)
[ "$BUILT" = "VALID" ] || { echo "!! could not build the zone: $BUILT"; exit 2; }

q() {   # the query with the node ON, registry populated in the same backend
    "${PSQL[@]}" 2>&1 <<SQL
$XPON
SELECT count(*) FROM zlfs_zone_info();
SELECT count(*)::text || '|' || sum(s)::text
FROM (SELECT k1, k2, sum($1) s FROM r13_fact
      WHERE k1 BETWEEN $LO AND $HI GROUP BY 1,2) q;
SQL
}

# v1 is the zone's third column, v2 is not in the zone at all.
for col in v1 v2; do
    WANT=$("${PSQL[@]}" -c "$OFF SELECT count(*)::text || '|' || sum(s)::text
            FROM (SELECT k1, k2, sum($col) s FROM r13_fact
                  WHERE k1 BETWEEN $LO AND $HI GROUP BY 1,2) q" 2>&1 | tail -1)
    OUT=$(q "$col")
    GOT=$(echo "$OUT" | grep -E '^[0-9]+\|' | tail -1)
    ZLFS=no; case "$OUT" in *"ZLFS: scan="*) ZLFS=yes ;; esac

    echo "=== sum($col): zone path=$ZLFS ==="
    if [ "$GOT" = "$WANT" ]; then ok "sum($col) agrees with PostgreSQL (= $GOT)"
    else bad "sum($col) — PostgreSQL [$WANT], XpGroupAgg2 [$GOT]"; fi

    if [ "$col" = "v1" ]; then
        [ "$ZLFS" = "yes" ] && ok "the zone path still runs for the query it answers" \
                            || bad "the zone path was declined for its own query — correctness by disabling the node"
    else
        [ "$ZLFS" = "no" ] && ok "the zone path declined a query it has no column for" \
                           || bad "the zone path ran for sum($col), which is not in the zone"
    fi
done

# The same three lines also assumed int4 and non-nullable columns. This case shows
# an int8 nullable column does not reach the zone path -- but it passed before the
# fix too, so what it demonstrates is that the node's shape gate declines
# sum(bigint) EARLIER, not that the zone's type and validity checks work. Kept as
# the boundary it is, labelled as such.
#
# NOT COVERED BY A TEST, therefore: the col_types != ZLFS_COL_INT4 and
# col_validity != NULL arms of the guard. Reaching them needs a zone whose third
# column is int8 or nullable while the query's aggregate is int4, i.e. a zone built
# over different columns than the query sums -- which the attno check already
# declines first. They are code-level only, and they are there because the attno
# check could be loosened later and these are the assumptions underneath it.
echo "=== an int8 nullable column: declined, but earlier than the zone check ==="
"${PSQL[@]}" -v ON_ERROR_STOP=1 >/dev/null 2>&1 <<SQL
DROP TABLE IF EXISTS r13_wide;
CREATE TABLE r13_wide (k1 int NOT NULL, k2 int NOT NULL, v1 bigint);
INSERT INTO r13_wide
SELECT i / 2000 + 1, i % 20, CASE WHEN i % 1000 = 0 THEN NULL ELSE (i % 97)::bigint END
FROM generate_series(0, 239999) i;
ANALYZE r13_wide;
SQL
WB=$("${PSQL[@]}" -c "$XPON SELECT zlfs_build_zone('r13_wide','1,2,3',$LO,$HI)" 2>&1 | tail -1)
if [ "$WB" = "VALID" ]; then
    WANT=$("${PSQL[@]}" -c "$OFF SELECT count(*)::text || '|' || coalesce(sum(s),0)::text
            FROM (SELECT k1, k2, sum(v1) s FROM r13_wide
                  WHERE k1 BETWEEN $LO AND $HI GROUP BY 1,2) q" 2>&1 | tail -1)
    OUT=$("${PSQL[@]}" 2>&1 <<SQL
$XPON
SELECT count(*) FROM zlfs_zone_info();
SELECT count(*)::text || '|' || coalesce(sum(s),0)::text
FROM (SELECT k1, k2, sum(v1) s FROM r13_wide
      WHERE k1 BETWEEN $LO AND $HI GROUP BY 1,2) q;
SQL
)
    GOT=$(echo "$OUT" | grep -E '^[0-9]+\|' | tail -1)
    case "$OUT" in *"ZLFS: scan="*) bad "the zone path ran on an int8 nullable column" ;;
        *) ok "an int8 nullable column never reaches the zone path (shape gate, not the zone check)" ;; esac
    [ "$GOT" = "$WANT" ] && ok "and the answer agrees with PostgreSQL (= $GOT)" \
                         || bad "answer differs — PostgreSQL [$WANT], XpGroupAgg2 [$GOT]"
else
    echo "  skip    int8 zone — zlfs_build_zone declined the column: $WB"
fi

"${PSQL[@]}" -c "DROP TABLE IF EXISTS r13_fact, r13_wide" >/dev/null 2>&1

echo
echo "############ $pass correct, $fail wrong ############"
[ "$fail" -eq 0 ]
