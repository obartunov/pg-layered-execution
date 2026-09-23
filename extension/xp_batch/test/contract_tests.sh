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

-- fixed-width prefix, but the requested column itself is variable-width
CREATE TABLE n_fixed (a int4 NOT NULL, d numeric(18,2) NOT NULL);
INSERT INTO n_fixed SELECT g, (g/100.0)::numeric(18,2) FROM generate_series(1,200) g;
SQL

compare "int4 only, deform"      t_i4  "ARRAY[1,2]"   deform "(0,a::text),(1,b::text)"
compare "int4 only, fixed"       t_i4  "ARRAY[1,2]"   fixed  "(0,a::text),(1,b::text)"
compare "int8 only, deform"      t_i8  "ARRAY[1,2]"   deform "(0,a::text),(1,b::text)"
compare "mixed int4/int8"        t_mix "ARRAY[1,2,3]" deform "(0,a::text),(1,b::text),(2,c::text)"

# int8 IS addressable by fixed offsets -- it is fixed-width and by-value --
# and became so in 05-B. What the fixed path still cannot address is a
# variable-width type, because it has no fixed width to step over.
compare "int8 accepted by the fixed path" t_i8 "ARRAY[1,2]" fixed \
        "(0,a::text),(1,b::text)"
must_error "numeric refused by the fixed path, by type" n_fixed "ARRAY[1,2]" fixed \
           'is not int4 or int8'

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
echo "=== GROUP BY and SUM semantics ==="

# $1 name  $2 batch-pipeline SQL  $3 PostgreSQL SQL -- both render one line
# per group as "gkey n=<rows> sums={...}"
compare_sql() {
    local name="$1" got want
    # WARNING and NOTICE lines are dropped. The shared ZLFS directory
    # accumulates zone files belonging to other databases and warns about each
    # one, and the report functions print a NOTICE carrying their mode name and
    # timings -- neither is part of the result being compared. ERROR lines are
    # kept, so a real failure still shows.
    got=$("${PSQL[@]}" -c "$2" 2>&1 | grep -vE '^(WARNING|NOTICE):')
    want=$("${PSQL[@]}" -c "$3" 2>&1 | grep -vE '^(WARNING|NOTICE):')

    if [ "$got" = "$want" ] && [ -n "$got" ] && [[ ! "$got" =~ ERROR ]]; then
        echo "  PASS  $name"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  $name"
        echo "        batch      : $(tr '\n' ' ' <<<"$got" | cut -c1-140)"
        echo "        postgresql : $(tr '\n' ' ' <<<"$want" | cut -c1-140)"
        fail=1
    fi
}

"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE g_f (g1 int4, g2 int8, s1 int4, s2 int8, s3 numeric(18,2));
INSERT INTO g_f VALUES
 (1,    10,   5,    5000000000,   10.37),
 (1,    10,   NULL, NULL,         NULL),      -- partial NULLs inside a group
 (1,    10,   7,    5000000000,    0.01),
 (NULL, 10,   3,    1,          1234.56),     -- NULL in the first key
 (NULL, 10,   4,    2,             0.00),
 (1,    NULL, 1,    1,             0.01),     -- NULL in the second key
 (NULL, NULL, NULL, NULL,         NULL),      -- every input NULL
 (NULL, NULL, NULL, NULL,         NULL);
SQL

compare_sql "GROUP BY with NULL keys, SUM over int4/int8/numeric" \
"SELECT gkey || ' n=' || nrows || ' sums=' || sums::text
 FROM xpb_typed_report('g_f', ARRAY[1,2], ARRAY[3,4,5]) ORDER BY gkey" \
"SELECT coalesce(g1::text,'\\N') || '|' || coalesce(g2::text,'\\N')
        || ' n=' || count(*)
        || ' sums={' || coalesce(sum(s1)::text,'NULL')
        || ',' || coalesce(sum(s2)::text,'NULL')
        || ',' || coalesce(sum(s3)::text,'NULL') || '}'
 FROM g_f GROUP BY g1, g2 ORDER BY 1"

# A group whose every input is NULL must sum to NULL. Summing to 0 is the
# classic wrong answer and it is invisible unless a test says so out loud.
compare_sql "all-NULL group sums to NULL, not 0" \
"SELECT gkey || ' ' || coalesce(sums[1],'NULL')
 FROM xpb_typed_report('g_f', ARRAY[1,2], ARRAY[3]) WHERE gkey = '\\N|\\N'" \
"SELECT '\\N|\\N ' || coalesce(sum(s1)::text,'NULL')
 FROM g_f WHERE g1 IS NULL AND g2 IS NULL"

