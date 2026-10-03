#!/bin/bash
#
# Shapes 4 and 6 -- group aggregate, and the same aggregate at higher group
# cardinality.
#
#   benchmarks/perf_v1/groupagg.sh <PGPORT> [PGHOST] [DB]
#
# WHY THIS IS A SEPARATE FILE WITH FEWER ARMS
#
# There is no entry point in this tree that drives a plain group-aggregate
# across every representation. xpb_batch_join2_groupby reaches all five, but it
# is a join+aggregate and its grouping is fixed at (year, account_group,
# company_key). xpb_batch_groupby reaches heap and zlfs only -- it predates the
# source registry and dispatches on a mode string of its own. xpq_scan is
# Parquet-only and aggregates nothing.
#
# So the comparable arms for these two shapes are:
#
#   heap    SQL over reg_buh        and  xpb_batch_groupby(..,'heap')
#   zlfs                                 xpb_batch_groupby(..,'zlfs')
#   pgcol   SQL over reg_buh_col
#
# Parquet has no arm for either shape. That hole is the measurement, not a gap
# to be filled by comparing something else and calling it Parquet.
#
# Cardinality comes from the GROUPING, on one dataset, so nothing else moves:
#   low    GROUP BY period_key                           -> 120 groups
#   mid    GROUP BY period_key, company_key              -> 6 000 groups
#   high   GROUP BY period_key, company_key, account_key -> measured below
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-/tmp/xpb_sock}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
COMMIT="$(cd "$REPO" && git rev-parse --short HEAD)"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RUNDIR="$HERE/raw/$STAMP-$COMMIT-groupagg"
mkdir -p "$RUNDIR"
CSV="$HERE/summary/$STAMP-$COMMIT-groupagg.csv"
PSQL=("$PGBIN/psql" -p "$PORT" -h "$PGHOST_ARG" -d "$DB" -X -q)
NWARM="${NWARM:-5}"

echo "arm,shape,state,run,wall_ms,total_ms,source_ms,operators_ms,rows,notes" > "$CSV"
echo "############ group aggregate, by group cardinality ############"
echo "raw: $RUNDIR"
echo

