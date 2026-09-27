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
# THIS test's own zone, by its relation oid. The ZLFS registry is global to the
# data directory, so picking the first zone_*.zlfs alphabetically could pick up
# -- and patch the header of -- a zone belonging to another database entirely,
# which made this case pass or fail depending on what else had run first.
zoid=$("${PSQL[@]}" -c "SELECT oid FROM pg_class WHERE relname='z_f'")
zfile=$(ls "$zpath"/zone_${zoid}_*.zlfs 2>/dev/null | head -1)
if [ -n "$zfile" ] && [ -w "$zfile" ]; then
    cp "$zfile" "$zfile.bak"
    # version is the second uint32 of the header
    printf '\2\0\0\0' | dd of="$zfile" bs=1 seek=4 conv=notrunc status=none
    out=$("${PSQL[@]}" -c "SELECT count(*) FROM xpb_typed_report('z_f', ARRAY[2], ARRAY[3], NULL, NULL, 'zlfs')" 2>&1 \
          | grep -v 'cannot validate schema')
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

echo
echo "=== projected heap path (experimental, benchmark 05-C) ==="
echo "    walks the tuple by PostgreSQL's rules, materializes only what was asked"

"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
-- varlena BEFORE and BETWEEN requested attributes, NULLs in both positions,
-- mixed widths, and int8 at both extremes
CREATE TABLE pj (
    a int4 NOT NULL,      -- 1  requested
    v1 text,              -- 2  varlena before a requested attr, 25% NULL
    b int8 NOT NULL,      -- 3  requested
    v2 text,              -- 4  varlena BETWEEN requested attrs
    n1 int4,              -- 5  nullable, not requested
    c int8,               -- 6  requested, nullable
    d numeric(18,2),      -- 7  numeric, not requested
    e int4 NOT NULL       -- 8  requested
);
INSERT INTO pj
SELECT g,
       CASE WHEN g % 4 = 0 THEN NULL WHEN g % 4 = 1 THEN '' ELSE repeat('v', g % 31) END,
       CASE WHEN g % 5 = 0 THEN 9223372036854775807
            WHEN g % 5 = 1 THEN -9223372036854775808
            ELSE g::int8 * 4000000000 END,
       CASE WHEN g % 3 = 0 THEN NULL ELSE repeat('w', g % 17) END,
       CASE WHEN g % 6 = 0 THEN NULL ELSE g END,
       CASE WHEN g % 7 = 0 THEN NULL ELSE g::int8 * 1000000 END,
       CASE WHEN g % 8 = 0 THEN NULL ELSE (g / 100.0)::numeric(18,2) END,
       g * 3
FROM generate_series(1, 3000) g;

-- every nullable attribute NULL in every row
CREATE TABLE pj_allnull (a int4 NOT NULL, v text, n int4, b int8 NOT NULL);
INSERT INTO pj_allnull SELECT g, NULL, NULL, g::int8 * 5000000000
FROM generate_series(1, 500) g;

-- an external toasted value ahead of a requested fixed-width column
CREATE TABLE pj_toast (a int4 NOT NULL, big text, b int8 NOT NULL);
INSERT INTO pj_toast SELECT g, repeat('abcdefgh', 3000 * g), g::int8 * 6000000000
FROM generate_series(1, 6) g;
SQL

# NULL is rendered by the harness, not here -- an extra coalesce would
# escape differently on the two sides and compare a literal against a NULL.
PJ="(0,a::text),(1,b::text),(2,c::text),(3,e::text)"
compare "projected: varlena before and between requested attrs" \
        pj "ARRAY[1,3,6,8]" projected "$PJ"
compare "deform: same rows, same projection" \
        pj "ARRAY[1,3,6,8]" deform "$PJ"

compare "projected: all-NULL attributes in every row" \
        pj_allnull "ARRAY[1,4]" projected "(0,a::text),(1,b::text)"
compare "projected: external toasted varlena before a requested attr" \
        pj_toast "ARRAY[1,3]" projected "(0,a::text),(1,b::text)"
compare "projected: the toasted column itself, when requested" \
        pj_toast "ARRAY[1,2,3]" projected "(0,a::text),(1,big),(2,b::text)"
compare "projected: int4/int8 mixed, INT64 extremes" \
        t_i8 "ARRAY[1,2]" projected "(0,a::text),(1,b::text)"
compare "projected: nullable int4 in the middle" \
        v_i4null "ARRAY[1,2,3]" projected "(0,a::text),(1,b::text),(2,c::text)"
compare "projected: NULL across a batch boundary" \
        v_span "ARRAY[1,2]" projected "(0,a::text),(1,b::text)"
compare "projected: numeric, exact decimal scale" \
        n_exact "ARRAY[1,2,3]" projected "(0,a::text),(1,d::text),(2,e::text)"

# The unused toasted value must be stepped over, not detoasted. A detoast of
# 144 kB per row would be visible; this asserts the result is right while the
# column is skipped, and the timing arm in 05-C reports the counters.
compare "projected: skipping a toasted column gives the same answer" \
        pj_toast "ARRAY[1,3]" deform "(0,a::text),(1,b::text)"

# An unused EXTERNAL varlena must be stepped over, not detoasted. Observed
# through the TOAST relation's block counters rather than by instrumenting the
# hot path: locating the next attribute needs only the 18-byte pointer's
# length header, so touching the toast relation at all would mean the value
# was followed.
"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE pj_ext (a int4 NOT NULL, big text, b int8 NOT NULL);
INSERT INTO pj_ext SELECT g, repeat('abcdefgh', 40000), g::int8 * 7
FROM generate_series(1, 200) g;
SQL
skipped=$("${PSQL[@]}" <<'SQL' 2>/dev/null | tail -1
SELECT pg_stat_reset(); SELECT pg_sleep(0.3);
SELECT count(*) FROM xpb_contract_probe('pj_ext', ARRAY[1,3], 'projected');
SELECT pg_stat_force_next_flush(); SELECT pg_sleep(0.3);
SELECT coalesce(toast_blks_hit + toast_blks_read, 0) FROM pg_statio_all_tables WHERE relname='pj_ext';
SQL
)
wanted=$("${PSQL[@]}" <<'SQL' 2>/dev/null | tail -1
SELECT pg_stat_reset(); SELECT pg_sleep(0.3);
SELECT count(*) FROM xpb_contract_probe('pj_ext', ARRAY[1,2,3], 'projected');
SELECT pg_stat_force_next_flush(); SELECT pg_sleep(0.3);
SELECT coalesce(toast_blks_hit + toast_blks_read, 0) FROM pg_statio_all_tables WHERE relname='pj_ext';
SQL
)
if [ "$skipped" = "0" ] && [ "${wanted:-0}" -gt 0 ]; then
    echo "  PASS  unused external varlena is NOT detoasted (toast blocks: $skipped skipped, $wanted requested)"
    pass_count=$((pass_count + 1))
