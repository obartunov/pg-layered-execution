#!/bin/bash
#
# Benchmark 05-B — Heap Deform Cost
#
#   benchmarks/05-1c-like-v2/run-heap-deform.sh <PGPORT> [PGHOST] [DBNAME]
#
# 05-A compared a wide generic-deform heap source against pgColumnar and found
# the heap source slower, where benchmark 04's narrow fixed-layout heap source
# had been faster. That comparison is confounded -- width, types, NULLability,
# attribute order, source path and generator all differ.
#
# This isolates one variable at a time, on ONE dataset:
#
#   A  reg2_bad     + deform   varlena ahead of the projection; deform forced
#                              (and required -- fixed cannot address it)
#   B  reg2_fixed   + deform   identical rows, projection is a fixed prefix,
#                              deform forced anyway
#   C  reg2_fixed   + fixed    the same table through fixed-offset extraction
#
#   B - C   deformation versus fixed-offset extraction, SAME physical table
#   A - B   the layout effect beyond the mechanism (carries a small size
#           difference from alignment padding -- see verify-layouts.sql)
#
# Nothing in the operator pipeline changes: same joins, same grouping, same
# three aggregates, same batch size, same predicate handling.
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

CK="md5(coalesce(string_agg(company_group||','||account_group||','||company_key||','||
                            debit_turnover||','||credit_turnover||','||net_turnover,
                            '|' ORDER BY company_group, account_group, company_key), ''))"

echo "############ Benchmark 05-B — heap deform cost ############"
echo
echo "=== layout equivalence and physical sizes ==="
"${PSQL[@]}" -f "$HERE/verify-layouts.sql"

echo
echo "############ source-mode proof ############"
echo "The mode is proved from the OUTSIDE, by a counter the fixed path cannot"
echo "increment, not by trusting the flag that was passed in."
echo
"${PSQL[@]}" <<SQL 2>&1 | sed -n 's/^NOTICE:  v2_register_report //p'
$GUC
SELECT count(*) FROM xpb_v2_register_report(1, 12, 'fixedlayout-deform');
SELECT count(*) FROM xpb_v2_register_report(1, 12, 'fixedlayout-fixed');
SQL

echo
echo "-- fixed mode on a layout that cannot support it must ERROR, not downgrade:"
# The error is the expected outcome here, so it must not stop the script.
{ "${PSQL[@]}" -c "SELECT count(*) FROM xpb_v2_register_report(1,12,'bad-fixed')" 2>&1 || true; } \
    | grep -E "^(ERROR|HINT)" | cut -c1-110

echo
echo "############ correctness gate ############"
echo "All three arms, every predicate. Timing is not taken if this fails."
echo
{
echo "$GUC"
cat <<'SQL'
CREATE TEMP TABLE ck(range text, arm text, ck text, groups bigint,
                     dsum bigint, csum bigint, nsum bigint, ord int);
SQL
ord=0
for range in "90 91" "1 1" "1 12"; do
    set -- $range; lo=$1; hi=$2; tag="$lo..$hi"
    for arm in bad-deform fixedlayout-deform fixedlayout-fixed; do
        ord=$((ord+1))
        echo "INSERT INTO ck SELECT '$tag', '$arm', $CK, count(*),
              sum(debit_turnover), sum(credit_turnover), sum(net_turnover), $ord
              FROM xpb_v2_register_report($lo, $hi, '$arm');"
    done
    # the SQL form over both physical tables, as an external reference
    for tbl in reg2_bad reg2_fixed; do
        ord=$((ord+1))
        echo "INSERT INTO ck SELECT '$tag', 'sql_$tbl', $CK, count(*),
              sum(debit_turnover), sum(credit_turnover), sum(net_turnover), $ord
              FROM (SELECT c.company_group, a.account_group, r.company_key,
                           sum(r.debit_cents)::bigint  AS debit_turnover,
                           sum(r.credit_cents)::bigint AS credit_turnover,
                           sum(r.debit_cents - r.credit_cents)::bigint AS net_turnover
                    FROM $tbl r
                    JOIN dim_company  c ON c.company_key = r.company_key
                    JOIN dim_account2 a ON a.account_key = r.account_key
                    WHERE r.period BETWEEN $lo AND $hi
                    GROUP BY 1,2,3) s;"
    done
done
cat <<'SQL'
SELECT rpad(range, 7) || rpad(arm, 22) || ck || '  groups=' || groups
FROM ck ORDER BY ord;

DO $$
DECLARE r record; n int;
BEGIN
    FOR r IN SELECT range, count(DISTINCT ck) nck, count(DISTINCT groups) ng
             FROM ck GROUP BY range LOOP
        IF r.nck <> 1 THEN RAISE EXCEPTION 'CHECKSUM MISMATCH for range %', r.range; END IF;
        IF r.ng  <> 1 THEN RAISE EXCEPTION 'GROUP COUNT MISMATCH for range %', r.range; END IF;
    END LOOP;
    SELECT count(*) INTO n FROM ck WHERE nsum <> dsum - csum;
    IF n <> 0 THEN RAISE EXCEPTION 'net identity fails in % rows', n; END IF;
    RAISE NOTICE '05-B gate PASS: three arms and both SQL layouts agree on every range';
END $$;
SQL
} | "${PSQL[@]}"