echo
echo "=== join NULL semantics ==="

"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
-- dimension: one NULL key (must never match), one NULL payload (ordinary)
CREATE TABLE j_d (k int4, payload int4);
INSERT INTO j_d VALUES (1, 100), (2, 200), (3, NULL), (NULL, 999);

CREATE TABLE j_f (jk int4, g1 int4, s1 int4);
INSERT INTO j_f VALUES
 (1,    7, 1),      -- matches
 (1,    7, 2),      -- matches, same group
 (2,    8, 4),      -- matches
 (3,    9, 8),      -- matches a NULL payload
 (4,    9, 16),     -- no such dimension key
 (NULL, 7, 32),     -- NULL fact key: matches nothing, including j_d's NULL
 (NULL, 7, 64);
SQL

compare_sql "inner join, NULL on both sides" \
"SELECT gkey || ' n=' || nrows || ' ' || coalesce(sums[1],'NULL')
 FROM xpb_typed_report('j_f', ARRAY[2], ARRAY[3], 'j_d', 1) ORDER BY gkey" \
"SELECT coalesce(d.payload::text,'\\N') || '|' || coalesce(f.g1::text,'\\N')
        || ' n=' || count(*) || ' ' || coalesce(sum(f.s1)::text,'NULL')
 FROM j_f f JOIN j_d d ON d.k = f.jk
 GROUP BY d.payload, f.g1 ORDER BY 1"