else
    echo "  FAIL  detoast check: skipped touched $skipped toast blocks, requested touched $wanted"
    fail=1
fi

echo
echo "=== early-predicate projected path (experimental, benchmark 05-D) ==="
echo "    rejects a row the moment the predicate attribute is known"

"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
-- predicate at attnum 1, ahead of a varlena and of the other requested
-- columns, so an early reject can skip the rest of the tuple
CREATE TABLE ep (
    p int4 NOT NULL,      -- 1  predicate
    note text,            -- 2  varlena after the predicate
    q int8 NOT NULL,      -- 3  requested
    r int4,               -- 4  requested, nullable
    big text              -- 5  varlena
);
INSERT INTO ep SELECT g % 12 + 1,
       CASE WHEN g % 4 = 0 THEN NULL ELSE repeat('n', g % 23) END,
       g::int8 * 4000000000,
       CASE WHEN g % 5 = 0 THEN NULL ELSE g END,
       CASE WHEN g % 3 = 0 THEN NULL ELSE repeat('b', g % 11) END
FROM generate_series(1, 4000) g;

-- nullable predicate column: BETWEEN on NULL is unknown, so WHERE rejects
CREATE TABLE ep_null (p int4, q int8 NOT NULL, note text);
INSERT INTO ep_null SELECT CASE WHEN g % 7 = 0 THEN NULL ELSE g % 12 + 1 END,
       g::int8 * 3000000000, repeat('x', g % 13)
FROM generate_series(1, 2000) g;

-- an external toasted value AFTER the predicate: an early reject must not
-- even reach it
CREATE TABLE ep_toast (p int4 NOT NULL, big text, q int8 NOT NULL);
INSERT INTO ep_toast SELECT g % 12 + 1, repeat('abcdefgh', 40000), g::int8 * 7
FROM generate_series(1, 200) g;
SQL

# $1 name  $2 table  $3 attnos  $4 lo  $5 hi  $6 SQL column list  $7 WHERE
ep_case() {
    local name="$1" tbl="$2" att="$3" lo="$4" hi="$5" cols="$6" where="$7"
    local e l p
    e=$("${PSQL[@]}" -c "SELECT md5(coalesce(string_agg(col||':'||coalesce(val,E'\\\\N'), ',' ORDER BY col, coalesce(val,E'\\\\N')),'')) FROM xpb_contract_probe('$tbl', $att, 'projected-early', $lo, $hi)" 2>&1)
    l=$("${PSQL[@]}" -c "SELECT md5(coalesce(string_agg(col||':'||coalesce(val,E'\\\\N'), ',' ORDER BY col, coalesce(val,E'\\\\N')),'')) FROM xpb_contract_probe('$tbl', $att, 'projected', $lo, $hi)" 2>&1)
    p=$("${PSQL[@]}" -c "SELECT md5(coalesce(string_agg(col||':'||coalesce(val,E'\\\\N'), ',' ORDER BY col, coalesce(val,E'\\\\N')),'')) FROM (SELECT * FROM $tbl WHERE $where) t, LATERAL (VALUES $cols) AS v(col,val)" 2>&1)
    if [ "$e" = "$l" ] && [ "$e" = "$p" ] && [ -n "$e" ] && [[ ! "$e" =~ ERROR ]]; then
        echo "  PASS  $name"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  $name"
        echo "        early=$(head -c 34 <<<"$e") late=$(head -c 34 <<<"$l") sql=$(head -c 34 <<<"$p")"
        fail=1
    fi
}

EPC="(0,p::text),(1,q::text),(2,r::text)"
ep_case "accepts all (1..12)"        ep "ARRAY[1,3,4]" 1 12 "$EPC" "p BETWEEN 1 AND 12"
ep_case "rejects all (90..91)"       ep "ARRAY[1,3,4]" 90 91 "$EPC" "p BETWEEN 90 AND 91"
ep_case "accepts a subset (1..1)"    ep "ARRAY[1,3,4]" 1 1  "$EPC" "p BETWEEN 1 AND 1"
ep_case "accepts a subset (4..7)"    ep "ARRAY[1,3,4]" 4 7  "$EPC" "p BETWEEN 4 AND 7"
ep_case "NULL predicate column rejects" ep_null "ARRAY[1,2]" 1 12 \
        "(0,p::text),(1,q::text)" "p BETWEEN 1 AND 12"
ep_case "predicate before a toasted varlena" ep_toast "ARRAY[1,3]" 1 1 \
        "(0,p::text),(1,q::text)" "p BETWEEN 1 AND 1"
ep_case "accepted rows keep int8 and NULL validity" ep "ARRAY[1,3,4]" 2 3 \
        "$EPC" "p BETWEEN 2 AND 3"

# An early reject before a toasted column must not touch the toast relation.
skipped=$("${PSQL[@]}" <<'SQL' 2>/dev/null | tail -1
SELECT pg_stat_reset(); SELECT pg_sleep(0.3);
SELECT count(*) FROM xpb_contract_probe('ep_toast', ARRAY[1,3], 'projected-early', 90, 91);
SELECT pg_stat_force_next_flush(); SELECT pg_sleep(0.3);
SELECT coalesce(toast_blks_hit + toast_blks_read, 0) FROM pg_statio_all_tables WHERE relname='ep_toast';
SQL
)
if [ "$skipped" = "0" ]; then
    echo "  PASS  early reject reads no toast blocks ($skipped)"
    pass_count=$((pass_count + 1))
else
    echo "  FAIL  early reject touched $skipped toast blocks"
    fail=1
fi

# The early path needs a predicate; asking without one must error, not
# silently behave like the late path.
out=$("${PSQL[@]}" -c "SELECT count(*) FROM xpb_contract_probe('ep', ARRAY[1,3], 'projected-early')" 2>&1)
if grep -q "early-predicate path needs a predicate" <<<"$out"; then
    echo "  PASS  early path without a predicate errors rather than falling back"
    pass_count=$((pass_count + 1))
else
    echo "  FAIL  no-predicate early path not refused: $(tr '\n' ' ' <<<"$out" | cut -c1-80)"
    fail=1
