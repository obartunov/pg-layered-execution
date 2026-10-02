#!/bin/bash
#
# R1-14 and R1-15 - XpGroupAgg2 read heap attributes at the wrong offset, and
# tested a pushed predicate on a nullable column as if NULL were zero
#
#   groupagg2_attr_offset_and_null_pred.sh <PGPORT> [PGHOST] [DB]
#
# Both are reachable from ordinary SQL with xp_batch.enabled and
# xp_batch.groupagg2 on, and both were found by the source-contract audit
# rather than by the reachability map.
#
# R1-15, xpb_typeops.c:xpb_getattr_scalar. The attribute walk did
#
#     tp += att_align_nominal(attr->attlen, attr->attalign);
#
# aligning the attribute's LENGTH instead of the running offset, so padding
# before a fast-path column was never counted. For columns of equal length and
# alignment -- (int4, int4, int4), which is the benchmark schema -- the two
# agree, which is why nothing caught it. One leading bool is enough:
#
#     PostgreSQL   2400 groups, sum 11 519 175
#     XpGroupAgg2  2400 groups, sum 193 259 687 116 800
#
# R1-14, xpb_groupagg2.c:xpga2_add_path. The NULL scope guard covered the
# grouping and aggregate columns; predicates are extracted later and were never
# checked. getattr_fn returns 0 for a NULL, so every NULL row was tested as zero
# and passed a < 10:
#
#     PostgreSQL   1200 groups, sum   985 461
#     XpGroupAgg2  2400 groups, sum 2 631 012
#
# The two fixes are deliberately different, and the test asserts which is which:
# the offset defect is CORRECTED, so the node must still run; the NULL predicate
# is REFUSED before execution, so the node must decline and PostgreSQL must do
# the query. A test that only compared answers would pass if the node were
# switched off entirely.
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"

PSQL=("$PGBIN/psql" -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

XPON="LOAD 'xp_batch'; SET xp_batch.enabled=on; SET xp_batch.groupagg2=on;
      SET max_parallel_workers_per_gather=0; SET jit=off; SET client_min_messages=warning;"
OFF="SET max_parallel_workers_per_gather=0; SET jit=off;"

pass=0; fail=0
ok()  { echo "  ok      $1"; pass=$((pass+1)); }
bad() { echo "  FAIL    $1"; fail=$((fail+1)); }

PRE=$("${PSQL[@]}" -c "SELECT 'alive'" 2>&1 | tail -1)
[ "$PRE" = "alive" ] || { echo "!! cannot query $DB on port $PORT: $PRE"; exit 2; }
PRE=$("${PSQL[@]}" -c "$XPON SELECT 'loaded'" 2>&1 | tail -1)
[ "$PRE" = "loaded" ] || { echo "!! xp_batch will not load: $PRE"; exit 2; }

# Own fixtures: r14_*/r15_* rather than the benchmark's names.
"${PSQL[@]}" -v ON_ERROR_STOP=1 >/dev/null 2>&1 <<'SQL' || { echo "!! fixture failed"; exit 2; }
DROP TABLE IF EXISTS r15_pad, r15_nopad, r15_wide, r14_pred;

-- one byte of padding before every fast-path column
CREATE TABLE r15_pad (flag bool NOT NULL, k1 int NOT NULL, k2 int NOT NULL, v int NOT NULL);
INSERT INTO r15_pad SELECT true, i/2000+1, i%20, i%97 FROM generate_series(0,239999) i;
ANALYZE r15_pad;

-- the control: same columns, no padding. Must be unaffected either way.
CREATE TABLE r15_nopad (k1 int NOT NULL, k2 int NOT NULL, v int NOT NULL);
INSERT INTO r15_nopad SELECT i/2000+1, i%20, i%97 FROM generate_series(0,239999) i;
ANALYZE r15_nopad;

-- int4 followed by int8: the second site, where the aggregate is 8-byte
-- aligned and sat 4 bytes from where the walk put it.
CREATE TABLE r15_wide (k1 int NOT NULL, k2 int NOT NULL, v bigint NOT NULL);
INSERT INTO r15_wide SELECT i/2000+1, i%20, (i%97)::bigint FROM generate_series(0,239999) i;
ANALYZE r15_wide;

-- nullable predicate column, NOT NULL keys and aggregate
CREATE TABLE r14_pred (k1 int NOT NULL, k2 int NOT NULL, v int NOT NULL, a int);
INSERT INTO r14_pred SELECT i/2000+1, i%20, i%97,
       CASE WHEN i%7 = 0 THEN NULL ELSE i%100 END FROM generate_series(0,239999) i;
ANALYZE r14_pred;
SQL

# compare <label> <table> <where> <expect_node: yes|no>
compare() {
    local label=$1 tbl=$2 where=$3 want_node=$4
    local q="SELECT coalesce(count(*)||'|'||sum(s),'ZERO ROWS')
             FROM (SELECT k1, k2, sum(v) s FROM $tbl $where GROUP BY 1,2) q"
    local want got plan node
    want=$("${PSQL[@]}" -c "$OFF $q" 2>&1 | tail -1)
    got=$("${PSQL[@]}" -c "$XPON $q" 2>&1 | tail -1)
    plan=$("${PSQL[@]}" -c "$XPON EXPLAIN (COSTS OFF) SELECT k1,k2,sum(v) FROM $tbl $where GROUP BY 1,2" 2>&1)
    node=no; case "$plan" in *XpGroupAgg2*) node=yes ;; esac

    echo "=== $label (node=$node) ==="
    [ "$got" = "$want" ] && ok "$label agrees with PostgreSQL (= $got)" \
                         || bad "$label - PostgreSQL [$want], XpGroupAgg2 [$got]"
    if [ "$want_node" = yes ]; then
        [ "$node" = yes ] && ok "and the node ran, so the agreement is not from declining it" \
                          || bad "the node declined - this says nothing about its arithmetic"
    else
        [ "$node" = no ] && ok "and the node declined before execution, as the policy requires" \
                         || bad "the node ran on a nullable predicate column"
    fi
}

compare "R1-15 padding before the keys" r15_pad   ""              yes
compare "R1-15 control, no padding"      r15_nopad ""              yes
compare "R1-15 int8 aggregate after int4 keys" r15_wide ""         yes
compare "R1-14 nullable predicate column" r14_pred "WHERE a < 10"  no

# The refusal must be about nullability, not about the predicate as such: the
# same shape with a NOT NULL predicate column still has to reach the node, or
# R1-14's guard would have disabled pushdown wholesale.
"${PSQL[@]}" -v ON_ERROR_STOP=1 >/dev/null 2>&1 <<'SQL'
DROP TABLE IF EXISTS r14_notnull;
CREATE TABLE r14_notnull (k1 int NOT NULL, k2 int NOT NULL, v int NOT NULL, a int NOT NULL);
INSERT INTO r14_notnull SELECT i/2000+1, i%20, i%97, i%100 FROM generate_series(0,239999) i;
ANALYZE r14_notnull;
SQL
compare "R1-14 control, NOT NULL predicate column" r14_notnull "WHERE a < 10" yes

"${PSQL[@]}" -c "DROP TABLE IF EXISTS r15_pad, r15_nopad, r15_wide, r14_pred, r14_notnull" >/dev/null 2>&1

echo
echo "############ $pass correct, $fail wrong ############"
[ "$fail" -eq 0 ]
