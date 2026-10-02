#!/bin/bash
#
# Legal NULLs must not be read as values by operators that have no NULL branch
#
#   extension/xp_batch/test/reproducers/batch_validity_refusal.sh <PGPORT> [PGHOST]
#
# The typed batch contract lets any source hand over a validity bitmap, and
# three of the four sources do: the heap source's deform and projected paths,
# pgcolumnar, ZLFS zones and Parquet. Several operators read cols[c].data[row]
# and group or sum it with no NULL branch anywhere, and the join probes' gather
# paths set validity = NULL while compacting values, which makes every row below
# a dropped bit look valid.
#
# Measured before the fix, on a ZLFS zone over a nullable company_key:
#
#     PostgreSQL         252 groups, sum 1 063 110
#     xpb_batch_groupby  240 groups, sum 1 063 110
#     zlfs_group_sum     240 groups, sum 1 063 110
#
# The SUMS AGREED. The NULL group key was merged into a value group, so twelve
# groups -- one per period in the range -- disappeared. A sum-only comparison
# passes this defect, which is why the group count is compared first.
#
# The policy is refusal before execution, not NULL semantics invented in one
# read loop: these are the benchmark's fixed-schema operators, and the path that
# does implement NULL semantics (xpb_typed_report) is checked here too, so the
# refusal cannot be mistaken for "NULLs are unsupported everywhere".
#
# Own database, created and dropped here: the entry points resolve reg_buh,
# dim_period and dim_account by name through the search path, so the fixtures
# must carry those names.
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
DB="xpb_validity_$$"

PSQLBASE=("$PGBIN/psql" -p "$PORT" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQLBASE+=(-h "$PGHOST_ARG")

"${PSQLBASE[@]}" -d postgres -c "CREATE DATABASE $DB" >/dev/null || {
    echo "cannot create database $DB"; exit 2; }
trap '"${PSQLBASE[@]}" -d postgres -c "DROP DATABASE IF EXISTS '"$DB"'" >/dev/null 2>&1' EXIT
PSQL=("${PSQLBASE[@]}" -d "$DB")
"${PSQL[@]}" -c "CREATE EXTENSION xp_batch" >/dev/null || { echo "no xp_batch"; exit 2; }

pass=0; fail=0
ok()  { echo "  ok      $1"; pass=$((pass+1)); }
bad() { echo "  FAIL    $1"; fail=$((fail+1)); }

q() { "${PSQL[@]}" 2>&1 <<SQL | grep -vE "^WARNING:  ZLFS" | tail -1
LOAD 'xp_batch';
SET max_parallel_workers_per_gather=0; SET jit=off; SET client_min_messages=warning;
$1
SQL
}
# An ERROR prints over three lines (ERROR / DETAIL / HINT) and tail -1 lands on
# the last of them, so a refusal read with q() looks like an answer. Refusals
# are judged on the whole output.
qall() { "${PSQL[@]}" 2>&1 <<SQL | grep -vE "^WARNING:  ZLFS" | tr '\n' ' '
LOAD 'xp_batch';
SET max_parallel_workers_per_gather=0; SET jit=off; SET client_min_messages=warning;
$1
SQL
}

echo "=== fixture: reg_buh with nullable company_key and amount_dt, ZLFS zone ==="
"${PSQL[@]}" -v ON_ERROR_STOP=1 >/dev/null 2>&1 <<'SQL' || { echo "!! fixture failed"; exit 2; }
CREATE TABLE reg_buh (period_key int NOT NULL, company_key int, account_key int NOT NULL,
                      debit_key int NOT NULL, credit_key int NOT NULL, amount_dt int,
                      amount_kt int NOT NULL, payload int NOT NULL);
INSERT INTO reg_buh SELECT i/2000+1, CASE WHEN i%11 = 0 THEN NULL ELSE i%20 END,
       1+i%200, 1, 1, CASE WHEN i%13 = 0 THEN NULL ELSE i%97 END, 1, 1
FROM generate_series(0,239999) i;
CREATE TABLE dim_period  (period_key int NOT NULL, year int NOT NULL, month int NOT NULL);
INSERT INTO dim_period SELECT id, (id-1)/12+2015, (id-1)%12+1 FROM generate_series(1,120) id;
CREATE TABLE dim_account (account_key int NOT NULL, account_group int NOT NULL);
INSERT INTO dim_account SELECT g, (g-1)/50+1 FROM generate_series(1,200) g;
ANALYZE reg_buh; ANALYZE dim_period; ANALYZE dim_account;
SQL
BUILT=$(q "SELECT zlfs_build_zone('reg_buh','1,2,6',25,36);")
[ "$BUILT" = "VALID" ] || { echo "!! zone not built: $BUILT"; exit 2; }

# The oracle, for the record and for the NULL-supporting path below.
ORACLE=$(q "SELECT count(*)||'|'||sum(s)::text FROM (SELECT period_key, company_key,
            sum(amount_dt) s FROM reg_buh WHERE period_key BETWEEN 25 AND 36
            GROUP BY 1,2) q;")
echo "          PostgreSQL: $ORACLE"
[ "$ORACLE" = "252|1063110" ] && ok "oracle is the measured one (252 groups, not 240)" \
                             || bad "oracle changed: $ORACLE (expected 252|1063110)"

# refuses <label> <sql>
refuses() {
    local out; out=$(qall "$2")
    case "$out" in
        *"carries NULLs"*)  ok "$1 refuses with the NULL reason" ;;
        *ERROR*)            bad "$1 errors for another reason: $out" ;;
        *)                  bad "$1 ANSWERED: $out" ;;
    esac
}