fi

# The optional pred_lo/pred_hi pair means the probe cannot be STRICT, so the
# other arguments are no longer NULL-checked by the executor.  Unguarded they
# segfault the backend, which is how this was found.  Half a range is refused
# too: accepting it would silently measure the unfiltered path.
probe_refuses() {
    local what="$1"; shift
    local want="$1"; shift
    local out
    out=$("${PSQL[@]}" -c "$1" 2>&1)
    if grep -q "$want" <<<"$out"; then
        echo "  PASS  $what"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  $what: $(tr '\n' ' ' <<<"$out" | cut -c1-90)"
        fail=1
    fi
}
probe_refuses "NULL relname is refused, not dereferenced" "must not be NULL" \
    "SELECT count(*) FROM xpb_contract_probe(NULL, ARRAY[1], 'deform')"
probe_refuses "NULL attnos is refused, not dereferenced" "must not be NULL" \
    "SELECT count(*) FROM xpb_contract_probe('ep', NULL::int[], 'deform')"
probe_refuses "NULL path is refused, not dereferenced" "must not be NULL" \
    "SELECT count(*) FROM xpb_contract_probe('ep', ARRAY[1], NULL)"
probe_refuses "half a predicate range is refused" "must both be given" \
    "SELECT count(*) FROM xpb_contract_probe('ep', ARRAY[1,3], 'projected-early', 1, NULL)"
probe_refuses "half a predicate range is refused (other side)" "must both be given" \
    "SELECT count(*) FROM xpb_contract_probe('ep', ARRAY[1,3], 'projected-early', NULL, 12)"

# The backend must still be alive after all of that.
if [ "$("${PSQL[@]}" -c 'SELECT 42')" = "42" ]; then
    echo "  PASS  backend survived the NULL-argument probes"
    pass_count=$((pass_count + 1))
else
    echo "  FAIL  backend did not survive the NULL-argument probes"
    fail=1
fi

echo
echo "=== benchmark-only source-mode forcing ==="
echo "    (proved by a counter the fixed path cannot increment, not by the flag)"

if "${PSQL[@]}" -c "SELECT 1 FROM pg_proc WHERE proname='xpb_v2_register_report'" \
        2>/dev/null | grep -q 1; then
    "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE reg2_fixed (period int4 NOT NULL, company_key int4 NOT NULL,
    account_key int8 NOT NULL, debit_cents int8 NOT NULL, credit_cents int8 NOT NULL,
    quantity int8, debit numeric(18,2), credit numeric(18,2) NOT NULL, comment text);
CREATE TABLE reg2_bad (period int4 NOT NULL, comment text, company_key int4 NOT NULL,
    account_key int8 NOT NULL, quantity int8, debit numeric(18,2),
    credit numeric(18,2) NOT NULL, debit_cents int8 NOT NULL, credit_cents int8 NOT NULL);
INSERT INTO reg2_fixed SELECT g % 12 + 1, g % 5 + 1, 4000000000 + g % 200,
       g * 3, g * 2, NULL, NULL, 1.00, CASE WHEN g % 3 = 0 THEN NULL ELSE 'c' END
FROM generate_series(1, 5000) g;
INSERT INTO reg2_bad SELECT period, comment, company_key, account_key, quantity,
       debit, credit, debit_cents, credit_cents FROM reg2_fixed;
CREATE TABLE dim_company (company_key int4 NOT NULL, company_group int4 NOT NULL);
INSERT INTO dim_company SELECT id, (id-1)/10+1 FROM generate_series(1,50) id;
CREATE TABLE dim_account2 (account_key int8 NOT NULL, account_group int4 NOT NULL);
INSERT INTO dim_account2 SELECT 4000000000+id, (id-1)/50+1 FROM generate_series(0,200) id;
SQL

    # DEFORM forced on a layout where FIXED is eligible must genuinely deform
    d=$("${PSQL[@]}" -c "SELECT count(*) FROM xpb_v2_register_report(1,12,'fixedlayout-deform')" 2>&1 \
        | sed -n 's/.*tuples_deformed=\([0-9]*\).*/\1/p')
    f=$("${PSQL[@]}" -c "SELECT count(*) FROM xpb_v2_register_report(1,12,'fixedlayout-fixed')" 2>&1 \
        | sed -n 's/.*tuples_deformed=\([0-9]*\).*/\1/p')
    if [ "$d" = "5000" ] && [ "$f" = "0" ]; then
        echo "  PASS  forced deform deforms (5000 tuples), forced fixed does not (0)"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  mode forcing: deform counted '$d' (want 5000), fixed counted '$f' (want 0)"
        fail=1
    fi

    # FIXED on an incompatible layout must error, naming the reason
    out=$("${PSQL[@]}" -c "SELECT count(*) FROM xpb_v2_register_report(1,12,'bad-fixed')" 2>&1)
    if grep -q 'column "comment" is variable-width' <<<"$out"; then
        echo "  PASS  forced fixed refuses an unaddressable layout, by reason"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  bad-fixed not refused: $(tr '\n' ' ' <<<"$out" | cut -c1-90)"
        fail=1
    fi

    # the two layouts must agree, which is what makes B-C meaningful
    compare_sql "both layouts give the same report" \
      "SELECT company_group||','||account_group||','||company_key||','||debit_turnover
       FROM xpb_v2_register_report(1,12,'fixedlayout-fixed') ORDER BY 1" \
      "SELECT company_group||','||account_group||','||company_key||','||debit_turnover
       FROM xpb_v2_register_report(1,12,'bad-deform') ORDER BY 1"
else
    echo "  SKIP  xpb_v2_register_report not installed"
fi

echo
echo "=== growing group hash (Hash Aggregate Growth v1) ==="

if "${PSQL[@]}" -c "SELECT 1 FROM pg_proc WHERE proname='xpb_grp_test_policy'" \
        2>/dev/null | grep -q 1; then

    # One company and 256 accounts, so the group count is exactly the number of
    # distinct account_group values and can be dialled to hit the growth
    # threshold on the nose. Small initial capacity via the test hook, so that
    # several growths cost 129 groups rather than a hundred thousand.
    "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE reg2_card (period int4 NOT NULL, company_key int4 NOT NULL,
    account_key int8 NOT NULL, debit_cents int8 NOT NULL, credit_cents int8 NOT NULL,
    quantity int8, debit numeric(18,2), credit numeric(18,2) NOT NULL, comment text);