# Stated separately because it is the rule most easily got wrong: a NULL fact
# key must not find the dimension's NULL key. If it did, rows 6 and 7 would
# join to payload 999 and appear above.
got=$("${PSQL[@]}" -c "SELECT coalesce(sum(nrows),0)
                       FROM xpb_typed_report('j_f', ARRAY[2], ARRAY[3], 'j_d', 1)")
want=$("${PSQL[@]}" -c "SELECT count(*) FROM j_f f JOIN j_d d ON d.k = f.jk")
if [ "$got" = "$want" ] && [ "$got" = "4" ]; then
    echo "  PASS  NULL key joins to nothing (4 of 7 rows survive)"
    pass_count=$((pass_count + 1))
else
    echo "  FAIL  NULL join: batch=$got postgresql=$want (expected 4)"
    fail=1
fi

echo
echo "=== ZLFS v2: typed zone, same answers as heap ==="

"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
-- a nullable int8 column in a zone: not representable in the old format
CREATE TABLE z_f (k int4 NOT NULL, g int8, s int4);
INSERT INTO z_f SELECT g,
                       CASE WHEN g % 5 = 0 THEN NULL ELSE (g % 3)::int8 * 5000000000 END,
                       CASE WHEN g % 7 = 0 THEN NULL ELSE g END
                FROM generate_series(1, 3000) g;
SQL
"${PSQL[@]}" -c "SELECT zlfs_build_zone('z_f', '1,2,3', 1, 3000)" >/dev/null 2>&1

Z_BATCH="SELECT gkey || ' ' || nrows || ' ' || coalesce(sums[1],'NULL')
         FROM xpb_typed_report('z_f', ARRAY[2], ARRAY[3], NULL, NULL, '%s') ORDER BY gkey"
Z_PG="SELECT coalesce(g::text,'\\N') || ' ' || count(*) || ' ' || coalesce(sum(s)::text,'NULL')
      FROM z_f GROUP BY g ORDER BY 1"

compare_sql "zone carries nullable int8" "$(printf "$Z_BATCH" zlfs)" "$Z_PG"
compare_sql "heap deform agrees with the zone" "$(printf "$Z_BATCH" deform)" "$Z_PG"

# An old-format file must be refused by name, never reinterpreted. Patching
# the version field to the untyped value is the whole test: if the reader
# accepted it, the int32 payload would be read as typed columns.
zpath=$("${PSQL[@]}" -c "SELECT setting || '/zlfs' FROM pg_settings WHERE name='data_directory'")
zfile=$(ls "$zpath"/zone_*.zlfs 2>/dev/null | head -1)
if [ -n "$zfile" ] && [ -w "$zfile" ]; then
    cp "$zfile" "$zfile.bak"
    # version is the second uint32 of the header
    printf '\2\0\0\0' | dd of="$zfile" bs=1 seek=4 conv=notrunc status=none
    out=$("${PSQL[@]}" -c "SELECT count(*) FROM xpb_typed_report('z_f', ARRAY[2], ARRAY[3], NULL, NULL, 'zlfs')" 2>&1)
    mv "$zfile.bak" "$zfile"

    if grep -qi "format version 2" <<<"$out"; then
        echo "  PASS  untyped zone file refused by name"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  untyped zone file not refused: $(tr '\n' ' ' <<<"$out" | cut -c1-100)"
        fail=1
    fi
else
    echo "  SKIP  untyped-file rejection (zone file not writable from here)"
fi

echo
echo "=== pgColumnar source: int8 and validity ==="

if "${PSQL[@]}" -c "CREATE EXTENSION IF NOT EXISTS pgcolumnar" >/dev/null 2>&1; then
    "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
-- int8 past 2^31 so a source that truncated to int32 would collapse the
-- groups together, and NULLs in both a key and a summed column
CREATE TABLE p_f (k int4 NOT NULL, g int8, s int8, t int4);
INSERT INTO p_f SELECT g,
                       CASE WHEN g % 5 = 0 THEN NULL ELSE (g % 3)::int8 * 5000000000 END,
                       CASE WHEN g % 7 = 0 THEN NULL ELSE g::int8 * 1000000 END,
                       g % 4
                FROM generate_series(1, 3000) g;
CREATE TABLE p_c (LIKE p_f) USING pgcolumnar;
INSERT INTO p_c SELECT * FROM p_f;
SQL

    P_PG="SELECT coalesce(g::text,'\\N') || ' ' || count(*) || ' ' || coalesce(sum(s)::text,'NULL')
          FROM p_f GROUP BY g ORDER BY 1"
    P_B="SELECT gkey || ' ' || nrows || ' ' || coalesce(sums[1],'NULL')
         FROM xpb_typed_report('%s', ARRAY[2], ARRAY[3], NULL, NULL, '%s') ORDER BY gkey"

    compare_sql "pgColumnar carries int8 keys and NULLs" \
        "$(printf "$P_B" p_c pgcolumnar)" "$P_PG"
    compare_sql "heap deform agrees on the same rows" \
        "$(printf "$P_B" p_f deform)" "$P_PG"

    # An all-NULL column through the columnar source: the dense stream is
    # empty, so a walk that advanced its present counter on a NULL would run
    # off the end of it.
    "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE p_allnull_f (k int4 NOT NULL, g int4, s int8);
INSERT INTO p_allnull_f SELECT g, NULL, NULL FROM generate_series(1, 500) g;
CREATE TABLE p_allnull_c (LIKE p_allnull_f) USING pgcolumnar;
INSERT INTO p_allnull_c SELECT * FROM p_allnull_f;
SQL
    compare_sql "pgColumnar all-NULL column" \
        "SELECT gkey || ' ' || nrows || ' ' || coalesce(sums[1],'NULL')
         FROM xpb_typed_report('p_allnull_c', ARRAY[2], ARRAY[3], NULL, NULL, 'pgcolumnar')" \
        "SELECT '\\N ' || count(*) || ' ' || coalesce(sum(s)::text,'NULL') FROM p_allnull_f"

    # numeric must be refused by name, not read as if it were fixed width
    "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE p_num_c (k int4 NOT NULL, d numeric(18,2) NOT NULL) USING pgcolumnar;
INSERT INTO p_num_c SELECT g, (g/100.0)::numeric(18,2) FROM generate_series(1,100) g;
SQL
    out=$("${PSQL[@]}" -c "SELECT count(*) FROM xpb_typed_report('p_num_c', ARRAY[1], ARRAY[2], NULL, NULL, 'pgcolumnar')" 2>&1)
    if grep -q "cannot carry" <<<"$out"; then
        echo "  PASS  pgColumnar refuses numeric by name"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  pgColumnar numeric not refused: $(tr '\n' ' ' <<<"$out" | cut -c1-90)"
        fail=1
    fi
else
    echo "  SKIP  pgcolumnar extension not available"
fi

echo
echo "=== heap source: int8 through the fixed-offset path ==="

# The fixed path used to be int4-only. int8 values past 2^31 on both signs:
# a path that truncated to int32 could not reproduce these.
"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE f_i8 (a int4 NOT NULL, b int8 NOT NULL, c int8 NOT NULL, note text);
INSERT INTO f_i8 VALUES
    (1,  2147483648, -2147483649, 'x'),
    (2,  5000000000, -5000000000, NULL),
    (3,  9223372036854775807, -9223372036854775808, ''),
    (4,  0, 0, 'y');
SQL
compare "int8 via the fixed-offset path" f_i8 "ARRAY[1,2,3]" fixed \
        "(0,a::text),(1,b::text),(2,c::text)"
compare "int8 via the deform path, same rows" f_i8 "ARRAY[1,2,3]" deform \
        "(0,a::text),(1,b::text),(2,c::text)"

# A varlena AFTER the projection leaves the fixed prefix addressable, so the
# fixed path must still be eligible -- the guard constrains the prefix, not
# the whole tuple.
compare "fixed prefix, varlena after it" f_i8 "ARRAY[1,2]" fixed \
        "(0,a::text),(1,b::text)"

echo "=== the whole row shape, end to end ==="

# Everything at once, on the layout that used to corrupt: the varlena sits at
# attnum 2, so every column that matters is read from AFTER it. int8 keys past
# 2^31, a nullable int8 group key, nullable numeric sums, a dimension with a
# NULL key and a NULL payload, and fact rows whose key is absent from it.
"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE a_reg (
    period      int4 NOT NULL,          -- 1
    comment     text,                   -- 2  varlena, NULL, never requested
    company_key int4 NOT NULL,          -- 3
    account_key int8 NOT NULL,          -- 4
    quantity    int8,                   -- 5  NULL
    debit       numeric(18,2),          -- 6  NULL
    credit      numeric(18,2) NOT NULL  -- 7
);
INSERT INTO a_reg
SELECT  g % 12 + 1,
        CASE WHEN g % 4 = 0 THEN NULL WHEN g % 4 = 1 THEN '' ELSE repeat('c', g % 30) END,
        g % 5 + 1,
        (g % 7)::int8 * 3000000000,
        CASE WHEN g % 9 = 0 THEN NULL ELSE (g % 11)::int8 * 1000000000 END,
        CASE WHEN g % 6 = 0 THEN NULL ELSE ((g % 977) / 100.0)::numeric(18,2) END,
        ((g % 383) / 100.0)::numeric(18,2)