# ---- the oracle, and the group counts, first ----
echo "=== cardinality and oracle (PostgreSQL over the heap) ==="
declare -A ORACLE
for g in "low:period_key" "mid:period_key,company_key" "high:period_key,company_key,account_key"; do
    lbl="${g%%:*}"; cols="${g#*:}"
    o=$("${PSQL[@]}" -At -c "
        SELECT count(*)||'|'||sum(s) FROM
          (SELECT $cols, sum(amount_dt)::bigint AS s FROM reg_buh GROUP BY $cols) q")
    ORACLE[$lbl]="$o"
    echo "  $lbl  GROUP BY $cols  ->  groups|checksum = $o"
done

# Every arm is gated against that before any timing is kept.
gate() {            # gate <label> <sql> <armname>
    local lbl="$1" sql="$2" arm="$3"
    local got
    got=$("${PSQL[@]}" -At -c "SET client_min_messages=warning; $sql" 2>&1 | tail -1)
    if [ "$got" = "${ORACLE[$lbl]}" ]; then
        echo "  ok      $arm [$lbl] matches the oracle ($got)"; return 0
    fi
    echo "  FAIL    $arm [$lbl] oracle=${ORACLE[$lbl]} arm=$got"; return 1
}

# Time a statement NWARM times in one backend after a warm-up, from psql's own
# \timing. Nothing here reads a module counter, because plain SQL has none --
# so source/operator columns stay EMPTY rather than being invented.
timeit() {          # timeit <arm> <shape> <sql> [notes]
    local arm="$1" shape="$2" sql="$3" notes="${4:-}"
    local out="$RUNDIR/$shape-$arm.txt"
    { echo "# arm=$arm shape=$shape state=warm runs=$NWARM (after 1 warm-up) commit=$COMMIT"
      echo "# sql: $sql"; echo; } > "$out"
    { echo "\\timing on"; echo "SET client_min_messages=notice;"; echo "LOAD 'xpb_parquet';"
      for ((r=0;r<=NWARM;r++)); do echo "$sql"; done; } > "$RUNDIR/.q.sql"
    "${PSQL[@]}" -f "$RUNDIR/.q.sql" > "$RUNDIR/.o" 2>&1
    cat "$RUNDIR/.o" >> "$out"
    local walls=""
    for ((r=1;r<=NWARM;r++)); do
        local w st tot src
        w=$(grep -oP '^Time: \K[0-9.]+' "$RUNDIR/.o" | sed -n "$((3+r))p")
        st=$(grep -oP 'total=\K.*' "$RUNDIR/.o" | sed -n "$((r+1))p")
        tot=$(echo "${st:-}" | grep -oP '^\K[0-9.]+')
        src=$(echo "${st:-}" | grep -oP '(^|\s)(source|scan)=\K[0-9.]+' | head -1)
        printf '%s,%s,warm,%s,%s,%s,%s,,,"%s"\n' \
            "$arm" "$shape" "$r" "${w:-}" "${tot:-}" "${src:-}" "$notes" >> "$CSV"
        walls="$walls ${w:-?}"
    done
    printf '  %-22s %-6s warm wall%s ms\n' "$arm" "$shape" "$walls"
}

echo
echo "=== correctness gate ==="
fail=0
gate low  "SELECT count(*)||'|'||sum(s) FROM (SELECT period_key, sum(amount_dt)::bigint s FROM reg_buh_col GROUP BY 1) q" pgcolumnar-sql || fail=$((fail+1))
gate mid  "SELECT count(*)||'|'||sum(s) FROM (SELECT period_key, company_key, sum(amount_dt)::bigint s FROM reg_buh_col GROUP BY 1,2) q" pgcolumnar-sql || fail=$((fail+1))
gate high "SELECT count(*)||'|'||sum(s) FROM (SELECT period_key, company_key, account_key, sum(amount_dt)::bigint s FROM reg_buh_col GROUP BY 1,2,3) q" pgcolumnar-sql || fail=$((fail+1))
# xpb_batch_groupby groups by (period_key, company_key) over the full range:
# the mid shape, and the only one it can express.
gate mid "SELECT count(*)||'|'||sum(total_amt) FROM xpb_batch_groupby(1,120,'heap')" heap-xpb || fail=$((fail+1))
gate mid "SELECT count(*)||'|'||sum(total_amt) FROM xpb_batch_groupby(1,120,'zlfs')" zlfs-xpb || fail=$((fail+1))
if [ "$fail" -ne 0 ]; then
    echo; echo "!! $fail arm(s) failed the oracle; no timings recorded"; exit 1
fi

echo
echo "=== timings ==="
timeit heap-sql        low  "SELECT count(*) FROM (SELECT period_key, sum(amount_dt) FROM reg_buh GROUP BY 1) q;"
timeit pgcolumnar-sql  low  "SELECT count(*) FROM (SELECT period_key, sum(amount_dt) FROM reg_buh_col GROUP BY 1) q;"
timeit heap-sql        mid  "SELECT count(*) FROM (SELECT period_key, company_key, sum(amount_dt) FROM reg_buh GROUP BY 1,2) q;"
timeit pgcolumnar-sql  mid  "SELECT count(*) FROM (SELECT period_key, company_key, sum(amount_dt) FROM reg_buh_col GROUP BY 1,2) q;"
timeit heap-sql        high "SELECT count(*) FROM (SELECT period_key, company_key, account_key, sum(amount_dt) FROM reg_buh GROUP BY 1,2,3) q;"
timeit pgcolumnar-sql  high "SELECT count(*) FROM (SELECT period_key, company_key, account_key, sum(amount_dt) FROM reg_buh_col GROUP BY 1,2,3) q;"
timeit heap-xpb        mid  "SELECT count(*) FROM xpb_batch_groupby(1,120,'heap');" "xpb_batch_groupby, fixed grouping"
timeit zlfs-xpb        mid  "SELECT count(*) FROM xpb_batch_groupby(1,120,'zlfs');" "xpb_batch_groupby, fixed grouping"

rm -f "$RUNDIR/.q.sql" "$RUNDIR/.o"
echo
echo "summary csv: $CSV"
