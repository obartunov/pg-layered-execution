#!/bin/bash
#
# Hash Aggregate Growth v1
#
#   benchmarks/05-1c-like-v2/run-hash-growth.sh <PGPORT> [PGHOST] [DBNAME]
#
# 05-E showed the aggregation ceiling was a compile-time constant: a static
# 16384-slot table with a 3/4 load limit, erroring at 12 289 groups, with the
# cost curve already breaking at half full. This measures the replacement.
#
# PREREGISTERED policy, fixed before any number was looked at and not tuned
# afterwards: grow when the table is half full, double the capacity. Hash
# function, key equality, linear probing and all layouts are unchanged, so the
# experiment isolates growth alone.
#
# Two fact tables, on purpose:
#
#   reg2_card    128 x 384 pairs, UNCHANGED from 05-E. The low ladder runs
#                here so the comparison against 05-E's published aggregate
#                timings at 6 144 / 8 192 / 12 288 groups is like for like.
#   reg2_card2   192 x 768 pairs, the most this report's dimension hashes can
#                hold. Carries the high ladder, which reg2_card cannot express.
#
# Primary source is ZLFS in both cases: the zone is materialised, so source_ms
# is ~0 and what is left is operator behaviour. The heap fixed-offset arm is
# carried alongside as a check that the aggregate behaves the same under a
# real source.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-onec2}"
RUNS=5

PSQL=(psql -p "$PORT" -d "$DB" -v ON_ERROR_STOP=1 -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

GUC="SET work_mem = '64MB'; SET enable_hashjoin = on; SET jit = off;
     SET max_parallel_workers_per_gather = 0;"

CK="md5(coalesce(string_agg(
        coalesce(company_group::text,E'\\\\N')||','||coalesce(account_group::text,E'\\\\N')||','||
        coalesce(company_key::text,E'\\\\N')||','||coalesce(debit_turnover::text,E'\\\\N')||','||
        coalesce(credit_turnover::text,E'\\\\N')||','||coalesce(net_turnover::text,E'\\\\N'),
        '|' ORDER BY company_group, account_group, company_key), ''))"

# groups = 128*k on reg2_card, 192*k on reg2_card2.
LOW="2 16 48 64 80 96 192 384"
HIGH="128 256 512 768"

echo "############ Hash Aggregate Growth v1 ############"
echo
echo "=== policy in force (must be the production one, never the test hook) ==="
"${PSQL[@]}" -c "$GUC SELECT count(*) FROM xpb_v2_register_report(1,12,'card')" 2>&1 \
    | grep -v '^WARNING' \
    | sed -n 's/.*\(grp_initial_cap=[0-9]*\).*/  \1, grow at half, double. Test hook must be unset here./p'

echo
echo "=== fact tables, which must not change across their ladders ==="
"${PSQL[@]}" -c "SELECT '  reg2_card  rows=' || count(*) || ' bytes=' || pg_relation_size('reg2_card')
                        || ' co=' || count(DISTINCT company_key) || ' ac=' || count(DISTINCT account_key)
                 FROM reg2_card"
"${PSQL[@]}" -c "SELECT '  reg2_card2 rows=' || count(*) || ' bytes=' || pg_relation_size('reg2_card2')
                        || ' co=' || count(DISTINCT company_key) || ' ac=' || count(DISTINCT account_key)
                 FROM reg2_card2"

echo
echo "=== ZLFS zones, built here and timed outside every measured run ==="
# zlfs_drop_zone(lo, hi) keys on the period range and not on the relation, so
# any earlier benchmark that drops zones for [1..12] takes these with it.
# Rebuilt here, which makes this runner independent of gate ordering.
for t in reg2_card reg2_card2; do
    t0=$(date +%s%N)
    "${PSQL[@]}" -c "SELECT zlfs_build_zone('$t','1,2,3,4,5',1,12)" 2>&1 \
        | grep -v '^WARNING' | sed -n 's/^NOTICE:  /  /p'
    t1=$(date +%s%N)
    echo "  $t zone built in $(( (t1 - t0) / 1000000 )) ms, outside measured runs"
