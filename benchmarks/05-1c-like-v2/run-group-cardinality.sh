#!/bin/bash
#
# Benchmark 05-E — Group Cardinality and Memory Boundary
#
#   benchmarks/05-1c-like-v2/run-group-cardinality.sh <PGPORT> [PGHOST] [DBNAME]
#
# 05-A..05-D characterised the source. 05-E moves the bottleneck downstream on
# purpose and asks how the pipeline behaves as the number of live groups grows.
#
# Only group cardinality varies. The fact table is byte-identical at every
# point -- cardinality is dialled by rewriting 384 dimension rows, so the row
# count, the predicate, the selectivity, both joins and both dimension hash
# occupancies are held fixed by construction rather than by assertion.
#
# Primary source arm is ZLFS (section 2): the zone is already materialised, so
# source_ms is ~0 and what remains is operator behaviour. The heap fixed-offset
# arm is carried alongside as the same aggregate under a real source.
#
# When 05-E was measured the aggregation hash table was static: 16384 slots,
# 3/4 load limit, linear probing, no growth, so the ladder ended in a clean
# ERROR and that boundary was the result. Hash Aggregate Growth v1 removed it
# -- the table now starts at 16384 and doubles at half full -- so this runner
# no longer searches for a failure. The published 05-E numbers above were taken
# against the static table and are not reproducible on the current build.
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

# Per-group checksum, with NULLs rendered explicitly rather than swallowed by
# concatenation, ordered canonically before hashing (section 15).
CK="md5(coalesce(string_agg(
        coalesce(company_group::text,E'\\\\N')||','||coalesce(account_group::text,E'\\\\N')||','||
        coalesce(company_key::text,E'\\\\N')||','||coalesce(debit_turnover::text,E'\\\\N')||','||
        coalesce(credit_turnover::text,E'\\\\N')||','||coalesce(net_turnover::text,E'\\\\N'),
        '|' ORDER BY company_group, account_group, company_key), ''))"

# k -> groups is 128*k.  k values that divide 384 give exactly uniform groups;
# 97 does not and is used only to narrow the capacity boundary.
LADDER="2 8 16 32 48 64 80 88 92 96"
OVER="97 128 192 384"

echo "############ Benchmark 05-E — group cardinality and memory boundary ############"
echo
echo "=== the fact table, which must not change across the ladder ==="
"${PSQL[@]}" -c "SELECT 'reg2_card rows=' || count(*) || ' bytes=' || pg_relation_size('reg2_card')
                        || ' distinct company_key=' || count(DISTINCT company_key)
                        || ' distinct account_key=' || count(DISTINCT account_key) FROM reg2_card"

echo
echo "=== ZLFS zone (built once, outside every measured run — section 17) ==="
# Built here rather than assumed: zlfs_drop_zone(lo, hi) keys on the period
# range alone, not on the relation, so any earlier benchmark that drops zones
# for [1..12] -- 05-A does, and so does 04 -- takes this one with it. Ensuring
# the zone at the top of the run makes 05-E independent of gate ordering. The
# build is outside every measured run and its cost is reported here, not in
# query latency.
t_start=$(date +%s%N)
"${PSQL[@]}" -c "SELECT zlfs_build_zone('reg2_card','1,2,3,4,5',1,12)" 2>&1 \
    | grep -v '^WARNING' | sed 's/^/  /'
t_end=$(date +%s%N)
echo "zone (re)built outside measured runs: $(( (t_end - t_start) / 1000000 )) ms wall"
"${PSQL[@]}" -c "SELECT 'zone rows=' || nrows || ' size_kb=' || size_kb || ' freshness=' || freshness
                 FROM zlfs_zone_info() WHERE period_lo=1 AND period_hi=12
                   AND filepath LIKE '%' || (SELECT oid FROM pg_class WHERE relname='reg2_card') || '%'" \
    2>&1 | grep -v '^WARNING' || true