echo
echo "############ buffer residency ############"
echo "Both tables must be operating under comparable cache conditions, or a"
echo "difference in reads would be read as deform cost."
mkdir -p "$HERE/plans/heap-deform"
for tbl in reg2_bad reg2_fixed; do
    f="$HERE/plans/heap-deform/${tbl}-1_12-seqscan.txt"
    "${PSQL[@]}" > "$f" 2>&1 <<SQL
$GUC
EXPLAIN (ANALYZE, BUFFERS, SETTINGS, COSTS OFF)
SELECT count(*), sum(debit_cents) FROM $tbl WHERE period BETWEEN 1 AND 12;
SQL
    echo "  $tbl: $(grep -oE 'shared hit=[0-9]+( read=[0-9]+)?' "$f" | head -1)"
done

echo
echo "############ timings: 1 warm-up + $RUNS measured runs ############"
echo "# arm,lo,hi,run,total_ms,open_ms,source_ms,operators_ms,rows,heap_path,tuples_deformed,attrs_deformed,attrs_requested"

for range in "90 91" "1 1" "1 12"; do
    set -- $range; lo=$1; hi=$2
    for arm in bad-deform fixedlayout-deform fixedlayout-fixed; do
        for i in $(seq 0 $RUNS); do
            line=$("${PSQL[@]}" -c "$GUC
                     SELECT count(*) FROM xpb_v2_register_report($lo,$hi,'$arm')" 2>&1 \
                   | sed -n 's/^NOTICE:  v2_register_report //p')
            [ "$i" -eq 0 ] && continue
            t=$(sed -n 's/.*total=\([0-9.]*\) ms.*/\1/p' <<<"$line")
            o=$(sed -n 's/.*open=\([0-9.]*\) ms.*/\1/p' <<<"$line")
            s=$(sed -n 's/.*source=\([0-9.]*\) ms.*/\1/p' <<<"$line")
            op=$(sed -n 's/.*operators=\([0-9.]*\) ms.*/\1/p' <<<"$line")
            rw=$(sed -n 's/.*rows=\([0-9]*\).*/\1/p' <<<"$line")
            hp=$(sed -n 's/.*heap_path=\([a-z]*\).*/\1/p' <<<"$line")
            td=$(sed -n 's/.*tuples_deformed=\([0-9]*\).*/\1/p' <<<"$line")
            ad=$(sed -n 's/.*attrs_deformed=\([0-9]*\).*/\1/p' <<<"$line")
            ar=$(sed -n 's/.*attrs_requested=\([0-9]*\).*/\1/p' <<<"$line")
            echo "$arm,$lo,$hi,$i,$t,$o,$s,$op,$rw,$hp,$td,$ad,$ar"
        done
    done
done

echo
echo "# pgColumnar context, same two predicates, unchanged code"
for range in "1 1" "1 12"; do
    set -- $range; lo=$1; hi=$2
    for i in $(seq 0 $RUNS); do
        line=$("${PSQL[@]}" -c "$GUC
                 SELECT count(*) FROM xpb_v2_register_report($lo,$hi,'pgcolumnar')" 2>&1 \
               | sed -n 's/^NOTICE:  v2_register_report //p')
        [ "$i" -eq 0 ] && continue
        t=$(sed -n 's/.*total=\([0-9.]*\) ms.*/\1/p' <<<"$line")
        o=$(sed -n 's/.*open=\([0-9.]*\) ms.*/\1/p' <<<"$line")
        s=$(sed -n 's/.*source=\([0-9.]*\) ms.*/\1/p' <<<"$line")
        op=$(sed -n 's/.*operators=\([0-9.]*\) ms.*/\1/p' <<<"$line")
        rw=$(sed -n 's/.*rows=\([0-9]*\).*/\1/p' <<<"$line")
        echo "pgcolumnar,$lo,$hi,$i,$t,$o,$s,$op,$rw,n/a,,,"
    done
done

echo
echo "############ done ############"
