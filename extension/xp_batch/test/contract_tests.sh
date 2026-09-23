#!/bin/bash
#
# Typed batch contract tests
#
#   extension/xp_batch/test/contract_tests.sh <PGPORT> [PGHOST]
#
# Every case compares the batch layer's view of a table against PostgreSQL's
# own view of the same table, per column, via xpb_contract_probe(). That is
# the point: an aggregate that comes out right proves much less than
# value-by-value agreement including type and NULLability, and it was an
# aggregate coming out right that hid the fixed-offset corruption.
#
# Rows are compared as sorted multisets, because heap scan order is not
# PostgreSQL's output order and neither is promised by the contract.
set -uo pipefail

PORT="${1:?port}"
PGHOST_ARG="${2:-}"
DB="xpb_contract_$$"

PSQLBASE=(psql -p "$PORT" -X -qAt -v ON_ERROR_STOP=0)
[ -n "$PGHOST_ARG" ] && PSQLBASE+=(-h "$PGHOST_ARG")

"${PSQLBASE[@]}" -d postgres -c "CREATE DATABASE $DB" >/dev/null
trap '"${PSQLBASE[@]}" -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1' EXIT

PSQL=("${PSQLBASE[@]}" -d "$DB")
"${PSQL[@]}" -c "CREATE EXTENSION xp_batch" >/dev/null

fail=0
pass_count=0

