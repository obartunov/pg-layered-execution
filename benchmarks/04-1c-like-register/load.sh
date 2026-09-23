#!/bin/bash
#
# Load dataset 1c-like-v1.
#
#   benchmarks/04-1c-like-register/load.sh [PGPORT] [PGHOST] [DBNAME]
#
# Creates reg_buh (heap), reg_buh_col (pgcolumnar, same rows) and the two
# dimensions, then prints the dataset properties and the physical sizes.
#
# Use a database of its own: this dataset reuses the table NAMES of
# reconstructed-v1 and must not be loaded over it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-onec}"
CSV="${XPB_1C_CSV:-/tmp/xpb_1c_reg_buh.csv}"

PSQL=(psql -p "$PORT" -d "$DB" -v ON_ERROR_STOP=1 -X)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

if [ ! -s "$CSV" ]; then
    echo "-- generating $CSV (1 000 008 rows, seed 20260923)"
    python3 "$HERE/gen_data.py" > "$CSV"
fi
chmod a+r "$CSV" 2>/dev/null || [ -r "$CSV" ] || { echo "$CSV not readable" >&2; exit 1; }

echo "-- schema"
"${PSQL[@]}" -f "$HERE/schema.sql" >/dev/null

echo "-- COPY $CSV"
"${PSQL[@]}" -c "COPY reg_buh FROM '$CSV' WITH (FORMAT csv, HEADER true)" >/dev/null

# The columnar copy is optional; without pgcolumnar the two columnar paths are
# skipped and the rest of the benchmark still runs.
if "${PSQL[@]}" -qAt -c \
   "SELECT 1 FROM pg_available_extensions WHERE name='pgcolumnar'" | grep -q 1
then
    echo "-- columnar copy (reg_buh_col), same rows"
    "${PSQL[@]}" >/dev/null <<'SQL'
CREATE EXTENSION IF NOT EXISTS pgcolumnar;
DROP TABLE IF EXISTS reg_buh_col;
CREATE TABLE reg_buh_col (LIKE reg_buh) USING pgcolumnar;
INSERT INTO reg_buh_col SELECT * FROM reg_buh;
SQL
else
    echo "-- pgcolumnar not available, skipping reg_buh_col"
fi

for t in reg_buh dim_period dim_account; do
    "${PSQL[@]}" -c "VACUUM ANALYZE $t" >/dev/null
done
"${PSQL[@]}" -qAt -c "SELECT to_regclass('reg_buh_col')" | grep -q . && \
    "${PSQL[@]}" -c "ANALYZE reg_buh_col" >/dev/null

echo
echo "=== dataset properties ==="
"${PSQL[@]}" -f "$HERE/verify.sql"

echo
echo "=== physical size of each representation ==="
"${PSQL[@]}" -qAt -c "
SELECT 'heap reg_buh:       ' || pg_size_pretty(pg_relation_size('reg_buh'))
UNION ALL SELECT 'pgcolumnar reg_buh_col: ' ||
    coalesce(pg_size_pretty(pg_relation_size('reg_buh_col')), 'absent')"
