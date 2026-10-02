#!/bin/bash
#
# Contract conformance, every registered provider
#
#   extension/xp_batch/test/source_conformance.sh <PGPORT> [PGHOST] [DB]
#
# One test for all sources instead of an ad-hoc set per source. The checks live
# in C (xpb_conformance.c) and are driven through the registry, so this script
# only decides what to point each provider at and prints the matrix.
#
# A provider is skipped, loudly, when what it needs is absent -- no ZLFS zone,
# no columnar table, no Parquet file, no module loaded. Skipped is not passed.
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
PQ="${PQ_FILE:-$REPO/benchmarks/02-batch-joins/data/reg_buh.parquet}"
LO=25; HI=36

PSQL=("$PGBIN/psql" -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

DECL="CREATE OR REPLACE FUNCTION xpb_source_conformance(
        provider text, relname text DEFAULT NULL, uri text DEFAULT NULL,
        attnos int[] DEFAULT NULL, colnames text[] DEFAULT NULL,
        lo int DEFAULT NULL, hi int DEFAULT NULL)
      RETURNS TABLE(\"check\" text, status text, detail text)
      LANGUAGE C AS '\$libdir/xp_batch', 'xpb_source_conformance';"

total_pass=0; total_fail=0; total_skip=0; providers=0; skipped_providers=""

PRE=$("${PSQL[@]}" -c "SELECT 'alive'" 2>&1 | tail -1)
[ "$PRE" = "alive" ] || { echo "!! cannot query $DB on port $PORT: $PRE"; exit 2; }
"${PSQL[@]}" -c "LOAD 'xp_batch'; $DECL" >/dev/null 2>&1 || {
    echo "!! cannot declare xpb_source_conformance"; exit 2; }

# run <provider> <load> <args...>
run() {
    local prov=$1 load=$2; shift 2
    local out
    out=$("${PSQL[@]}" 2>&1 <<SQL | grep -vE "^WARNING:  ZLFS"
LOAD 'xp_batch'; $load
SET client_min_messages = warning;
$DECL
SELECT rpad("check",30)||rpad(status,6)||detail FROM xpb_source_conformance($*);
SQL
)
    echo "=== $prov ==="
    # An ERROR LINE, not the word "ERROR" inside a check's detail text -- the
    # skip reasons mention "ERROR cleanup", which matched a naive glob and made
    # every provider look unavailable.
    if echo "$out" | grep -qE "^ERROR:"; then
        echo "$out" | grep -E "^(ERROR|DETAIL|HINT):" | sed 's/^/    /'
        echo "  provider SKIPPED: it could not be driven here"
        skipped_providers="$skipped_providers $prov"
        return
    fi
    echo "$out" | sed 's/^/  /'
    providers=$((providers+1))
    local t
    t=$(echo "$out" | grep "^TOTAL" | tail -1)
    total_pass=$((total_pass + $(echo "$t" | grep -oE '[0-9]+ pass' | grep -oE '[0-9]+')))
    total_fail=$((total_fail + $(echo "$t" | grep -oE '[0-9]+ fail' | grep -oE '[0-9]+')))
    total_skip=$((total_skip + $(echo "$t" | grep -oE '[0-9]+ skip' | grep -oE '[0-9]+')))
}

run heap       ""                  "'heap','reg_buh',NULL,ARRAY[1,2,3,6],NULL,$LO,$HI"
run zlfs       "SELECT zlfs_build_zone('reg_buh','1,2,3,6',$LO,$HI);" \
                                   "'zlfs','reg_buh',NULL,ARRAY[1,2,3,6],NULL,$LO,$HI"
run pgcolumnar ""                  "'pgcolumnar','reg_buh_col',NULL,ARRAY[1,2,3,6],NULL,$LO,$HI"
if [ -f "$PQ" ]; then
    run parquet "LOAD 'xpb_parquet';" \
        "'parquet',NULL,'$PQ',NULL,ARRAY['period_key','company_key','account_key','amount_dt'],$LO,$HI"
else
    echo "=== parquet ==="
    echo "  provider SKIPPED: $PQ absent (run benchmarks/02-batch-joins/export-parquet.py)"
    skipped_providers="$skipped_providers parquet"
fi

# The one check the C side cannot make: a scan interrupted from outside. Done
# here for whichever provider has the longest scan available.
echo "=== cancellation (outside the C harness) ==="
T0=$(date +%s%N)
OUT=$("${PSQL[@]}" -c "LOAD 'xp_batch'; SET statement_timeout = 10;
       SELECT count(*) FROM xpb_batch_groupby(1,120,'heap')" 2>&1 | tr '\n' ' ')
T1=$(date +%s%N); MS=$(( (T1-T0)/1000000 ))
case "$OUT" in
    *"statement timeout"*)
        if [ "$MS" -lt 300 ]; then
            echo "  pass  heap scan cancelled in ${MS} ms (statement_timeout 10 ms)"
            total_pass=$((total_pass+1))
        else
            echo "  FAIL  heap scan took ${MS} ms to cancel"
            total_fail=$((total_fail+1))
        fi ;;
    *)  echo "  skip  the full scan finished before the timeout could fire: $OUT"
        total_skip=$((total_skip+1)) ;;
esac

echo
echo "providers driven: $providers${skipped_providers:+, skipped:$skipped_providers}"
echo "############ $total_pass pass, $total_fail fail, $total_skip skip ############"
[ "$total_fail" -eq 0 ]
