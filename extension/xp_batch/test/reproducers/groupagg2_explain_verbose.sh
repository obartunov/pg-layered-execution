#!/bin/bash
#
# EXPLAIN (VERBOSE) on XpGroupAgg2 must terminate.
#
#   extension/xp_batch/test/reproducers/groupagg2_explain_verbose.sh <PGPORT> [PGHOST] [DB]
#
# Guards the self-referential target list fixed with R1-11.
#
# xpga2_plan_path() set scan.plan.targetlist AND custom_scan_tlist to the same
# hand-built list of Var(INDEX_VAR, i), so entry i of custom_scan_tlist was a
# reference to itself. Execution never noticed -- its only consumers read
# exprType() -- but EXPLAIN (VERBOSE) deparses the target list, and resolving
# INDEX_VAR means following it into custom_scan_tlist, which pointed back.
#
# The resulting loop could not be interrupted:
#
#   SET statement_timeout did not fire
#   pg_terminate_backend() did not end it
#   only pg_ctl restart -m immediate cleared it
#
# and it held a relation lock while wedged, so later DROP TABLEs queued behind
# it and the whole test suite queued behind those. Two were observed, at 39 and
# 9 minutes, on a plan whose non-VERBOSE form returns in 0.04 ms.
#
# EVERY psql call here is wrapped in `timeout`, at the CLIENT, deliberately.
# statement_timeout cannot be relied on to end this failure mode, so a
# regression must be caught from outside the backend or it will hang the suite
# that is supposed to detect it.
set -uo pipefail

PORT="${1:-5432}"; PGHOST_ARG="${2:-}"; DB="${3:-sdaudit}"
PSQL=(psql -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

LIMIT=45          # seconds; the correct forms take well under one

if ! timeout 20 "${PSQL[@]}" -c 'SELECT 1' >/dev/null 2>&1; then
    echo "cannot connect" >&2; exit 2
fi

pass=0; fail=0
GUC="SET jit=off; SET max_parallel_workers_per_gather=0; SET statement_timeout='20s';"
XPON="LOAD 'xp_batch'; SET xp_batch.enabled=on; SET xp_batch.groupagg2=on;"

echo "############ XpGroupAgg2 EXPLAIN VERBOSE termination ############"
echo

timeout 120 "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
SET client_min_messages=warning;
DROP TABLE IF EXISTS ev_t;
CREATE TABLE ev_t (k1 int4 NOT NULL, k2 int4 NOT NULL, v int8 NOT NULL);
INSERT INTO ev_t SELECT k1,0,1 FROM (SELECT (g/200)::int4 AS k1 FROM generate_series(0,199999) g) s ORDER BY k1;
ANALYZE ev_t;
SQL

# probe <label> <explain-options>
probe() {
    local label="$1" opts="$2" out rc
    out=$(timeout "$LIMIT" "${PSQL[@]}" -c "SET client_min_messages=warning; $GUC $XPON
          EXPLAIN ($opts) SELECT k1,k2,sum(v) FROM ev_t WHERE k1 <= 500 GROUP BY k1,k2" 2>&1)
    rc=$?
    if [ "$rc" -eq 124 ]; then
        echo "  HUNG    $label — no result in ${LIMIT}s (client timeout; the backend"
        echo "                  may still be wedged and holding locks: check"
        echo "                  pg_stat_activity and restart with -m immediate)"
        fail=$((fail+1)); return
    fi
    if grep -q '^ERROR' <<<"$out"; then
        echo "  ERROR   $label — $(grep -m1 '^ERROR' <<<"$out")"; fail=$((fail+1)); return
    fi
    if ! grep -q 'Custom Scan (XpGroupAgg2)' <<<"$out"; then
        echo "  BROKEN  $label — XpGroupAgg2 not in the plan, so nothing was tested"
        fail=$((fail+1)); return
    fi
    # VERBOSE must actually produce a deparsed Output line: that is the thing
    # that used to loop. Its absence would mean the test passed vacuously.
    if ! grep -q 'Output:' <<<"$out"; then
        echo "  BROKEN  $label — no Output: line, VERBOSE deparse did not happen"
        fail=$((fail+1)); return
    fi
    echo "  ok      $label — $(grep -m1 'Output:' <<<"$out" | sed 's/^ *//')"
    pass=$((pass+1))
}

probe "EXPLAIN (VERBOSE)          " "VERBOSE, COSTS OFF"
probe "EXPLAIN (ANALYZE, VERBOSE) " "ANALYZE, VERBOSE, COSTS OFF, TIMING OFF"

# The node must still produce the right answer with the corrected tlist.
want=$(timeout "$LIMIT" "${PSQL[@]}" -c "SET client_min_messages=warning; $GUC
        LOAD 'xp_batch'; SET xp_batch.enabled=off;
        SELECT count(*) || '|' || sum(s) FROM (SELECT k1,k2,sum(v) AS s FROM ev_t WHERE k1<=500 GROUP BY k1,k2) a" 2>&1 | tail -1)
got=$(timeout "$LIMIT" "${PSQL[@]}" -c "SET client_min_messages=warning; $GUC $XPON
        SELECT count(*) || '|' || sum(s) FROM (SELECT k1,k2,sum(v) AS s FROM ev_t WHERE k1<=500 GROUP BY k1,k2) a" 2>&1 | tail -1)
if [ "$want" = "$got" ]; then echo "  ok      result unchanged by the tlist fix (= $got)"; pass=$((pass+1))
else echo "  WRONG   result — want $want, got $got"; fail=$((fail+1)); fi

timeout 30 "${PSQL[@]}" -c "DROP TABLE IF EXISTS ev_t" >/dev/null 2>&1

echo
echo "############ $pass correct, $fail wrong ############"
exit $(( fail > 0 ? 1 : 0 ))