done

echo
echo "############ correctness gate ############"
echo "Per-group equality against PostgreSQL at every cardinality point, both"
echo "source arms, on both fact tables. A rehash that lost or duplicated a"
echo "group would keep the row count right while splitting one group's sums,"
echo "so grand totals are not accepted. Timing is not taken if this fails."
echo
{
echo "$GUC"
echo "CREATE TEMP TABLE ck(tbl text, k int, arm text, ck text, groups bigint,
                           dsum numeric, csum numeric, nsum numeric, ord int);"
ord=0
for k in $LOW; do
    echo "SELECT xpe_set_cardinality($k);"
    for arm in card-zlfs card; do
        ord=$((ord+1))
        echo "INSERT INTO ck SELECT 'card', $k, '$arm', $CK, count(*),
              sum(debit_turnover), sum(credit_turnover), sum(net_turnover), $ord
              FROM xpb_v2_register_report(1, 12, '$arm');"
    done
    ord=$((ord+1))
    echo "INSERT INTO ck SELECT 'card', $k, 'sql', $CK, count(*),
          sum(debit_turnover), sum(credit_turnover), sum(net_turnover), $ord
          FROM (SELECT c.company_group, a.account_group, r.company_key,
                       sum(r.debit_cents)::bigint  AS debit_turnover,
                       sum(r.credit_cents)::bigint AS credit_turnover,
                       sum(r.debit_cents - r.credit_cents)::bigint AS net_turnover
                FROM reg2_card r
                JOIN dim_company_c c ON c.company_key = r.company_key
                JOIN dim_account_c a ON a.account_key = r.account_key
                WHERE r.period BETWEEN 1 AND 12
                GROUP BY 1,2,3) s;"
done
for k in $HIGH; do
    echo "SELECT xpe_set_cardinality_wide($k);"
    for arm in card2-zlfs card2; do
        ord=$((ord+1))
        echo "INSERT INTO ck SELECT 'card2', $k, '$arm', $CK, count(*),
              sum(debit_turnover), sum(credit_turnover), sum(net_turnover), $ord
              FROM xpb_v2_register_report(1, 12, '$arm');"
    done
    ord=$((ord+1))
    echo "INSERT INTO ck SELECT 'card2', $k, 'sql', $CK, count(*),
          sum(debit_turnover), sum(credit_turnover), sum(net_turnover), $ord
          FROM (SELECT c.company_group, a.account_group, r.company_key,
                       sum(r.debit_cents)::bigint  AS debit_turnover,
                       sum(r.credit_cents)::bigint AS credit_turnover,
                       sum(r.debit_cents - r.credit_cents)::bigint AS net_turnover
                FROM reg2_card2 r
                JOIN dim_company_c2 c ON c.company_key = r.company_key
                JOIN dim_account_c2 a ON a.account_key = r.account_key
                WHERE r.period BETWEEN 1 AND 12
                GROUP BY 1,2,3) s;"
done
cat <<'SQL'
SELECT rpad(tbl,6) || ' k=' || lpad(k::text,3) || '  groups=' || lpad(groups::text,7)
       || '  ' || rpad(arm,11) || ck
FROM ck ORDER BY ord;

DO $$
DECLARE r record; n int;
BEGIN
    FOR r IN SELECT tbl, k, count(DISTINCT ck) nck, count(DISTINCT groups) ng
             FROM ck GROUP BY tbl, k LOOP
        IF r.nck <> 1 THEN RAISE EXCEPTION 'PER-GROUP CHECKSUM MISMATCH at %/k=%', r.tbl, r.k; END IF;
        IF r.ng  <> 1 THEN RAISE EXCEPTION 'GROUP COUNT MISMATCH at %/k=%', r.tbl, r.k; END IF;
    END LOOP;
    SELECT count(*) INTO n FROM ck WHERE nsum <> dsum - csum;
    IF n <> 0 THEN RAISE EXCEPTION 'net identity fails in % rows', n; END IF;
    SELECT count(*) INTO n FROM ck WHERE groups <> CASE tbl WHEN 'card' THEN 128 ELSE 192 END * k;
    IF n <> 0 THEN RAISE EXCEPTION 'group count is not the expected multiple of k in % rows', n; END IF;
    RAISE NOTICE 'growth v1 gate PASS: both arms and PostgreSQL agree per group at every cardinality point';
