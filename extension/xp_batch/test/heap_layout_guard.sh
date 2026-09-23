#!/bin/bash
#
# HeapBatchSource fixed-offset layout guard
#
#   extension/xp_batch/test/heap_layout_guard.sh <PGPORT> [PGHOST]
#
# xpb_heap_source_create() computes each column's byte offset once, from
# attlen, and applies it to every tuple. That is only valid while every
# attribute up to the highest one read is fixed-width and NOT NULL.
#
# When it was not, the source did not fail -- it read the wrong bytes and
# returned a result with the right row count, the right group count, and an
# internally consistent net = debit - credit. On the layouts below the
# unguarded code returned:
#
#   varlena at attnum 4   debit=5000     credit=5000000  (truth: 5000000 / 500000)
#   NULL    at attnum 4   debit=500000   credit=0        (truth: 5000000 / 500000)
#
# Each case must now raise an error instead. A case that RETURNS ROWS is a
# failure of this test whatever those rows contain -- that is the bug.
set -uo pipefail

PORT="${1:?port}"
PGHOST_ARG="${2:-}"
DB="xpb_layout_guard_$$"

PSQLBASE=(psql -p "$PORT" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQLBASE+=(-h "$PGHOST_ARG")

"${PSQLBASE[@]}" -d postgres -c "CREATE DATABASE $DB" >/dev/null
trap '"${PSQLBASE[@]}" -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1' EXIT

PSQL=("${PSQLBASE[@]}" -d "$DB")
"${PSQL[@]}" -c "CREATE EXTENSION xp_batch" >/dev/null

fail=0

# $1 = case name, $2 = DDL for reg_buh, $3 = the INSERT's value list
check() {
    local name="$1" ddl="$2" vals="$3" out

    "${PSQL[@]}" >/dev/null 2>&1 <<SQL
DROP TABLE IF EXISTS reg_buh, dim_period, dim_account CASCADE;
$ddl
INSERT INTO reg_buh SELECT $vals FROM generate_series(1, 5000);
CREATE TABLE dim_period  (period_key int NOT NULL, year int NOT NULL, month int NOT NULL);
INSERT INTO dim_period VALUES (1, 2026, 1);
CREATE TABLE dim_account (account_key int NOT NULL, account_group int NOT NULL);
INSERT INTO dim_account VALUES (1, 1);
SQL

    out=$("${PSQL[@]}" -c \
        "SELECT debit_turnover || '/' || credit_turnover
           FROM xpb_1c_register_report(1, 1, 'heap')" 2>&1)

    if grep -q "^ERROR:" <<<"$out"; then
        echo "  PASS  $name"
        echo "        $(grep '^ERROR:' <<<"$out" | head -1 | cut -c1-96)"
    else
        echo "  FAIL  $name: returned rows instead of erroring -> $(tr '\n' ' ' <<<"$out" | cut -c1-72)"
        fail=1
    fi
}

echo "=== layouts the fixed-offset source must refuse ==="

check "varlena before a read column" "
CREATE TABLE reg_buh (
    period_key int NOT NULL, company_key int NOT NULL, account_key int NOT NULL,
    debit_key  text NOT NULL,
    credit_key int NOT NULL, amount_dt int NOT NULL, amount_kt int NOT NULL,
    payload    int NOT NULL);" \
"1, 1, 1, 'x', 1, 1000, 100, 0"

check "nullable column before a read column" "
CREATE TABLE reg_buh (
    period_key int NOT NULL, company_key int NOT NULL, account_key int NOT NULL,
    debit_key  int,
    credit_key int NOT NULL, amount_dt int NOT NULL, amount_kt int NOT NULL,
    payload    int NOT NULL);" \
"1, 1, 1, NULL, 1, 1000, 100, 0"

check "nullable READ column" "
CREATE TABLE reg_buh (
    period_key int NOT NULL, company_key int NOT NULL, account_key int NOT NULL,
    debit_key  int NOT NULL, credit_key int NOT NULL,
    amount_dt  int,
    amount_kt  int NOT NULL, payload int NOT NULL);" \
"1, 1, 1, 1, 1, 1000, 100, 0"

check "numeric before a read column" "
CREATE TABLE reg_buh (
    period_key int NOT NULL, company_key int NOT NULL, account_key int NOT NULL,
    debit_key  numeric(18,2) NOT NULL,
    credit_key int NOT NULL, amount_dt int NOT NULL, amount_kt int NOT NULL,
    payload    int NOT NULL);" \
"1, 1, 1, 10.37, 1, 1000, 100, 0"

echo
echo "=== a layout it must still accept ==="

"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
DROP TABLE IF EXISTS reg_buh, dim_period, dim_account CASCADE;
CREATE TABLE reg_buh (
    period_key int NOT NULL, company_key int NOT NULL, account_key int NOT NULL,
    debit_key  int NOT NULL, credit_key int NOT NULL, amount_dt int NOT NULL,
    amount_kt  int NOT NULL,
    -- after the last column read (attnum 7): unconstrained on purpose
    comment    text, extra numeric(18,2));
INSERT INTO reg_buh SELECT 1, 1, 1, 1, 1, 1000, 100, NULL, NULL FROM generate_series(1, 5000);
CREATE TABLE dim_period  (period_key int NOT NULL, year int NOT NULL, month int NOT NULL);
INSERT INTO dim_period VALUES (1, 2026, 1);
CREATE TABLE dim_account (account_key int NOT NULL, account_group int NOT NULL);
INSERT INTO dim_account VALUES (1, 1);
SQL

got=$("${PSQL[@]}" -c "SELECT debit_turnover || '/' || credit_turnover
                       FROM xpb_1c_register_report(1, 1, 'heap')" 2>&1 | tail -1)
want=$("${PSQL[@]}" -c "SELECT sum(amount_dt) || '/' || sum(amount_kt) FROM reg_buh")

if [ "$got" = "$want" ]; then
    echo "  PASS  varlena and nullable AFTER the last read column: $got"
else
    echo "  FAIL  expected $want, got $got"
    fail=1
fi

echo
[ $fail -eq 0 ] && echo "heap_layout_guard: ALL PASS" || echo "heap_layout_guard: FAILURES"
exit $fail
