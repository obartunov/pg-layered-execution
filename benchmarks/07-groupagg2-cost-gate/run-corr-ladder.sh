#!/bin/bash
#
# Benchmark 07 — XpGroupAgg2 cost-gate audit
#
#   benchmarks/07-groupagg2-cost-gate/run-corr-ladder.sh <PGPORT> [PGHOST] [DBNAME]
#
# The symptom recorded in docs/COST_OF_CORRECT_LAYERED_EXECUTION.md: the same
# query over 1000 groups in 2,000,000 rows was accepted for `k1 = g/2000` and
# refused for `k1 = g%1000`, and that was attributed to key correlation.
#
# That attribution was not isolated. The two tables differed in TWO ways: the
# key expression, and the `tag` column ('keep'/'skip', 4 bytes, against 'x',
# 1 byte) -- which changes tuple width, hence relpages, which IS an input to
# the Xp cost floor:
#
#   C1_scan = relpages * seq_page_cost * 0.95 + relrows * cpu_tuple_cost * 0.85
#
# This ladder varies correlation ALONE. Every variant holds:
#
#   same row count          2,000,000
#   same k1 value multiset  0..999, each exactly 2000 times
#   same NDV                1000
#   same tag content        a function of k1, so byte-identical per row
#   same column types and order
#
# so relpages, the k1 histogram, the selectivity of `k1 <= 500` and
# estimate_num_groups() are all held fixed by construction. Only the physical
# sequence of rows changes, via ORDER BY on the INSERT.
#
# Nothing is tuned. The cost gate is not touched. No hints, no forcing in the
# decision measurements.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-5432}"; PGHOST_ARG="${2:-}"; DB="${3:-onec2}"
RAW="$HERE/raw"; mkdir -p "$RAW"

PSQL=(psql -p "$PORT" -X -qAt -d "$DB")
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

GUC="SET jit=off; SET max_parallel_workers_per_gather=0; SET work_mem='64MB';"
XPON="LOAD 'xp_batch'; SET xp_batch.enabled=on; SET xp_batch.groupagg2=on;"
Q="SELECT k1,k2,sum(v) FROM cg_t WHERE k1 <= 500 GROUP BY k1,k2"

# order_expr for each rung. The value multiset is identical in all of them.
rung_order() {
    case "$1" in
        asc)      echo "k1" ;;
        desc)     echo "k1 DESC" ;;
        blocks10) echo "(k1 % 10), k1" ;;       # 10 interleaved ascending runs
        blocks100) echo "(k1 % 100), k1" ;;     # 100 interleaved ascending runs
        jitter)   echo "k1 + (random()*200 - 100)" ;;
        random)   echo "random()" ;;
    esac
}

: > "$RAW/ladder.txt"
echo "############ Benchmark 07 — correlation ladder, correlation varied ALONE ############"
echo
"${PSQL[@]}" -c "SELECT 'server: ' || version()" | cut -c1-58
echo
printf '%-10s %12s %10s %9s %9s %9s  %-28s %10s %10s\n' \
       rung correlation n_distinct relpages est_rows est_grps 'chosen path' xp_cost win_cost

for rung in asc blocks100 blocks10 jitter random desc; do
    ord=$(rung_order "$rung")

    "${PSQL[@]}" >/dev/null 2>&1 <<SQL
SET client_min_messages=warning;
DROP TABLE IF EXISTS cg_t;
CREATE TABLE cg_t (k1 int4 NOT NULL, k2 int4 NOT NULL, v int8 NOT NULL, tag text);
-- tag is a function of k1 only, so every rung stores byte-identical rows and
-- therefore the same relpages. Only the ORDER BY differs.
INSERT INTO cg_t
SELECT k1, 0, 1, CASE WHEN k1 % 10 = 0 THEN 'keep' ELSE 'skip' END
FROM (SELECT (g / 2000)::int4 AS k1 FROM generate_series(0,1999999) g) s
ORDER BY $ord;
ANALYZE cg_t;
SQL

    read -r corr ndv pages rows <<<"$("${PSQL[@]}" -c "
        SELECT round(s.correlation::numeric,4) || ' ' || s.n_distinct || ' ' ||
               c.relpages || ' ' || c.reltuples::bigint
        FROM pg_stats s JOIN pg_class c ON c.relname = s.tablename
        WHERE s.tablename='cg_t' AND s.attname='k1'")"

    # Planner estimates and the Xp path's own cost, from EXPLAIN with costs.
    plan=$("${PSQL[@]}" -c "$GUC $XPON EXPLAIN $Q" 2>&1)
    node=$(sed -n '1p' <<<"$plan" | sed 's/^ *//;s/  *(cost.*//')
    win_cost=$(sed -n '1p' <<<"$plan" | grep -oE 'cost=[0-9.]+\.\.[0-9.]+' | head -1 | sed 's/.*\.\.//')
    est_rows=$(sed -n '1p' <<<"$plan" | grep -oE 'rows=[0-9]+' | head -1 | cut -d= -f2)

    # The Xp path's cost whether or not it won: DEBUG2 prints the floor
    # breakdown every time xpga2_add_path runs.
    dbg=$("${PSQL[@]}" -c "$GUC $XPON SET client_min_messages=debug2; EXPLAIN $Q" 2>&1 \
          | grep -oE 'xpga2 cost: .*' | head -1)
    xp_cost=$(grep -oE 'total=[0-9]+' <<<"$dbg" | head -1 | cut -d= -f2)
    xp_grps=$(grep -oE 'ngroups=[0-9]+' <<<"$dbg" | head -1 | cut -d= -f2)
    xp_rows=$(grep -oE 'nrows=[0-9]+' <<<"$dbg" | head -1 | cut -d= -f2)

    printf '%-10s %12s %10s %9s %9s %9s  %-28s %10s %10s\n' \
           "$rung" "$corr" "$ndv" "$pages" "${xp_rows:--}" "${xp_grps:--}" \
           "${node:0:28}" "${xp_cost:--}" "${win_cost:--}"
    printf '%s|%s|%s|%s|%s|%s|%s|%s|%s\n' \
           "$rung" "$corr" "$ndv" "$pages" "${xp_rows:--}" "${xp_grps:--}" \
           "$node" "${xp_cost:--}" "${win_cost:--}" >> "$RAW/ladder.txt"
    echo "$rung :: $dbg" >> "$RAW/debug-floor.txt"
    { echo "=== rung $rung (correlation $corr) ==="; echo "$plan"; echo; } >> "$RAW/plans.txt"
done

echo
echo "raw: $RAW/ladder.txt  $RAW/debug-floor.txt  $RAW/plans.txt"