END $$;
SQL
} | "${PSQL[@]}"

echo
echo "############ cardinality verification, per point (section 16) ############"
for k in $LOW; do
    g=$("${PSQL[@]}" -c "SELECT xpe_set_cardinality($k)")
    echo "-- card  k=$k groups=$g --"
    "${PSQL[@]}" -f "$HERE/verify-cardinality.sql" | sed 's/^/   /'
done
for k in $HIGH; do
    g=$("${PSQL[@]}" -c "SELECT xpe_set_cardinality_wide($k)")
    echo "-- card2 k=$k groups=$g --"
    "${PSQL[@]}" <<SQL | sed 's/^/   /'
WITH g AS (
    SELECT c.company_group, a.account_group, r.company_key, count(*) AS n
    FROM reg2_card2 r
    JOIN dim_company_c2 c ON c.company_key = r.company_key
    JOIN dim_account_c2 a ON a.account_key = r.account_key
    WHERE r.period BETWEEN 1 AND 12
    GROUP BY 1,2,3)
SELECT 'input rows        ' || (SELECT count(*) FROM reg2_card2 WHERE period BETWEEN 1 AND 12)
UNION ALL SELECT 'groups            ' || count(*) FROM g
UNION ALL SELECT 'rows per group    avg=' || round(avg(n),2) || ' min=' || min(n) || ' max=' || max(n) FROM g
UNION ALL SELECT 'uniform           ' || CASE WHEN min(n)=max(n) THEN 'yes' ELSE 'no' END FROM g;
SQL
done

echo
echo "############ timings: 1 warm-up + $RUNS measured runs ############"
echo "# tbl,arm,k,run,groups,total_ms,open_ms,source_ms,join1_ms,join2_ms,agg_ms,rehash_ms,agg_minus_rehash_ms,operators_ms,rows,initial_cap,capacity,grow_at,occupied,load_factor,bytes,bytes_peak,cxt_bytes,inserts,hits,probes,probes_per_lookup,max_probe,growths,rehash_groups,rehash_probes,status"

