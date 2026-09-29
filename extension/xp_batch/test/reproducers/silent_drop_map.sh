#!/bin/bash
#
# Silent-Drop Reachability — runtime reproducers for every site proven R1
#
#   extension/xp_batch/test/reproducers/silent_drop_map.sh <PGPORT> [PGHOST] [DB]
#
# THIS SCRIPT IS EXPECTED TO FAIL while the defects are open. It is deliberately
# NOT called by contract_tests.sh. Each case compares xp_batch against plain
# PostgreSQL on the same data and prints want/got. It inverts into a regression
# suite as each site is fixed.
#
# Every case creates its own objects in a scratch database and drops them again.
# Nothing here touches the benchmark datasets or any published artifact.
#
# The map these cases belong to is docs/roadmap/silent-drop-reachability.md.
set -uo pipefail

PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-sdaudit}"

PSQL=(psql -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

# The shared ZLFS directory accumulates zone files from dropped relations and
# warns about each one; that is environment noise, not part of any result.
strip() { grep -vE '^(WARNING|NOTICE|LOAD|SET|CREATE|DROP|INSERT|TRUNCATE|ANALYZE)'; }

pass=0; fail=0
check() {   # check <label> <want> <got>
    if [ "$2" = "$3" ]; then
        echo "  ok      $1 (= $3)"
        pass=$((pass + 1))
    else
        echo "  SILENT  $1 — want $2, got $3"
        fail=$((fail + 1))
    fi
}

echo "############ Silent-Drop Reachability — runtime reproducers ############"
echo
echo "Each case: identical data, xp_batch against plain PostgreSQL."
echo

# ---------------------------------------------------------------- R1-1, R1-3 --
# xpb_projection.c AGG_CAP 16384 and xpb_columnar_pipeline.c S2CAP 16384.
# Both read a relation named reg_buh BY NAME and group by (period, company).
# The column ORDER matters: these functions read fixed byte offsets into the
# tuple (amount_dt at d+20), so the layout below is the benchmark's, not a
# convenience. A different column order silently yields zeros, which is its own
# finding (see the map, "unvalidated fixed offsets").
"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
DROP TABLE IF EXISTS reg_buh;
CREATE TABLE reg_buh (period_key int NOT NULL, company_key int NOT NULL, account_key int NOT NULL,
                      debit_key int NOT NULL, credit_key int NOT NULL, amount_dt int NOT NULL,
                      amount_kt int NOT NULL, payload int NOT NULL);
INSERT INTO reg_buh SELECT p, c, 1, 0, 0, 1, 0, 0
  FROM generate_series(1,100) p, generate_series(1,100) c;    -- 10 000 pairs, under both caps
ANALYZE reg_buh;
SQL
echo "=== control: 10 000 distinct (period,company) pairs, under every cap ==="
truth=$("${PSQL[@]}" -c "SELECT sum(amount_dt) FROM reg_buh" 2>&1 | strip)
got=$("${PSQL[@]}" -c "LOAD 'xp_batch'; SELECT sum(total_amt) FROM projection_experiment(1,100) WHERE path='projection'" 2>&1 | strip)
check "projection_experiment below AGG_CAP" "$truth" "$got"
got=$("${PSQL[@]}" -c "LOAD 'xp_batch'; SELECT sum(total_amt) FROM ctr_pipeline(1,100) WHERE path='columnar_tr'" 2>&1 | strip)
check "ctr_pipeline below S2CAP" "$truth" "$got"

echo
echo "=== R1-1 / R1-3: 16 900 distinct pairs, past AGG_CAP and S2CAP (both 16384) ==="
"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
TRUNCATE reg_buh;
INSERT INTO reg_buh SELECT p, c, 1, 0, 0, 1, 0, 0
  FROM generate_series(1,130) p, generate_series(1,130) c;    -- 16 900 pairs
ANALYZE reg_buh;
SQL
truth=$("${PSQL[@]}" -c "SELECT sum(amount_dt) FROM reg_buh" 2>&1 | strip)
got=$("${PSQL[@]}" -c "LOAD 'xp_batch'; SELECT sum(total_amt) FROM projection_experiment(1,130) WHERE path='projection'" 2>&1 | strip)
check "R1-1 xpb_projection.c AGG_CAP" "$truth" "$got"
got=$("${PSQL[@]}" -c "LOAD 'xp_batch'; SELECT sum(total_amt) FROM ctr_pipeline(1,130) WHERE path='columnar_tr'" 2>&1 | strip)
check "R1-3 xpb_columnar_pipeline.c S2CAP" "$truth" "$got"

echo
echo "=== R1-2: 140 000 distinct triples, past WHASH_CAP (131072) ==="
echo "    pairs held at 10 000 so stage 2 is clean and only WHASH can drop"
"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
TRUNCATE reg_buh;
INSERT INTO reg_buh SELECT p, c, a, 0, 0, 1, 0, 0
  FROM generate_series(1,100) p, generate_series(1,100) c, generate_series(1,14) a;
ANALYZE reg_buh;
SQL
truth=$("${PSQL[@]}" -c "SELECT sum(amount_dt) FROM reg_buh" 2>&1 | strip)
got=$("${PSQL[@]}" -c "LOAD 'xp_batch'; SELECT sum(total_amt) FROM ctr_pipeline(1,100) WHERE path='columnar_tr'" 2>&1 | strip)
check "R1-2 xpb_columnar_pipeline.c WHASH_CAP" "$truth" "$got"

# ---------------------------------------------------------------------- R1-5 --
# xpb_batch_partition.c: pdim_year/adim_group return -1 for "no such key", and
# the caller drops on `yr < 0 || ag < 0`. A dimension payload that is itself
# negative is therefore read as a join miss. Nothing constrains the payload:
# dim_account.account_group is plain int NOT NULL with no CHECK.
echo
echo "=== R1-5: a negative dimension payload reads as a join miss ==="
"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
DROP TABLE IF EXISTS pt_parent, dim_period, dim_account CASCADE;
CREATE TABLE dim_period  (period_key int NOT NULL, year int NOT NULL, quarter int NOT NULL);
CREATE TABLE dim_account (account_key int NOT NULL, account_group int NOT NULL, account_type int NOT NULL);
INSERT INTO dim_period  VALUES (1, 2020, 1), (2, 2021, 1);
INSERT INTO dim_account VALUES (1, 5, 1), (2, -3, 1);        -- -3 is a legal account_group
CREATE TABLE pt_parent (period_key int NOT NULL, company_key int NOT NULL, account_key int NOT NULL,
                        debit_key int NOT NULL, credit_key int NOT NULL, amount_dt int NOT NULL,
                        amount_kt int NOT NULL, payload int NOT NULL) PARTITION BY RANGE (period_key);
CREATE TABLE pt_p1 PARTITION OF pt_parent FOR VALUES FROM (1) TO (3);
INSERT INTO pt_parent SELECT p, 1, a, 0, 0, 100, 0, 0
  FROM generate_series(1,2) p, generate_series(1,2) a;
ANALYZE pt_parent; ANALYZE dim_period; ANALYZE dim_account;
SQL
truth=$("${PSQL[@]}" -c "SELECT sum(f.amount_dt) FROM pt_parent f
                         JOIN dim_period d ON d.period_key = f.period_key
                         JOIN dim_account a ON a.account_key = f.account_key" 2>&1 | strip)
got=$("${PSQL[@]}" -c "LOAD 'xp_batch'; SELECT coalesce(sum(total_amt)::text,'0')
                       FROM xpb_partition_join2_groupby('pt_parent',1,2)" 2>&1 | strip)
check "R1-5 xpb_batch_partition.c -1 sentinel" "$truth" "$got"

# ------------------------------------------------------------- R1-6 to R1-8 --
# These are NOT capacity exhaustion. They were found by the capacity audit and
# they are the same harm: a valid query, no ERROR, a wrong answer. Kept here
# because a map of "what can silently change a SQL result" that omitted them
# would be worse than useless.
echo
echo "=== R1-6 / R1-7: xpb_pageagg.c — predicates that are never applied ==="
"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
DROP TABLE IF EXISTS pa_t;
CREATE TABLE pa_t (period_key int4 NOT NULL, amount_dt int4 NOT NULL, name text);
INSERT INTO pa_t SELECT g % 100, 1000 + g, CASE WHEN g % 10 = 0 THEN 'x' ELSE 'y' END
  FROM generate_series(1, 20000) g;
ANALYZE pa_t;
SQL
truth=$("${PSQL[@]}" -c "SELECT count(*) FROM pa_t WHERE name = 'x'" 2>&1 | strip)
got=$("${PSQL[@]}" -c "LOAD 'xp_batch'; SET xp_batch.enabled=on; SET xp_batch.pageagg=on;
                       SELECT count(*) FROM pa_t WHERE name = 'x'" 2>&1 | strip)
check "R1-6 residual qual dropped (WHERE name='x')" "$truth" "$got"

truth=$("${PSQL[@]}" -c "SELECT sum(amount_dt) FROM pa_t WHERE period_key < 10" 2>&1 | strip)
got=$("${PSQL[@]}" -c "LOAD 'xp_batch'; SET xp_batch.enabled=on; SET xp_batch.pageagg=on;
                       SET xp_batch.pageagg_summary=on; SET xp_batch.pageagg_skip=on;
                       SELECT sum(amount_dt) FROM pa_t WHERE period_key < 10" 2>&1 | strip)
check "R1-7 page skip uses the aggregated column's min/max" "$truth" "$got"

echo
echo "=== R1-8: xpb_groupagg2.c — residual qual dropped when one pred was pushed ==="
"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
DROP TABLE IF EXISTS ga_t;
CREATE TABLE ga_t (k1 int4 NOT NULL, k2 int4 NOT NULL, v int8 NOT NULL, tag text);
INSERT INTO ga_t SELECT g/10, 0, 1, CASE WHEN g%10=0 THEN 'keep' ELSE 'skip' END
  FROM generate_series(0,9999) g;
ANALYZE ga_t;
SQL
truth=$("${PSQL[@]}" -c "SELECT sum(v) FROM ga_t WHERE k1 <= 500 AND tag = 'keep'" 2>&1 | strip)
got=$("${PSQL[@]}" -c "LOAD 'xp_batch'; SET xp_batch.enabled=on; SET xp_batch.groupagg2=on;
                       SELECT sum(x) FROM (SELECT k1,k2,sum(v) x FROM ga_t
                       WHERE k1 <= 500 AND tag = 'keep' GROUP BY k1,k2) q" 2>&1 | strip)
check "R1-8 residual qual dropped (k1<=500 AND tag='keep')" "$truth" "$got"

"${PSQL[@]}" -c "DROP TABLE IF EXISTS reg_buh, pa_t, ga_t, pt_parent, dim_period, dim_account CASCADE" >/dev/null 2>&1

echo
echo "############ $pass agree with PostgreSQL, $fail silently disagree ############"
[ "$fail" -eq 0 ]