CREATE TABLE dim_company_c (company_key int4 NOT NULL, company_group int4 NOT NULL);
CREATE TABLE dim_account_c (account_key int8 NOT NULL, account_group int4 NOT NULL);
INSERT INTO dim_company_c VALUES (1, 7);
-- 4 rows per (company, account) pair, and a NULL-keyed dimension row plus a
-- fact row pointing at an absent account, so grouping/NULL semantics are
-- exercised at every growth count rather than only on the happy path.
-- 512 accounts, which is inside the dimension hash's own 768-key limit:
-- overflowing THAT is a different failure and has its own test below.
INSERT INTO reg2_card
SELECT (g % 12) + 1, 1, (g % 512) + 1, g * 3, g * 2,
       NULL, NULL, 1.00, CASE WHEN g % 3 = 0 THEN NULL ELSE 'c' END
FROM generate_series(0, 2047) g;
INSERT INTO reg2_card VALUES (1, 1, 999999, 5, 1, NULL, NULL, 1.00, NULL);
SQL

    # groups = k, because there is exactly one company.
    card_k() {
        "${PSQL[@]}" -c "TRUNCATE dim_account_c;
             INSERT INTO dim_account_c SELECT g + 1, (g % $1) + 1
             FROM generate_series(0, 511) g;" >/dev/null
    }

    # The policy hook is a process-local static and every psql invocation is a
    # new backend, so it has to be set in the SAME connection as the report it
    # governs. POL does that; forgetting it silently measures the production
    # policy instead, which is how the first version of these tests failed.
    POL="SELECT xpb_grp_test_policy(64, 0);"
    GUCS="SET jit=off; SET max_parallel_workers_per_gather=0;"

    # Every growth count must still agree with PostgreSQL, per group, not just
    # on a grand total: a rehash that lost or duplicated a group would keep the
    # row count right while splitting one group's sums across two slots.
    grow_case() {   # grow_case <label> <k> <want_growths>
        local label="$1" k="$2" want="$3" line g b p
        card_k "$k"
        line=$("${PSQL[@]}" -c "$GUCS $POL
                SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1 \
               | sed -n 's/^NOTICE:  v2_register_report //p')
        g=$(sed -n 's/.*[ =]grp_growths=\([0-9]*\).*/\1/p' <<<"$line")
        b=$("${PSQL[@]}" -c "$GUCS $POL
             SELECT md5(string_agg(company_group||','||account_group||','||company_key
                        ||','||debit_turnover||','||credit_turnover||','||net_turnover,
                        '|' ORDER BY company_group, account_group, company_key))
             FROM xpb_v2_register_report(1,12,'card')" 2>/dev/null | tail -1)
        p=$("${PSQL[@]}" -c "SELECT md5(string_agg(cg||','||ag||','||ck||','||d||','||c||','||n,
                    '|' ORDER BY cg, ag, ck)) FROM (
                 SELECT c.company_group cg, a.account_group ag, r.company_key ck,
                        sum(r.debit_cents)::bigint d, sum(r.credit_cents)::bigint c,
                        sum(r.debit_cents - r.credit_cents)::bigint n
                 FROM reg2_card r
                 JOIN dim_company_c c ON c.company_key = r.company_key
                 JOIN dim_account_c a ON a.account_key = r.account_key
                 WHERE r.period BETWEEN 1 AND 12
                 GROUP BY 1,2,3) s" 2>/dev/null | tail -1)
        if [ "$g" = "$want" ] && [ -n "$b" ] && [ "$b" = "$p" ]; then
            echo "  PASS  $label: $g growth(s), per-group result matches PostgreSQL"
            pass_count=$((pass_count + 1))
        else
            echo "  FAIL  $label: growths=$g (want $want) batch=${b:0:12} sql=${p:0:12}"
            fail=1
        fi
    }

    # Initial capacity 64 => grows when a NEW group is needed and 32 are held.
    # Section 12: the threshold, exactly. The decision reads ngroups and
    # capacity only, so it cannot depend on where probing happened to stop.
    grow_case "31 groups, below half of 64"    31 0
    grow_case "32 groups, exactly half of 64"  32 0
    grow_case "33 groups, one past half"       33 1

    # Section 11: 0 / 1 / 2 / 3+ growths.
    grow_case "65 groups"                      65 2
    grow_case "129 groups"                     129 3
    grow_case "256 groups, exactly half of 512" 256 3
    grow_case "257 groups, one past half"       257 4
    grow_case "512 groups"                      512 4

    # Load factor must never exceed 0.5, and after four growths the context
    # must still hold exactly one table (section 8). Both read from the same
    # run, and grp_cxt_bytes is the memory system's own accounting rather than
    # this file's arithmetic: had any predecessor survived, the context would
    # hold roughly twice the live table instead of it plus block overhead.
    # Initial capacity 512 so that every generation (16 KB, then 32 KB) is
    # larger than AllocSet's chunk limit and therefore its own block, which a
    # pfree returns outright. Below that limit a freed table goes on a
    # freelist the context keeps, and the context's figure would look like a
    # leak when nothing had leaked.
    card_k 512
    line=$("${PSQL[@]}" -c "$GUCS SELECT xpb_grp_test_policy(512, 0);
            SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1 \
           | sed -n 's/^NOTICE:  v2_register_report //p')
    lf=$(sed -n 's/.*grp_load_factor=\([0-9.]*\).*/\1/p' <<<"$line")
    cur=$(sed -n 's/.*[ =]grp_bytes=\([0-9]*\).*/\1/p' <<<"$line")
    cxt=$(sed -n 's/.*[ =]grp_cxt_bytes=\([0-9]*\).*/\1/p' <<<"$line")
    gro=$(sed -n 's/.*[ =]grp_growths=\([0-9]*\).*/\1/p' <<<"$line")
    if [ -n "$lf" ] && awk "BEGIN{exit !($lf <= 0.5)}"; then
        echo "  PASS  load factor stays at or below 0.5 ($lf after $gro growths)"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  load factor $lf exceeds the 0.5 growth policy"
        fail=1
    fi
    pk=$(sed -n 's/.*grp_bytes_peak=\([0-9]*\).*/\1/p' <<<"$line")
    # Every generation summed: had the predecessor survived, the context would
    # have to hold at least this much. Releasing it leaves roughly the live
    # table plus one block of context overhead, comfortably under.
    allgen=$(( cur + cur / 2 ))
    if [ "$gro" -ge 1 ] && [ -n "$cxt" ] && [ "$cxt" -lt "$allgen" ]; then
        echo "  PASS  $gro growth(s) leave one live table ($cxt held, table $cur, all generations $allgen)"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  after $gro growths the context holds $cxt bytes; all generations total $allgen"
        fail=1
    fi
    # Peak must be exactly old+new at the last growth: the two tables really
    # are both live across a rehash, and that is reported rather than smoothed.
    if [ "$pk" = "$allgen" ]; then
        echo "  PASS  peak memory is old+new across the rehash ($pk for a $cur table)"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  peak memory $pk is not old+new ($allgen) for a $cur byte table"
        fail=1
    fi

    # Section 11/28: two reports in ONE backend. The second must start from the
    # initial capacity again and carry nothing over from the first -- run in a
    # single connection, or "starts fresh" would be true for the wrong reason.
    card_k 129
    line=$("${PSQL[@]}" -c "$GUCS $POL
            SELECT count(*) FROM xpb_v2_register_report(1,12,'card');
            TRUNCATE dim_account_c;
            INSERT INTO dim_account_c SELECT g + 1, (g % 31) + 1 FROM generate_series(0,511) g;
            SELECT count(*) FROM xpb_v2_register_report(1,12,'card');" 2>&1 \
           | sed -n 's/^NOTICE:  v2_register_report //p' | tail -1)
    ic=$(sed -n 's/.*grp_initial_cap=\([0-9]*\).*/\1/p' <<<"$line")
    oc=$(sed -n 's/.*[ =]grp_occupied=\([0-9]*\).*/\1/p' <<<"$line")
    gr=$(sed -n 's/.*[ =]grp_growths=\([0-9]*\).*/\1/p' <<<"$line")
    if [ "$ic" = "64" ] && [ "$oc" = "31" ] && [ "$gr" = "0" ]; then
        echo "  PASS  a later, smaller report in the same backend starts fresh at 64 with 31 groups"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  rescan state leaked: initial_cap=$ic groups=$oc growths=$gr (want 64/31/0)"
        fail=1
    fi

    # Section 13: a growth that cannot be satisfied must still fail cleanly.
    # Forced with a test-only ceiling rather than by inducing a real OOM.
    card_k 129
    out=$("${PSQL[@]}" -c "$GUCS SELECT xpb_grp_test_policy(64, 64);
           SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1)
    if grep -q "may not grow past 64 slots" <<<"$out"; then
        echo "  PASS  a refused growth errors, naming the limit and the groups held"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  refused growth not reported: $(tr '\n' ' ' <<<"$out" | cut -c1-90)"
        fail=1
    fi
    if [ "$("${PSQL[@]}" -c 'SELECT 42')" = "42" ]; then
        echo "  PASS  backend survives a refused growth"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  backend did not survive a refused growth"
        fail=1
    fi
    # A refused growth must not leave a half-grown table behind either: the
    # same backend, asked for a size that fits, must still answer correctly.
    card_k 31
    b=$("${PSQL[@]}" -c "$GUCS SELECT xpb_grp_test_policy(64, 64);
         SELECT count(*) FROM xpb_v2_register_report(1,12,'card');
         SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1 | grep -c "groups=31")
    if [ "$b" = "2" ]; then
        echo "  PASS  the backend still reports correctly after a refused growth"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  after a refused growth a fitting report did not succeed twice ($b)"
        fail=1
    fi

    # ---- probe high-water marks ----------------------------------------
    # Nothing asserted these before, and one counter was doing both jobs: it
    # reported the densest pre-growth chain while being read as a property of
    # the table that answered the query.
    #
    # This needs its own dataset. The threshold dataset above varies ONE key
    # component, and the group hash multiplies each key by an odd constant, so
    # consecutive keys land on a stride that never collides -- every chain is
    # length 1 at any load and a reset is invisible. Collisions need two key
    # components to vary, so here company_key does too.
    "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE probe_reg (period int4 NOT NULL, company_key int4 NOT NULL,
    account_key int8 NOT NULL, debit_cents int8 NOT NULL, credit_cents int8 NOT NULL,
    quantity int8, debit numeric(18,2), credit numeric(18,2) NOT NULL, comment text);