FROM generate_series(1, 20000) g;

CREATE TABLE a_dim (k int4, payload int4);
INSERT INTO a_dim VALUES (1, 100), (2, 200), (3, NULL), (NULL, 999);
SQL
"${PSQL[@]}" -c "SELECT zlfs_build_zone('a_reg', '1,4,5,3', 1, 12)" >/dev/null 2>&1

compare_sql "join + int8 keys + NULL keys + numeric sums, past a varlena" \
"SELECT md5(string_agg(gkey||':'||nrows||':'||coalesce(sums[1],'N')||':'||coalesce(sums[2],'N'),
                       ',' ORDER BY gkey)) || ' groups=' || count(*)
 FROM xpb_typed_report('a_reg', ARRAY[4,5], ARRAY[6,7], 'a_dim', 3, 'deform')" \
"SELECT md5(string_agg(gkey||':'||nrows||':'||coalesce(d,'N')||':'||coalesce(c,'N'),
                       ',' ORDER BY gkey)) || ' groups=' || count(*)
 FROM (SELECT coalesce(d.payload::text,'\\N')||'|'||r.account_key||'|'
              ||coalesce(r.quantity::text,'\\N') AS gkey,
              count(*) AS nrows, sum(r.debit)::text AS d, sum(r.credit)::text AS c
       FROM a_reg r JOIN a_dim d ON d.k = r.company_key
       GROUP BY d.payload, r.account_key, r.quantity) s"

# The same pipeline from a zone. Integer columns only -- a zone carries no
# numeric -- which is a stated limitation, not a silent omission.
compare_sql "same pipeline served from a ZLFS zone" \
"SELECT md5(string_agg(gkey||':'||nrows||':'||coalesce(sums[1],'N'), ',' ORDER BY gkey))
        || ' groups=' || count(*)
 FROM xpb_typed_report('a_reg', ARRAY[4,5], ARRAY[5], NULL, NULL, 'zlfs')" \
"SELECT md5(string_agg(gkey||':'||nrows||':'||coalesce(q,'N'), ',' ORDER BY gkey))
        || ' groups=' || count(*)
 FROM (SELECT account_key||'|'||coalesce(quantity::text,'\\N') AS gkey,
              count(*) AS nrows, sum(quantity)::text AS q
       FROM a_reg GROUP BY account_key, quantity) s"

echo
if [ $fail -eq 0 ]; then
    echo "contract_tests: ALL PASS ($pass_count cases)"
else
    echo "contract_tests: FAILURES"
fi
exit $fail