echo "=== operators with no NULL branch must refuse, not answer ==="
refuses "xpb_batch_groupby(zlfs)" \
        "SELECT count(*) FROM xpb_batch_groupby(25,36,'zlfs');"
refuses "zlfs_group_sum" \
        "SELECT count(*) FROM zlfs_group_sum(25,36);"
refuses "xpb_batch_join_groupby(zlfs)" \
        "SELECT count(*) FROM xpb_batch_join_groupby(25,36,'zlfs');"
# join2 and the 1C report project attno 3 as well, so they need a zone over
# four columns; the three-column zone above makes them fail for the wrong reason.
# The 1C report also projects attno 7 (amount_kt), so it needs five.
BUILT4=$(q "SELECT zlfs_build_zone('reg_buh','1,2,3,6,7',25,36);")
[ "$BUILT4" = "VALID" ] || echo "  !! five-column zone not built: $BUILT4"
refuses "xpb_batch_join2_groupby(zlfs)" \
        "SELECT count(*) FROM xpb_batch_join2_groupby(25,36,'zlfs');"
refuses "xpb_1c_register_report(zlfs)" \
        "SELECT count(*) FROM xpb_1c_register_report(25,36,'zlfs');"

echo "=== and the refusal is about NULLs, not about the zone ==="
# Same zone shape over NOT NULL columns: every one of those must work again, or
# the refusal would have disabled the ZLFS arm wholesale.
"${PSQL[@]}" -v ON_ERROR_STOP=1 >/dev/null 2>&1 <<'SQL'
DROP TABLE reg_buh;
CREATE TABLE reg_buh (period_key int NOT NULL, company_key int NOT NULL, account_key int NOT NULL,
                      debit_key int NOT NULL, credit_key int NOT NULL, amount_dt int NOT NULL,
                      amount_kt int NOT NULL, payload int NOT NULL);