INSERT INTO probe_reg
SELECT (g % 12) + 1, (g % 16) + 1, ((g / 16) % 128) + 1, g * 3, g * 2,
       NULL, NULL, 1.00, NULL
FROM generate_series(0, 4095) g;
INSERT INTO dim_company_c SELECT g + 2, g + 2 FROM generate_series(0, 14) g;
ALTER TABLE reg2_card RENAME TO reg2_card_keep;
ALTER TABLE probe_reg RENAME TO reg2_card;
SQL
    # 16 companies x 128 account groups = 2048 groups from a 64-slot table:
    # five growths, and dense enough before each one to build real chains.
    card_k 128
    line=$("${PSQL[@]}" -c "$GUCS SELECT xpb_grp_test_policy(64, 0);
            SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1 \
           | sed -n 's/^NOTICE:  v2_register_report //p')
    pc=$(sed -n 's/.*grp_max_probe_current=\([0-9]*\).*/\1/p' <<<"$line")
    pl=$(sed -n 's/.*grp_max_probe_lifetime=\([0-9]*\).*/\1/p' <<<"$line")
    gw=$(sed -n 's/.*[ =]grp_growths=\([0-9]*\).*/\1/p' <<<"$line")
    if [ "$gw" -ge 1 ] && [ -n "$pc" ] && [ -n "$pl" ] && [ "$pc" -lt "$pl" ]; then
        echo "  PASS  after $gw growths the current probe mark ($pc) is below the lifetime mark ($pl)"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  probe marks after $gw growths: current=$pc lifetime=$pl (current must be lower)"
        fail=1
    fi
    # With no growth nothing has been reset, so the two must agree.
    line=$("${PSQL[@]}" -c "$GUCS SELECT xpb_grp_test_policy(16384, 0);
            SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1 \
           | sed -n 's/^NOTICE:  v2_register_report //p')
    pc=$(sed -n 's/.*grp_max_probe_current=\([0-9]*\).*/\1/p' <<<"$line")
    pl=$(sed -n 's/.*grp_max_probe_lifetime=\([0-9]*\).*/\1/p' <<<"$line")
    gw=$(sed -n 's/.*[ =]grp_growths=\([0-9]*\).*/\1/p' <<<"$line")
    if [ "$gw" = "0" ] && [ "$pc" = "$pl" ] && [ "$pc" -ge 1 ]; then
        echo "  PASS  with no growth the two probe marks agree ($pc)"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  no-growth probe marks: growths=$gw current=$pc lifetime=$pl"
        fail=1
    fi
    "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
