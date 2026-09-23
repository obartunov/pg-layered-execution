#!/bin/bash
#
# Deterministic load of the benchmark dataset.
#
#   benchmarks/common/load.sh [PGPORT] [PGHOST] [DBNAME]
#
# gen_data.py is seeded (seed=42, 10M rows), so the fact table is byte-identical
# on every run and every machine.  The CSV is kept between runs; delete it to
# force regeneration.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-testdb}"
CSV="${XPB_CSV:-/tmp/xpb_reg_buh.csv}"

PSQL=(psql -p "$PORT" -d "$DB" -v ON_ERROR_STOP=1 -X)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

if [ ! -s "$CSV" ]; then
    echo "-- generating $CSV (10M rows, seed 42)"
    python3 "$HERE/gen_data.py" > "$CSV"
fi
# the server reads the CSV as its own user; a pre-existing file we do not own is
# fine as long as it is already readable, so a failed chmod is not fatal
chmod a+r "$CSV" 2>/dev/null || [ -r "$CSV" ] || {
    echo "$CSV is not readable" >&2; exit 1; }

echo "-- schema"
"${PSQL[@]}" -f "$HERE/schema.sql" >/dev/null

echo "-- COPY $CSV"
"${PSQL[@]}" -c "COPY reg_buh FROM '$CSV' WITH (FORMAT csv, HEADER true)" >/dev/null

echo "-- analyze"
# The columnar copy is optional: without pgcolumnar the benchmark still runs
# its heap and ZLFS paths.
if "${PSQL[@]}" -qAt -c \
   "SELECT 1 FROM pg_available_extensions WHERE name='pgcolumnar'" | grep -q 1
then
    echo "-- columnar copy (reg_buh_col)"
    "${PSQL[@]}" -f "$HERE/schema-columnar.sql" >/dev/null
else
    echo "-- pgcolumnar not available, skipping reg_buh_col"
fi

# one VACUUM per -c: psql wraps a multi-statement -c in a transaction block
for t in reg_buh dim_period dim_account; do
    "${PSQL[@]}" -c "VACUUM ANALYZE $t" >/dev/null
done
"${PSQL[@]}" -qAt -c "SELECT to_regclass('reg_buh_col')" | grep -q . && \
    "${PSQL[@]}" -c "ANALYZE reg_buh_col" >/dev/null

"${PSQL[@]}" -qAt -c "
SELECT 'fact rows:    ' || count(*) FROM reg_buh
UNION ALL SELECT 'periods 25-36: ' || count(*) FROM reg_buh WHERE period_key BETWEEN 25 AND 36
UNION ALL SELECT 'dim_period:   ' || count(*) FROM dim_period
UNION ALL SELECT 'dim_account:  ' || count(*) FROM dim_account"