measure() {   # measure <tbl> <arm> <k> <setter>
    local tbl="$1" arm="$2" k="$3" setter="$4" i line
    "${PSQL[@]}" -c "SELECT $setter($k)" >/dev/null
    for i in $(seq 0 $RUNS); do
        line=$("${PSQL[@]}" -c "$GUC
                 SELECT count(*) FROM xpb_v2_register_report(1,12,'$arm')" 2>&1 \
               | grep -v '^WARNING' || true)
        if grep -q '^ERROR' <<<"$line"; then
            [ "$i" -eq 0 ] && continue
            echo "$tbl,$arm,$k,$i,,,,,,,,,,,,,,,,,,,,,,,,,,,,ERROR"
            continue
        fi
        line=$(sed -n 's/^NOTICE:  v2_register_report //p' <<<"$line")
        [ "$i" -eq 0 ] && continue
        g() { sed -n "s/.*[ =]$1=\([0-9.]*\).*/\1/p" <<<"$line"; }
        echo "$tbl,$arm,$k,$i,$(g groups),$(g total),$(g open),$(g source),$(g join1),$(g join2),$(g agg),$(g grp_rehash_ms),$(g grp_agg_minus_rehash_ms),$(g operators),$(g rows),$(g grp_initial_cap),$(g grp_cap),$(g grp_grow_at),$(g grp_occupied),$(g grp_load_factor),$(g grp_bytes),$(g grp_bytes_peak),$(g grp_cxt_bytes),$(g grp_inserts),$(g grp_hits),$(g grp_probes),$(g grp_probes_per_lookup),$(g grp_max_probe),$(g grp_growths),$(g grp_rehash_groups),$(g grp_rehash_probes),OK"
    done
}

for arm in card-zlfs card; do
    for k in $LOW; do measure card "$arm" "$k" xpe_set_cardinality; done
done
for arm in card2-zlfs card2; do
    for k in $HIGH; do measure card2 "$arm" "$k" xpe_set_cardinality_wide; done
done

echo
echo "############ PostgreSQL context (sections 24, 20) ############"
echo "# engine,tbl,groups,run,ms"
for k in 64 384; do
    "${PSQL[@]}" -c "SELECT xpe_set_cardinality($k)" >/dev/null
    for i in $(seq 0 $RUNS); do
        ms=$("${PSQL[@]}" -c "$GUC
             EXPLAIN (ANALYZE, TIMING OFF, FORMAT JSON)
             SELECT c.company_group, a.account_group, r.company_key,
                    sum(r.debit_cents), sum(r.credit_cents), sum(r.debit_cents - r.credit_cents)
             FROM reg2_card r
             JOIN dim_company_c c ON c.company_key = r.company_key
             JOIN dim_account_c a ON a.account_key = r.account_key
             WHERE r.period BETWEEN 1 AND 12
             GROUP BY 1,2,3" | sed -n 's/.*"Execution Time": \([0-9.]*\).*/\1/p')
        [ "$i" -eq 0 ] && continue
        echo "postgres,card,$((128*k)),$i,$ms"
    done
done
for k in 512; do
    "${PSQL[@]}" -c "SELECT xpe_set_cardinality_wide($k)" >/dev/null
    for i in $(seq 0 $RUNS); do
        ms=$("${PSQL[@]}" -c "$GUC
             EXPLAIN (ANALYZE, TIMING OFF, FORMAT JSON)
             SELECT c.company_group, a.account_group, r.company_key,
                    sum(r.debit_cents), sum(r.credit_cents), sum(r.debit_cents - r.credit_cents)
             FROM reg2_card2 r
             JOIN dim_company_c2 c ON c.company_key = r.company_key
             JOIN dim_account_c2 a ON a.account_key = r.account_key
             WHERE r.period BETWEEN 1 AND 12
             GROUP BY 1,2,3" | sed -n 's/.*"Execution Time": \([0-9.]*\).*/\1/p')
        [ "$i" -eq 0 ] && continue
        echo "postgres,card2,$((192*k)),$i,$ms"
    done
done

echo
echo "############ PostgreSQL plan shapes ############"
mkdir -p "$HERE/plans/hash-growth"
plan() {   # plan <tbl> <dim1> <dim2> <setter> <k>
    "${PSQL[@]}" -c "SELECT $4($5)" >/dev/null
    local f="$HERE/plans/hash-growth/postgres-$1-k$5.txt"
    "${PSQL[@]}" > "$f" 2>&1 <<SQL
$GUC
EXPLAIN (ANALYZE, BUFFERS, SETTINGS, COSTS OFF)
SELECT c.company_group, a.account_group, r.company_key,
       sum(r.debit_cents), sum(r.credit_cents), sum(r.debit_cents - r.credit_cents)
FROM $1 r JOIN $2 c ON c.company_key = r.company_key
          JOIN $3 a ON a.account_key = r.account_key
WHERE r.period BETWEEN 1 AND 12
GROUP BY 1,2,3;
SQL
    echo "  $1 k=$5: $(grep -oE '(Hash|Group)Aggregate|Batches: [0-9]+|Planned Partitions: [0-9]+|Memory Usage: [0-9]+kB|Disk Usage: [0-9]+kB' "$f" | tr '\n' ' ')"
}
plan reg2_card  dim_company_c  dim_account_c  xpe_set_cardinality      64
plan reg2_card  dim_company_c  dim_account_c  xpe_set_cardinality      384
plan reg2_card2 dim_company_c2 dim_account_c2 xpe_set_cardinality_wide 512

echo
echo "############ done ############"