ALTER TABLE reg2_card RENAME TO probe_reg;
ALTER TABLE reg2_card_keep RENAME TO reg2_card;
DELETE FROM dim_company_c WHERE company_key > 1;
SQL
    card_k 31

    # ---- int8 turnover overflow ----------------------------------------
    # The accumulator used to wrap: on inputs PostgreSQL refuses outright it
    # returned a negative turnover with no error at all. Three things are
    # asserted together, because the contract is "indistinguishable from
    # PostgreSQL's own int8 arithmetic":
    #
    #   PostgreSQL sum(int8)      -> exact numeric, a correct large value
    #   explicit cast to int8     -> bigint out of range
    #   xp_batch accumulator      -> bigint out of range, never a wrapped value
    "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE ovf_reg (period int4 NOT NULL, company_key int4 NOT NULL,
    account_key int8 NOT NULL, debit_cents int8 NOT NULL, credit_cents int8 NOT NULL,
    quantity int8, debit numeric(18,2), credit numeric(18,2) NOT NULL, comment text);
INSERT INTO ovf_reg SELECT 1, 1, 1, 4000000000000000000, 1, NULL, NULL, 1.00, NULL
FROM generate_series(1, 4) g;
SQL
    pg_exact=$("${PSQL[@]}" -c "SELECT sum(debit_cents) FROM ovf_reg")
    pg_cast=$("${PSQL[@]}" -c "SELECT sum(debit_cents)::int8 FROM ovf_reg" 2>&1)
    "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