# --- compare the probe against PostgreSQL, column by column ---------------
# $1 name  $2 table  $3 attnos literal  $4 path  $5 SQL column list matching attnos
compare() {
    local name="$1" tbl="$2" attnos="$3" path="$4" cols="$5"
    local got want

    # batch view: (col, value) multiset, NULL rendered as the sentinel-free
    # marker \N so an empty string stays distinguishable from NULL
    got=$("${PSQL[@]}" -c "
        SELECT md5(string_agg(col || ':' || coalesce(val, E'\\\\N'), ','
                              ORDER BY col, coalesce(val, E'\\\\N')))
        FROM xpb_contract_probe('$tbl', $attnos, '$path')" 2>&1)

    # PostgreSQL's view of the same columns, unpivoted the same way
    want=$("${PSQL[@]}" -c "
        SELECT md5(string_agg(col || ':' || coalesce(val, E'\\\\N'), ','
                              ORDER BY col, coalesce(val, E'\\\\N')))
        FROM (SELECT * FROM $tbl) t,
        LATERAL (VALUES $cols) AS v(col, val)" 2>&1)

    if [ "$got" = "$want" ] && [ -n "$got" ] && [[ ! "$got" =~ ERROR ]]; then
        echo "  PASS  $name"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  $name"
        echo "        batch      : $(head -c 120 <<<"$got")"
        echo "        postgresql : $(head -c 120 <<<"$want")"
        fail=1
    fi
}

# $1 name  $2 table  $3 attnos  $4 path  $5 expected reason substring
#
# The reason is checked, not just the fact of an error. A case named "nullable
# refused" that actually trips over a column being int8 proves nothing about
# nullability, and a test that cannot tell those apart is the kind that passes
# on broken code.
must_error() {
    local name="$1" want="$5" out line
    out=$("${PSQL[@]}" -c "SELECT count(*) FROM xpb_contract_probe('$2', $3, '$4')" 2>&1)
    line=$(grep '^ERROR:' <<<"$out" | head -1)

    if [ -z "$line" ]; then
        echo "  FAIL  $name: no error, got $(tr '\n' ' ' <<<"$out" | cut -c1-60)"
        fail=1
    elif [[ "$line" != *"$want"* ]]; then
        echo "  FAIL  $name: wrong reason"
        echo "        wanted : ...$want..."
        echo "        got    : $(cut -c1-92 <<<"$line")"
        fail=1
    else
        echo "  PASS  $name"
        echo "        $(cut -c1-92 <<<"$line")"
        pass_count=$((pass_count + 1))
    fi
}

echo "=== fixed-width types ==="

"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE t_i4 (a int4 NOT NULL, b int4 NOT NULL);
INSERT INTO t_i4 SELECT g, -g FROM generate_series(1, 3000) g;

-- values past 2^31 on both sides of zero: truncation to int32 cannot survive
CREATE TABLE t_i8 (a int8 NOT NULL, b int8 NOT NULL);
INSERT INTO t_i8 VALUES
    (2147483647, -2147483648),
    (2147483648, -2147483649),
    (5000000000, -5000000000),
    (9223372036854775807, -9223372036854775808);

CREATE TABLE t_mix (a int4 NOT NULL, b int8 NOT NULL, c int4 NOT NULL);
INSERT INTO t_mix SELECT g, g::int8 * 4000000000, g * 7 FROM generate_series(1, 500) g;
SQL

compare "int4 only, deform"      t_i4  "ARRAY[1,2]"   deform "(0,a::text),(1,b::text)"
compare "int4 only, fixed"       t_i4  "ARRAY[1,2]"   fixed  "(0,a::text),(1,b::text)"
compare "int8 only, deform"      t_i8  "ARRAY[1,2]"   deform "(0,a::text),(1,b::text)"
compare "mixed int4/int8"        t_mix "ARRAY[1,2,3]" deform "(0,a::text),(1,b::text),(2,c::text)"

must_error "int8 refused by the fixed path" t_i8 "ARRAY[1,2]" fixed \
           'column "a" is not int4'

echo
echo "=== validity ==="

"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE v_none (a int4 NOT NULL, b int8 NOT NULL);
INSERT INTO v_none SELECT g, g::int8 FROM generate_series(1, 100) g;

CREATE TABLE v_some (a int4, b int8);
INSERT INTO v_some SELECT CASE WHEN g % 7 = 0 THEN NULL ELSE g END,
                          CASE WHEN g % 5 = 0 THEN NULL ELSE g::int8 END
                   FROM generate_series(1, 100) g;

CREATE TABLE v_all (a int4, b int8);
INSERT INTO v_all SELECT NULL, NULL FROM generate_series(1, 100);

CREATE TABLE v_first (a int4, b int4);
INSERT INTO v_first SELECT CASE WHEN g = 1 THEN NULL ELSE g END, g
                    FROM generate_series(1, 100) g;

CREATE TABLE v_last (a int4, b int4);
INSERT INTO v_last SELECT CASE WHEN g = 100 THEN NULL ELSE g END, g
                   FROM generate_series(1, 100) g;

-- more than one batch (probe capacity is 1024) with NULLs on both boundaries
CREATE TABLE v_span (a int4, b int4);
INSERT INTO v_span SELECT CASE WHEN g IN (1, 1024, 1025, 3000) THEN NULL ELSE g END, g
                   FROM generate_series(1, 3000) g;
SQL

compare "all valid"              v_none  "ARRAY[1,2]" deform "(0,a::text),(1,b::text)"
compare "some NULL"              v_some  "ARRAY[1,2]" deform "(0,a::text),(1,b::text)"
compare "all NULL"               v_all   "ARRAY[1,2]" deform "(0,a::text),(1,b::text)"
compare "NULL in first row"      v_first "ARRAY[1,2]" deform "(0,a::text),(1,b::text)"
compare "NULL in last row"       v_last  "ARRAY[1,2]" deform "(0,a::text),(1,b::text)"
compare "NULL across batch edge" v_span  "ARRAY[1,2]" deform "(0,a::text),(1,b::text)"

# all int4, so "not int4" cannot be the reason -- only nullability can be
"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE v_i4null (a int4 NOT NULL, b int4, c int4 NOT NULL);
INSERT INTO v_i4null SELECT g, CASE WHEN g % 4 = 0 THEN NULL ELSE g END, g * 3
                     FROM generate_series(1, 200) g;
SQL
compare "nullable int4 in the middle" v_i4null "ARRAY[1,2,3]" deform \
        "(0,a::text),(1,b::text),(2,c::text)"
must_error "nullable refused by the fixed path" v_i4null "ARRAY[1,2,3]" fixed \
           'column "b" is nullable'
must_error "nullable BEFORE a read column refused" v_i4null "ARRAY[1,3]" fixed \
           'column "b" is nullable' 

echo
echo "=== varlena and numeric ==="

"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
-- the layout that used to corrupt silently: varlena at attnum 2, ahead of
-- the fixed-width columns the report reads
CREATE TABLE w_var (a int4 NOT NULL, note text, b int4 NOT NULL, c int8 NOT NULL);
INSERT INTO w_var SELECT g,
                         CASE WHEN g % 3 = 0 THEN NULL
                              WHEN g % 3 = 1 THEN ''
                              ELSE repeat('x', g % 40) END,
                         g * 2, g::int8 * 3000000000
                  FROM generate_series(1, 500) g;

-- non-integer scales on purpose: an implementation that routes numeric
-- through float or truncates the scale cannot reproduce these
CREATE TABLE n_exact (a int4 NOT NULL, d numeric(18,2) NOT NULL, e numeric(18,2));
INSERT INTO n_exact VALUES
    (1,       10.37,        0.01),
    (2,     1234.56,     1234.56),
    (3,        0.01,        NULL),
    (4, 99999999.99,        0.00),
    (5,        0.00, -99999999.99);

-- unconstrained numeric: scale varies row to row
CREATE TABLE n_free (a int4 NOT NULL, d numeric NOT NULL);
INSERT INTO n_free VALUES
    (1, 0.000000000000000001),
    (2, 123456789012345678901234567890.123456789),
    (3, -0.5),
    (4, 7);
SQL

compare "varlena before fixed-width cols" w_var   "ARRAY[1,2,3,4]" deform \
        "(0,a::text),(1,note),(2,b::text),(3,c::text)"
compare "skipping the varlena"            w_var   "ARRAY[1,3,4]"   deform \
        "(0,a::text),(1,b::text),(2,c::text)"
compare "numeric, exact decimal scale"    n_exact "ARRAY[1,2,3]"   deform \
        "(0,a::text),(1,d::text),(2,e::text)"
compare "numeric, unconstrained scale"    n_free  "ARRAY[1,2]"     deform \
        "(0,a::text),(1,d::text)"

# every REQUESTED column is int4; the varlena sits between them, which is
# exactly the layout that used to be read at the wrong offsets
"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE w_i4var (a int4 NOT NULL, note text NOT NULL, b int4 NOT NULL);
INSERT INTO w_i4var SELECT g, repeat('y', g % 17), g * 5 FROM generate_series(1, 200) g;
SQL
compare "int4 either side of a varlena" w_i4var "ARRAY[1,3]" deform \
        "(0,a::text),(1,b::text)"
must_error "varlena between read columns refused by fixed" w_i4var "ARRAY[1,3]" fixed \
           'column "note" is variable-width'
must_error "numeric refused by the fixed path"  n_exact "ARRAY[1,2]" fixed \
           'column "d" is not int4' 

echo
echo "=== toasted varlena ==="

"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
-- big enough to be pushed out of line, so the deform path has to detoast
CREATE TABLE w_toast (a int4 NOT NULL, big text, b int4 NOT NULL);
INSERT INTO w_toast SELECT g, repeat('abcdefgh', 2000 * g), g * 11
                    FROM generate_series(1, 5) g;
SQL

compare "external (toasted) varlena" w_toast "ARRAY[1,2,3]" deform \
        "(0,a::text),(1,big),(2,b::text)"

echo
if [ $fail -eq 0 ]; then
    echo "contract_tests: ALL PASS ($pass_count cases)"
else
    echo "contract_tests: FAILURES"
fi
exit $fail