INSERT INTO reg_buh SELECT i/2000+1, i%20, 1+i%200, 1, 1, i%97, 1, 1
FROM generate_series(0,239999) i;
ANALYZE reg_buh;
SQL
BUILT=$(q "SELECT zlfs_build_zone('reg_buh','1,2,6',25,36);")
[ "$BUILT" = "VALID" ] || { echo "!! zone not rebuilt: $BUILT"; exit 2; }
WANT=$(q "SELECT count(*)||'|'||sum(s)::text FROM (SELECT period_key, company_key,
          sum(amount_dt) s FROM reg_buh WHERE period_key BETWEEN 25 AND 36 GROUP BY 1,2) q;")
GOT=$(q "SELECT count(*)||'|'||sum(total_amt)::text FROM xpb_batch_groupby(25,36,'zlfs');")
[ "$GOT" = "$WANT" ] && ok "xpb_batch_groupby(zlfs) still runs on NOT NULL columns (= $GOT)" \
                     || bad "NOT NULL case broke: PostgreSQL [$WANT], Xp [$GOT]"
GOT=$(q "SELECT count(*)||'|'||sum(total_amt)::text FROM zlfs_group_sum(25,36);")
[ "$GOT" = "$WANT" ] && ok "zlfs_group_sum still runs on NOT NULL columns (= $GOT)" \
                     || bad "NOT NULL case broke: PostgreSQL [$WANT], Xp [$GOT]"

echo "=== an int8 zone column is refused by type, not read in halves ==="
"${PSQL[@]}" -v ON_ERROR_STOP=1 >/dev/null 2>&1 <<'SQL'
DROP TABLE reg_buh;
CREATE TABLE reg_buh (period_key int NOT NULL, company_key int NOT NULL, account_key int NOT NULL,
                      debit_key int NOT NULL, credit_key int NOT NULL, amount_dt bigint NOT NULL,
                      amount_kt int NOT NULL, payload int NOT NULL);
INSERT INTO reg_buh SELECT i/2000+1, i%20, 1+i%200, 1, 1, (i%97)::bigint, 1, 1
FROM generate_series(0,239999) i;
ANALYZE reg_buh;
SQL
BUILT=$(q "SELECT zlfs_build_zone('reg_buh','1,2,6',25,36);")
if [ "$BUILT" = "VALID" ]; then
    OUT=$(qall "SELECT count(*) FROM zlfs_group_sum(25,36);")
    case "$OUT" in
        *"not int4"*) ok "zlfs_group_sum refuses an int8 zone column by type" ;;
        *ERROR*)      bad "refused for another reason: $OUT" ;;
        *)            bad "zlfs_group_sum ANSWERED over an int8 column: $OUT" ;;
    esac
else
    echo "  skip    int8 zone -- zlfs_build_zone declined it: $BUILT"
fi

echo "=== the path that does implement NULL semantics still does ==="
# xpb_typed_report consults xpcb_isnull() for join keys, group keys and sums.
# If the refusals above had been applied indiscriminately, this would break.
"${PSQL[@]}" -v ON_ERROR_STOP=1 >/dev/null 2>&1 <<'SQL'
DROP TABLE reg_buh;
CREATE TABLE reg_buh (period_key int NOT NULL, company_key int, account_key int NOT NULL,
                      debit_key int NOT NULL, credit_key int NOT NULL, amount_dt int,
                      amount_kt int NOT NULL, payload int NOT NULL);
INSERT INTO reg_buh SELECT i/2000+1, CASE WHEN i%11 = 0 THEN NULL ELSE i%20 END,
       1+i%200, 1, 1, CASE WHEN i%13 = 0 THEN NULL ELSE i%97 END, 1, 1
FROM generate_series(0,239999) i;
ANALYZE reg_buh;
SQL
OUT=$(q "SELECT count(*) FROM xpb_typed_report('reg_buh', ARRAY[1,2], ARRAY[6]);")
case "$OUT" in
    [0-9]*) ok "xpb_typed_report still answers over nullable columns (= $OUT rows)" ;;
    *)      bad "xpb_typed_report broke: $OUT" ;;
esac

echo
echo "############ $pass correct, $fail wrong ############"
[ "$fail" -eq 0 ]