ALTER TABLE reg2_card RENAME TO reg2_card_keep;
ALTER TABLE ovf_reg RENAME TO reg2_card;
SQL
    xp_out=$("${PSQL[@]}" -c "$GUCS SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1)
    "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
ALTER TABLE reg2_card RENAME TO ovf_reg;
ALTER TABLE reg2_card_keep RENAME TO reg2_card;
SQL
    if [ "$pg_exact" = "16000000000000000000" ] &&
       grep -q "bigint out of range" <<<"$pg_cast" &&
       grep -q "bigint out of range" <<<"$xp_out" &&
       ! grep -q -- "-2446744073709551616" <<<"$xp_out"; then
        echo "  PASS  int8 turnover overflow raises like PostgreSQL, never wraps"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  overflow: pg_exact=$pg_exact pg_cast=${pg_cast:0:40} xp=$(tr '\n' ' ' <<<"$xp_out" | cut -c1-70)"
        fail=1
    fi

    # The net identity is an invariant, NOT an overflow detector, and this is
    # the regression that keeps that from being forgotten again. These are the
    # exact values the unchecked accumulator produced for the rows above: both
    # sides of the identity wrapped identically, so the benchmark gate passed
    # a wrong answer. Any future gate must not rely on it to catch overflow.
    idh=$("${PSQL[@]}" -c "SELECT (-2446744073709551616::int8) - 4::int8 = -2446744073709551620::int8")
    if [ "$idh" = "t" ]; then
        echo "  PASS  net identity holds on wrapped values, so it cannot detect overflow"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  the wraparound identity regression no longer demonstrates its point ($idh)"
        fail=1
    fi

    # A dimension larger than its hash must never silently lose rows. The
    # mechanism has changed twice and the invariant has not: originally the
    # probe loop ran off the end of a full table and dropped the row, so every
    # fact row referencing that key vanished from the aggregate and the sums
    # came back smaller with no error; then it was made to refuse; and since
    # Dimension Hash Growth v1 it grows instead. So this asserts the property,
    # not the mechanism -- no error, a growth, and a join result that still
    # matches PostgreSQL row for row.
    "${PSQL[@]}" -c "INSERT INTO dim_account_c SELECT g + 1000, 1
                     FROM generate_series(1, 900) g" >/dev/null
    out=$("${PSQL[@]}" -c "$GUCS SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1)
    g2=$(sed -n 's/.*dim2_growths=\([0-9]*\).*/\1/p' <<<"$out")
    ob=$("${PSQL[@]}" -c "$GUCS
          SELECT md5(string_agg(company_group||','||account_group||','||company_key
                     ||','||debit_turnover, '|'
                     ORDER BY company_group, account_group, company_key))
          FROM xpb_v2_register_report(1,12,'card')" 2>/dev/null | tail -1)
    op=$("${PSQL[@]}" -c "SELECT md5(string_agg(cg||','||ag||','||ck||','||d, '|'
                 ORDER BY cg, ag, ck)) FROM (
              SELECT c.company_group cg, a.account_group ag, r.company_key ck,
                     sum(r.debit_cents)::bigint d
              FROM reg2_card r
              JOIN dim_company_c c ON c.company_key = r.company_key
              JOIN dim_account_c a ON a.account_key = r.account_key
              WHERE r.period BETWEEN 1 AND 12 GROUP BY 1,2,3) s" 2>/dev/null | tail -1)
    if ! grep -q '^ERROR' <<<"$out" && [ "${g2:-0}" -ge 1 ] && [ -n "$ob" ] && [ "$ob" = "$op" ]; then
        echo "  PASS  an oversized dimension grows ($g2 growth) and loses no rows"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  oversized dimension: growths=$g2 batch=${ob:0:12} sql=${op:0:12} $(grep -c '^ERROR' <<<"$out") errors"
        fail=1
    fi
    "${PSQL[@]}" -c "DELETE FROM dim_account_c WHERE account_key > 1000" >/dev/null
    card_k 31

    # A non-power-of-two capacity would silently address part of the table.
    out=$("${PSQL[@]}" -c "SELECT xpb_grp_test_policy(100, 0)" 2>&1)
    if grep -q "power of two" <<<"$out"; then
        echo "  PASS  a capacity that is not a power of two is refused"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  non-power-of-two capacity accepted: $(tr '\n' ' ' <<<"$out" | cut -c1-80)"
        fail=1
    fi

    # The production policy is what a connection that never calls the hook gets.
    line=$("${PSQL[@]}" -c "$GUCS SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1 \
           | sed -n 's/^NOTICE:  v2_register_report //p')
    ic=$(sed -n 's/.*grp_initial_cap=\([0-9]*\).*/\1/p' <<<"$line")
    if [ "$ic" = "16384" ]; then
        echo "  PASS  a connection that never sets the hook gets the production policy (16384)"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  production policy not in force by default: initial_cap=$ic"
        fail=1
    fi
else
    echo "  SKIP  xpb_grp_test_policy not installed"
fi

echo
echo "=== growing dimension hashes (Dimension Hash Growth v1) ==="

if "${PSQL[@]}" -c "SELECT 1 FROM pg_proc WHERE proname='xpb_v2_register_report'" \
        2>/dev/null | grep -q 1; then

    GUCS="SET jit=off; SET max_parallel_workers_per_gather=0;"

    # Dimension cardinality is what varies here, so the fact table is built once
    # to reference a wide key space and the dimensions are repopulated per case.
    # reg2_card is renamed aside while this runs, exactly as the overflow case
    # does, so the group-hash cases above keep their own dataset.
    "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
CREATE TABLE dimgrow_fact (period int4 NOT NULL, company_key int4 NOT NULL,
    account_key int8 NOT NULL, debit_cents int8 NOT NULL, credit_cents int8 NOT NULL,
    quantity int8, debit numeric(18,2), credit numeric(18,2) NOT NULL, comment text);
ALTER TABLE reg2_card RENAME TO reg2_card_dimkeep;
ALTER TABLE dimgrow_fact RENAME TO reg2_card;
SQL

    # dim_fill <n_companies> <n_accounts> <company_stride> <account_stride>
    # A stride of 1 gives sequential keys, which the hash maps collision-free.
    # A stride equal to a capacity gives guaranteed collisions: the index comes
    # from the low bits of key * odd, so keys differing by a multiple of the
    # capacity land on the same slot.
    dim_fill() {
        "${PSQL[@]}" >/dev/null <<SQL
TRUNCATE dim_company_c; TRUNCATE dim_account_c; TRUNCATE reg2_card;
INSERT INTO dim_company_c SELECT 1 + g * $3, (g % 8) + 1 FROM generate_series(0, $1 - 1) g;
INSERT INTO dim_account_c SELECT 1 + g * $4, (g % 4) + 1 FROM generate_series(0, $2 - 1) g;
INSERT INTO reg2_card
SELECT (g % 12) + 1,
       1 + (g % $1) * $3,
       1 + ((g / $1) % $2) * $4,
       g * 3, g * 2, NULL, NULL, 1.00, NULL
FROM generate_series(0, $(( $1 * $2 * 2 - 1 ))) g;
SQL
    }

    # dim_case <label> <ncomp> <nacc> <cstride> <astride> <want_d1_growths> <want_d2_growths>
    # Join results are compared per row against PostgreSQL: a rehash that lost
    # an entry would silently drop every fact row referencing that key, which is
    # a wrong join result and not an error.
    dim_case() {
        local label="$1" line g1 g2 b p
        dim_fill "$2" "$3" "$4" "$5"
        line=$("${PSQL[@]}" -c "$GUCS
                SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1 \
               | sed -n 's/^NOTICE:  v2_register_report //p')
        g1=$(sed -n 's/.*dim1_growths=\([0-9]*\).*/\1/p' <<<"$line")
        g2=$(sed -n 's/.*dim2_growths=\([0-9]*\).*/\1/p' <<<"$line")
        b=$("${PSQL[@]}" -c "$GUCS
             SELECT md5(string_agg(company_group||','||account_group||','||company_key
                        ||','||debit_turnover||','||credit_turnover||','||net_turnover,
                        '|' ORDER BY company_group, account_group, company_key))
             FROM xpb_v2_register_report(1,12,'card')" 2>/dev/null | tail -1)
        p=$("${PSQL[@]}" -c "SELECT md5(string_agg(cg||','||ag||','||ck||','||d||','||c||','||n,
                    '|' ORDER BY cg, ag, ck)) FROM (
                 SELECT c.company_group cg, a.account_group ag, r.company_key ck,
                        sum(r.debit_cents)::bigint d, sum(r.credit_cents)::bigint c,
                        sum(r.debit_cents - r.credit_cents)::bigint n
                 FROM reg2_card r
                 JOIN dim_company_c c ON c.company_key = r.company_key
                 JOIN dim_account_c a ON a.account_key = r.account_key
                 WHERE r.period BETWEEN 1 AND 12
                 GROUP BY 1,2,3) s" 2>/dev/null | tail -1)
        if [ "$g1" = "$6" ] && [ "$g2" = "$7" ] && [ -n "$b" ] && [ "$b" = "$p" ]; then
            echo "  PASS  $label: dim1 $g1 growth(s), dim2 $g2, join matches PostgreSQL"
            pass_count=$((pass_count + 1))
        else
            echo "  FAIL  $label: dim1=$g1 (want $6) dim2=$g2 (want $7) batch=${b:0:12} sql=${p:0:12}"
            fail=1
        fi
    }

    # dim1 capacity 256 (grow at 128), dim2 capacity 1024 (grow at 512).
    # Section 15: the threshold, exactly, for each table independently.
    dim_case "dim1 127 entries, below half of 256"   127 16  1 1  0 0
    dim_case "dim1 128 entries, exactly half"        128 16  1 1  0 0
    dim_case "dim1 129 entries, one past half"       129 16  1 1  1 0
    dim_case "dim2 511 entries, below half of 1024"   8 511 1 1  0 0
    dim_case "dim2 512 entries, exactly half"         8 512 1 1  0 0
    dim_case "dim2 513 entries, one past half"        8 513 1 1  0 1
    # Section 14: 0 / 1 / 2 / 3+ growths.
    dim_case "dim1 2 growths"                       300 16  1 1  2 0
    dim_case "dim1 3 growths, dim2 2"               600 1100 1 1  3 2
    # A key absent from the dimension must still drop its fact rows, and a
    # duplicate dimension key must still keep the first payload.
    "${PSQL[@]}" >/dev/null <<'SQL'
INSERT INTO reg2_card VALUES (1, 999999, 1, 7, 3, NULL, NULL, 1.00, NULL);
INSERT INTO dim_company_c VALUES (1, 4242);
SQL
    dim_missing=$("${PSQL[@]}" -c "$GUCS
         SELECT md5(string_agg(company_group||','||account_group||','||company_key
                    ||','||debit_turnover, '|'
                    ORDER BY company_group, account_group, company_key))
         FROM xpb_v2_register_report(1,12,'card')" 2>/dev/null | tail -1)
    sql_missing=$("${PSQL[@]}" -c "SELECT md5(string_agg(cg||','||ag||','||ck||','||d,
                '|' ORDER BY cg, ag, ck)) FROM (
             SELECT c.company_group cg, a.account_group ag, r.company_key ck,
                    sum(r.debit_cents)::bigint d
             FROM reg2_card r
             JOIN (SELECT DISTINCT ON (company_key) company_key, company_group
                   FROM dim_company_c ORDER BY company_key, ctid) c
                  ON c.company_key = r.company_key
             JOIN dim_account_c a ON a.account_key = r.account_key
             WHERE r.period BETWEEN 1 AND 12 GROUP BY 1,2,3) s" 2>/dev/null | tail -1)
    if [ -n "$dim_missing" ] && [ "$dim_missing" = "$sql_missing" ]; then
        echo "  PASS  absent keys drop their rows and a duplicate key keeps the first payload"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  missing/duplicate key semantics changed: ${dim_missing:0:12} vs ${sql_missing:0:12}"
        fail=1
    fi

    # Section 16: collisions, and the reset. A stride of 256 makes every
    # company key collide at capacity 256; after the table doubles to 512 the
    # same keys split across two slots, so the current mark must fall while the
    # lifetime mark keeps the pre-growth maximum. This case fails if the reset
    # in v2_dim1_grow is removed.
    dim_fill 200 8 256 1
    line=$("${PSQL[@]}" -c "$GUCS
            SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1 \
           | sed -n 's/^NOTICE:  v2_register_report //p')
    pc=$(sed -n 's/.*dim1_max_probe_current=\([0-9]*\).*/\1/p' <<<"$line")
    pl=$(sed -n 's/.*dim1_max_probe_lifetime=\([0-9]*\).*/\1/p' <<<"$line")
    gw=$(sed -n 's/.*dim1_growths=\([0-9]*\).*/\1/p' <<<"$line")
    if [ "$gw" -ge 1 ] && [ "$pl" -gt 1 ] && [ "$pc" -lt "$pl" ]; then
        echo "  PASS  colliding keys build chains ($pl) and the current mark resets on growth ($pc)"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  collision case: dim1 growths=$gw current=$pc lifetime=$pl (want lifetime>1, current<lifetime)"
        fail=1
    fi

    # Section 9: repeated growth must leave one live table. Checked against the
    # memory system's own accounting, at a capacity where each generation is
    # its own allocation block rather than a freelist chunk.
    dim_fill 8 1100 1 1
    line=$("${PSQL[@]}" -c "$GUCS
            SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1 \
           | sed -n 's/^NOTICE:  v2_register_report //p')
    cur=$(sed -n 's/.*dim2_bytes=\([0-9]*\).*/\1/p' <<<"$line")
    cxt=$(sed -n 's/.*dim2_cxt_bytes=\([0-9]*\).*/\1/p' <<<"$line")
    pk=$(sed -n 's/.*dim2_bytes_peak=\([0-9]*\).*/\1/p' <<<"$line")
    gw=$(sed -n 's/.*dim2_growths=\([0-9]*\).*/\1/p' <<<"$line")
    allgen=$(( cur + cur / 2 ))
    if [ "$gw" -ge 1 ] && [ "$cxt" -lt "$allgen" ]; then
        echo "  PASS  dim2 after $gw growths holds one live table ($cxt bytes, table $cur)"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  dim2 after $gw growths: context holds $cxt for a $cur byte table"
        fail=1
    fi
    if [ "$pk" = "$allgen" ]; then
        echo "  PASS  dim2 peak memory is old+new across the rehash ($pk)"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  dim2 peak $pk is not old+new ($allgen)"
        fail=1
    fi

    # Two reports in ONE backend: the second must start from the initial
    # capacity with no entries carried over.
    dim_fill 300 16 1 1
    line=$("${PSQL[@]}" -c "$GUCS
            SELECT count(*) FROM xpb_v2_register_report(1,12,'card');
            TRUNCATE dim_company_c;
            INSERT INTO dim_company_c SELECT g + 1, 1 FROM generate_series(0, 9) g;
            SELECT count(*) FROM xpb_v2_register_report(1,12,'card');" 2>&1 \
           | sed -n 's/^NOTICE:  v2_register_report //p' | tail -1)
    ic=$(sed -n 's/.*dim1_initial_cap=\([0-9]*\).*/\1/p' <<<"$line")
    en=$(sed -n 's/.*dim1_entries=\([0-9]*\).*/\1/p' <<<"$line")
    gw=$(sed -n 's/.*dim1_growths=\([0-9]*\).*/\1/p' <<<"$line")
    if [ "$ic" = "256" ] && [ "$en" = "10" ] && [ "$gw" = "0" ]; then
        echo "  PASS  a later report in the same backend starts fresh at 256 with 10 entries"
        pass_count=$((pass_count + 1))
    else
        echo "  FAIL  dimension rescan state leaked: initial=$ic entries=$en growths=$gw (want 256/10/0)"
        fail=1
    fi

    # Load must never exceed 0.5 on either table once it has grown.
    #
    # This needs its OWN report on a table that actually grew. Reusing $line
    # from the rescan case above measured 10 entries in 256 slots and 16 in
    # 1024 -- neither had grown, so no growth-policy defect could have made
    # either assertion fail. Each case below therefore asserts the growth
    # count as well, so it cannot quietly become vacuous again.
    dim_fill 700 2600 1 1
    line=$("${PSQL[@]}" -c "$GUCS
            SELECT count(*) FROM xpb_v2_register_report(1,12,'card');" 2>&1 \
           | sed -n 's/^NOTICE:  v2_register_report //p' | tail -1)
    for d in dim1 dim2; do
        lf=$(sed -n "s/.*${d}_load_factor=\([0-9.]*\).*/\1/p" <<<"$line")
        gw=$(sed -n "s/.*${d}_growths=\([0-9]*\).*/\1/p" <<<"$line")
        if [ -n "$lf" ] && [ -n "$gw" ] && [ "$gw" -ge 1 ] \
           && awk "BEGIN{exit !($lf <= 0.5)}"; then
            echo "  PASS  $d load factor stays at or below 0.5 ($lf) after $gw growth(s)"
            pass_count=$((pass_count + 1))
        else
            echo "  FAIL  $d load=$lf growths=$gw (want load <= 0.5 on a table that grew)"
            fail=1
        fi
    done

    "${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
ALTER TABLE reg2_card RENAME TO dimgrow_fact;
ALTER TABLE reg2_card_dimkeep RENAME TO reg2_card;
SQL
else
    echo "  SKIP  xpb_v2_register_report not installed"
fi

echo
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