echo
echo "############ correctness gate ############"
echo "Per-group equality against PostgreSQL at every cardinality point, both"
echo "source arms. Grand totals alone are not accepted. Timing is not taken if"
echo "this fails."
echo
{
echo "$GUC"
echo "CREATE TEMP TABLE ck(k int, arm text, ck text, groups bigint, dsum numeric, csum numeric, nsum numeric, ord int);"
ord=0
for k in $LADDER; do
    echo "SELECT xpe_set_cardinality($k);"
    for arm in card-zlfs card; do
        ord=$((ord+1))
        echo "INSERT INTO ck SELECT $k, '$arm', $CK, count(*),
              sum(debit_turnover), sum(credit_turnover), sum(net_turnover), $ord
              FROM xpb_v2_register_report(1, 12, '$arm');"
    done
    ord=$((ord+1))
    echo "INSERT INTO ck SELECT $k, 'sql', $CK, count(*),
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
cat <<'SQL'
SELECT 'k=' || lpad(k::text,3) || '  groups=' || lpad(groups::text,6) || '  ' || rpad(arm,10) || ck
FROM ck ORDER BY ord;

DO $$
DECLARE r record; n int;
BEGIN
    FOR r IN SELECT k, count(DISTINCT ck) nck, count(DISTINCT groups) ng
             FROM ck GROUP BY k LOOP
        IF r.nck <> 1 THEN RAISE EXCEPTION 'PER-GROUP CHECKSUM MISMATCH at k=%', r.k; END IF;
        IF r.ng  <> 1 THEN RAISE EXCEPTION 'GROUP COUNT MISMATCH at k=%', r.k; END IF;
    END LOOP;
    SELECT count(*) INTO n FROM ck WHERE nsum <> dsum - csum;
    IF n <> 0 THEN RAISE EXCEPTION 'net identity fails in % rows', n; END IF;
    SELECT count(*) INTO n FROM ck WHERE groups <> 128 * k;
    IF n <> 0 THEN RAISE EXCEPTION 'group count is not 128*k in % rows', n; END IF;
    RAISE NOTICE '05-E gate PASS: both arms and PostgreSQL agree per group at every cardinality point';
END $$;
SQL
} | "${PSQL[@]}"

echo
echo "############ cardinality verification, per point ############"
for k in $LADDER; do
    g=$("${PSQL[@]}" -c "SELECT xpe_set_cardinality($k)")
    echo "-- k=$k (groups=$g) --"
    "${PSQL[@]}" -f "$HERE/verify-cardinality.sql" | sed 's/^/   /'
done

echo
echo "############ timings: 1 warm-up + $RUNS measured runs ############"
echo "# arm,k,run,groups,total_ms,open_ms,source_ms,join1_ms,join2_ms,agg_ms,operators_ms,rows,cap,load_limit,occupied,load_factor,grp_bytes,entry_bytes,inserts,hits,probes,probes_per_lookup,max_probe_lifetime,growths,rehashes,status"

