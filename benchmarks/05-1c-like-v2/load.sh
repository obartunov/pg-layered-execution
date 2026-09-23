#!/bin/bash
#
# Load dataset 1c-like-v2
#
#   benchmarks/05-1c-like-v2/load.sh <PGPORT> [PGHOST] [DBNAME]   # default: onec2
#
# Loads identical rows into heap and pgColumnar, in PERIOD ORDER. The order is
# the point: it is what gives pgColumnar row groups narrow period ranges, and
# it is verified afterwards against pgcolumnar.zone_map by verify.sql.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-onec2}"
CSV="${TMPDIR:-/tmp}/1c-like-v2.csv"

PSQL=(psql -p "$PORT" -d "$DB" -v ON_ERROR_STOP=1 -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

echo "=== generating (deterministic, seed in gen_data.py) ==="
python3 "$HERE/gen_data.py" > "$CSV"
wc -l < "$CSV" | sed 's/^/csv lines (incl header): /'

echo "=== schema ==="
"${PSQL[@]}" -f "$HERE/schema.sql"

echo "=== heap load, in period order ==="
"${PSQL[@]}" -c "\\copy reg2 FROM '$CSV' WITH (FORMAT csv, HEADER true)"
"${PSQL[@]}" -c "ANALYZE reg2, dim_company, dim_account2"

echo "=== pgColumnar copy, same rows, same order ==="
if "${PSQL[@]}" -c "SELECT 1 FROM pg_extension WHERE extname='pgcolumnar'" | grep -q 1; then
    # INSERT ... SELECT without ORDER BY would take whatever order the seq
    # scan produced; the heap is already in period order, but say so anyway.
    "${PSQL[@]}" -c "CREATE TABLE reg2_col (LIKE reg2) USING pgcolumnar"
    "${PSQL[@]}" -c "INSERT INTO reg2_col SELECT * FROM reg2 ORDER BY period"
    "${PSQL[@]}" -c "ANALYZE reg2_col"
else
    echo "!! pgcolumnar extension absent: columnar paths will be skipped"
fi

echo "=== physical sizes ==="
"${PSQL[@]}" -c "
SELECT rpad(c.relname, 12) || ' ' ||
       lpad(pg_size_pretty(pg_total_relation_size(c.oid)), 10) || '  ' ||
       lpad(round(pg_total_relation_size(c.oid)::numeric / r.n, 1)::text, 7) || ' bytes/row'
FROM pg_class c, (SELECT count(*)::numeric AS n FROM reg2) r
WHERE c.relname IN ('reg2','reg2_col','dim_company','dim_account2')
ORDER BY c.relname"

rm -f "$CSV"
echo "=== loaded. Run verify.sql next -- it proves the clustering. ==="
