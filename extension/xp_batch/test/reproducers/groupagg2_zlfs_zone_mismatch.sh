#!/bin/bash
#
# R1-13 — XpGroupAgg2 read a ZLFS zone that did not answer the query
#
#   extension/xp_batch/test/reproducers/groupagg2_zlfs_zone_mismatch.sh <PGPORT> [PGHOST] [DB]
#
# xpga2_exec() had a ZLFS fast path that took zz->cols[0], [1] and [2] as the two
# group keys and the summed column, POSITIONALLY. zlfs_lookup_valid_zone() matches
# on relid and on the predicate bounds and nothing else, so any query over the
# same range took the zone's third column whatever column it asked to sum.
#
# Measured before the fix, zone over (period_key, company_key, amount_dt) on
# reg_buh [25..36] -- the shape benchmark 02 builds:
#
#   sum(amount_dt)   50 004 205 035   = PostgreSQL
#   sum(amount_kt)   50 004 205 035   PostgreSQL: 50 004 441 948
#   sum(payload)     50 004 205 035   PostgreSQL: 500 390 842 330
#
# Right group count, right keys, wrong values, no error. 10x out on payload.
#
# THE TEST MUST ALSO SHOW THE NODE STILL RUNS. Correctness reached by declining
# the zone for every query would pass a comparison against PostgreSQL and prove
# nothing, which is the failure mode this project has hit four times. So the
# ZLFS path is asserted PRESENT for the query the zone does answer and ABSENT for
# the ones it does not, by looking for its NOTICE.
#
# Reachability, for the record: both GUCs default to off, a zone must exist with
# bounds exactly matching the query's range predicate, and the zone registry must
# have been populated in the same backend (any zlfs_* call does it). None of that
# makes it unreachable -- benchmark 02 builds exactly this zone.
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"

PSQL=("$PGBIN/psql" -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

XPON="LOAD 'xp_batch'; SET xp_batch.enabled=on; SET xp_batch.groupagg2=on;
      SET max_parallel_workers_per_gather=0; SET jit=off;"
OFF="SET max_parallel_workers_per_gather=0; SET jit=off;"

pass=0; fail=0
ok()   { echo "  ok      $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL    $1"; fail=$((fail+1)); }

PRE=$("${PSQL[@]}" -c "SELECT 'alive'" 2>&1 | tail -1)
[ "$PRE" = "alive" ] || { echo "!! cannot query $DB on port $PORT: $PRE"; exit 2; }
PRE=$("${PSQL[@]}" -c "$XPON SELECT to_regclass('reg_buh') IS NOT NULL" 2>&1 | tail -1)
[ "$PRE" = "t" ] || { echo "!! reg_buh absent or xp_batch will not load: $PRE"; exit 2; }

q() {   # one query with the node ON, in one backend, with the registry populated
    "${PSQL[@]}" 2>&1 <<SQL
$XPON
SELECT count(*) FROM zlfs_zone_info();
SELECT count(*)::text || '|' || sum(s)::text
FROM (SELECT period_key, company_key, sum($1) s FROM reg_buh
      WHERE period_key BETWEEN 25 AND 36 GROUP BY 1,2) q;
SQL
}

echo "=== setup: zone over (period_key, company_key, amount_dt) for [25..36] ==="
BUILT=$("${PSQL[@]}" -c "$XPON SELECT zlfs_build_zone('reg_buh','1,2,6',25,36)" 2>&1 | tail -1)
[ "$BUILT" = "VALID" ] || { echo "!! could not build the zone: $BUILT"; exit 2; }

for col in amount_dt amount_kt payload; do
    WANT=$("${PSQL[@]}" -c "$OFF SELECT count(*)::text || '|' || sum(s)::text
            FROM (SELECT period_key, company_key, sum($col) s FROM reg_buh
                  WHERE period_key BETWEEN 25 AND 36 GROUP BY 1,2) q" 2>&1 | tail -1)
    OUT=$(q "$col")
    GOT=$(echo "$OUT" | grep -E '^[0-9]+\|' | tail -1)
    ZLFS=no; case "$OUT" in *"ZLFS: scan="*) ZLFS=yes ;; esac

    echo "=== sum($col): zone path=$ZLFS ==="
    if [ "$GOT" = "$WANT" ]; then ok "sum($col) agrees with PostgreSQL (= $GOT)"
    else bad "sum($col) — PostgreSQL [$WANT], XpGroupAgg2 [$GOT]"; fi

    # The zone's third column is amount_dt, so that is the one query it answers.
    if [ "$col" = "amount_dt" ]; then
        [ "$ZLFS" = "yes" ] && ok "the zone path still runs for the query it answers" \
                            || bad "the zone path was declined for its own query — correctness by disabling the node"
    else
        [ "$ZLFS" = "no" ] && ok "the zone path declined a query it cannot answer" \
                           || bad "the zone path ran for sum($col), which it has no column for"
    fi
done

echo
echo "############ $pass correct, $fail wrong ############"
[ "$fail" -eq 0 ]