for arm in card-zlfs card; do
    for k in $LADDER $OVER; do
        "${PSQL[@]}" -c "SELECT xpe_set_cardinality($k)" >/dev/null
        for i in $(seq 0 $RUNS); do
            # A capacity failure is an expected outcome here, so it must not
            # stop the script -- but it must be recorded as ERROR, not skipped.
            line=$("${PSQL[@]}" -c "$GUC
                     SELECT count(*) FROM xpb_v2_register_report(1,12,'$arm')" 2>&1 \
                   | grep -v '^WARNING' || true)
            if grep -q '^ERROR' <<<"$line"; then
                [ "$i" -eq 0 ] && continue
                echo "$arm,$k,$i,$((128*k)),,,,,,,,,,,,,,,,,,,,,,ERROR"
                continue
            fi
            line=$(sed -n 's/^NOTICE:  v2_register_report //p' <<<"$line")
            [ "$i" -eq 0 ] && continue
            g()  { sed -n "s/.*[ =]$1=\([0-9.]*\).*/\1/p" <<<"$line"; }
            echo "$arm,$k,$i,$(g groups),$(g total),$(g open),$(g source),$(g join1),$(g join2),$(g agg),$(g operators),$(g rows),$(g grp_cap),$(g grp_grow_at),$(g grp_occupied),$(g grp_load_factor),$(g grp_bytes),$(g grp_entry_bytes),$(g grp_inserts),$(g grp_hits),$(g grp_probes),$(g grp_probes_per_lookup),$(g grp_max_probe_lifetime),$(g grp_growths),$(g grp_rehashes),OK"
        done
    done
done

echo
echo "############ the former failure boundary ############"
echo "k=96 / k=97 was 05-E's boundary: 12 288 groups was the last cardinality a"
echo "fixed 16 384-slot table with a 3/4 load limit would accept, and 12 416"
echo "raised an error. Hash Aggregate Growth v1 removed that boundary, so this"
echo "section no longer searches for a failure -- it asserts that the two points"
echo "05-E separated are now both served, and that the second one grew."
for k in 96 97; do
    "${PSQL[@]}" -c "SELECT xpe_set_cardinality($k)" >/dev/null
    echo "-- k=$k, $((128 * k)) groups:"
    line=$({ "${PSQL[@]}" -c "$GUC
               SELECT count(*) FROM xpb_v2_register_report(1,12,'card-zlfs')" 2>&1 || true; } \
           | grep -v '^WARNING')
    if grep -q '^ERROR' <<<"$line"; then
        echo "   FAIL  a cardinality the growing table must serve was refused:"
        grep -E '^(ERROR|DETAIL|HINT)' <<<"$line" | sed 's/^/     /'
        exit 1
    fi
    sed -n 's/^NOTICE:  v2_register_report /   OK  /p' <<<"$line" | cut -c1-120
    cap=$(sed -n 's/.*[ =]grp_cap=\([0-9]*\).*/\1/p' <<<"$line")
    gro=$(sed -n 's/.*[ =]grp_growths=\([0-9]*\).*/\1/p' <<<"$line")
    echo "         grp_cap=$cap grp_growths=$gro"
    # 12 416 groups cannot fit 16 384 slots under a 0.5 policy, so the table
    # must have grown at least once to have served it at all.
    if [ "$k" = 97 ] && { [ -z "$gro" ] || [ "$gro" -lt 1 ]; }; then
        echo "   FAIL  k=97 was served without a growth, which the 0.5 policy forbids"
        exit 1
    fi
done

echo
echo "############ PostgreSQL baseline, single core (section 19) ############"
echo "# engine,k,run,groups,ms"
for k in 2 64 96; do
    "${PSQL[@]}" -c "SELECT xpe_set_cardinality($k)" >/dev/null
    for i in $(seq 0 $RUNS); do
        ms=$("${PSQL[@]}" -c "$GUC
             EXPLAIN (ANALYZE, TIMING OFF, FORMAT JSON)
             SELECT c.company_group, a.account_group, r.company_key,
                    sum(r.debit_cents), sum(r.credit_cents),
                    sum(r.debit_cents - r.credit_cents)
             FROM reg2_card r
             JOIN dim_company_c c ON c.company_key = r.company_key
             JOIN dim_account_c a ON a.account_key = r.account_key
             WHERE r.period BETWEEN 1 AND 12
             GROUP BY 1,2,3" | sed -n 's/.*"Execution Time": \([0-9.]*\).*/\1/p')
        [ "$i" -eq 0 ] && continue
        echo "postgres,$k,$i,$((128*k)),$ms"
    done
done

echo
echo "############ PostgreSQL plan shapes (section 20) ############"
mkdir -p "$HERE/plans/group-cardinality"
for k in 2 64 96; do
    "${PSQL[@]}" -c "SELECT xpe_set_cardinality($k)" >/dev/null
    f="$HERE/plans/group-cardinality/postgres-k${k}.txt"
    "${PSQL[@]}" > "$f" 2>&1 <<SQL
$GUC
EXPLAIN (ANALYZE, BUFFERS, SETTINGS, COSTS OFF)
SELECT c.company_group, a.account_group, r.company_key,
       sum(r.debit_cents), sum(r.credit_cents), sum(r.debit_cents - r.credit_cents)
FROM reg2_card r
JOIN dim_company_c c ON c.company_key = r.company_key
JOIN dim_account_c a ON a.account_key = r.account_key
WHERE r.period BETWEEN 1 AND 12
GROUP BY 1,2,3;
SQL
    echo "  k=$k groups=$((128*k)): $(grep -oE '(Hash|Group)Aggregate|Batches: [0-9]+|Planned Partitions: [0-9]+|Peak Memory Usage: [0-9]+ kB' "$f" | tr '\n' ' ')"
done

echo
echo "############ done ############"
