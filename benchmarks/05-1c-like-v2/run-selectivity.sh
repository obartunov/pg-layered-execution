#!/bin/bash
#
# Benchmark 05-A — Selectivity Crossover
#
#   benchmarks/05-1c-like-v2/run-selectivity.sh <PGPORT> [PGHOST] [DBNAME]
#
# The question: at what predicate selectivity does pgColumnar + xp_batch
# recover the cost of columnar decode through row-group pruning, relative to
# heap + xp_batch?
#
# One fixed report throughout. Two joins, three aggregates, int8 money. Only
# the period predicate changes between points, so what moves is the source.
#
# THE HEAP ARM RUNS ON THE GENERIC DEFORM PATH. The v2 row has a varlena at
# attnum 2, ahead of every column the pipeline reads, so the fixed-offset path
# cannot address it. Its source timing is therefore NOT comparable with
# benchmark 04's, which measured a different mechanism on a narrower row.
#
# ZLFS is a pre-materialized zone for a range, so a zone is built per range.
# Build time is measured and reported separately; it is not query latency.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-onec2}"
RUNS=5

PSQL=(psql -p "$PORT" -d "$DB" -v ON_ERROR_STOP=1 -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

# GUCs, set explicitly rather than trusted from the cluster
GUC_COMMON="SET work_mem = '64MB'; SET enable_hashjoin = on; SET jit = off;"
GUC_SINGLE="$GUC_COMMON SET max_parallel_workers_per_gather = 0;"
GUC_PARALLEL="$GUC_COMMON SET max_parallel_workers_per_gather = 2;"

# The vanilla form of the same report. %s is the fact table.
sql_body() {
    cat <<SQL
SELECT c.company_group, a.account_group, r.company_key,
       sum(r.debit_cents)::bigint  AS debit_turnover,
       sum(r.credit_cents)::bigint AS credit_turnover,
       sum(r.debit_cents - r.credit_cents)::bigint AS net_turnover
FROM $1 r
JOIN dim_company  c ON c.company_key  = r.company_key
JOIN dim_account2 a ON a.account_key  = r.account_key
WHERE r.period BETWEEN $2 AND $3
GROUP BY 1, 2, 3
SQL
}

# Canonical checksum: every grouping key and every aggregate, sorted before
# hashing so output order cannot enter it. Timing columns are excluded.
CK="md5(coalesce(string_agg(company_group||','||account_group||','||company_key||','||
                            debit_turnover||','||credit_turnover||','||net_turnover,
                            '|' ORDER BY company_group, account_group, company_key), ''))"

echo "############ Benchmark 05-A — selectivity crossover ############"
echo
echo "=== versions ==="
"${PSQL[@]}" -c "SELECT 'server: ' || version()" | cut -c1-60
"${PSQL[@]}" -c "SELECT 'pgcolumnar: ' || extversion FROM pg_extension WHERE extname='pgcolumnar'"

echo
echo "=== physical layout: row groups and pruning opportunity ==="
"${PSQL[@]}" -f "$HERE/inspect-rowgroups.sql"

echo
echo "=== physical sizes ==="
"${PSQL[@]}" -c "
SELECT rpad(c.relname,12) || ' ' || lpad(pg_size_pretty(pg_total_relation_size(c.oid)),10)
       || '  ' || lpad(round(pg_total_relation_size(c.oid)::numeric / r.n, 1)::text,7) || ' bytes/row'
FROM pg_class c, (SELECT count(*)::numeric n FROM reg2) r
WHERE c.relname IN ('reg2','reg2_col') ORDER BY 1"

echo
echo "=== ZLFS zone build (measured separately; NOT query latency) ==="
for range in "1 1" "1 3" "1 6" "1 12"; do
    set -- $range
    "${PSQL[@]}" -c "SELECT zlfs_drop_zone($1, $2)" >/dev/null 2>&1 || true
done
for range in "1 12"; do
    set -- $range
    "${PSQL[@]}" -c "SELECT zlfs_build_zone('reg2','1,3,4,8,9',$1,$2)" 2>&1 \
        | sed -n 's/^NOTICE:  //p'
done

echo
echo "############ correctness gate ############"
echo "Runs before any timing. Timing is not taken if it fails."
echo

{
echo "$GUC_SINGLE"
cat <<'SQL'
CREATE TEMP TABLE ck(range text, path text, ck text, groups bigint,
                     dsum bigint, csum bigint, nsum bigint, ord int);
SQL
ord=0
for range in "1 1" "1 3" "1 6" "1 12" "90 91" "12 12"; do
    set -- $range; lo=$1; hi=$2; tag="$lo..$hi"
    for spec in "vanilla_heap:$(sql_body reg2 $lo $hi)" \
                "vanilla_col:$(sql_body reg2_col $lo $hi)"; do
        ord=$((ord+1))
        name="${spec%%:*}"; body="${spec#*:}"
        echo "INSERT INTO ck SELECT '$tag', '$name', $CK, count(*),
              sum(debit_turnover), sum(credit_turnover), sum(net_turnover), $ord
              FROM ($body) s;"
    done
    for mode in heap pgcolumnar; do
        ord=$((ord+1))
        echo "INSERT INTO ck SELECT '$tag', 'xpb_$mode', $CK, count(*),
              sum(debit_turnover), sum(credit_turnover), sum(net_turnover), $ord
              FROM xpb_v2_register_report($lo, $hi, '$mode');"
    done
    # repeats in the same backend, to catch state carried between calls
    for mode in pgcolumnar; do
        ord=$((ord+1))
        echo "INSERT INTO ck SELECT '$tag', 'xpb_${mode}_again', $CK, count(*),
              sum(debit_turnover), sum(credit_turnover), sum(net_turnover), $ord
              FROM xpb_v2_register_report($lo, $hi, '$mode');"
    done
done
cat <<'SQL'
SELECT rpad(range, 7) || rpad(path, 20) || ck || '  groups=' || groups
FROM ck ORDER BY ord;

DO $$
DECLARE r record; n int;
BEGIN
    FOR r IN SELECT range, count(DISTINCT ck) AS nck, count(DISTINCT groups) AS ng
             FROM ck GROUP BY range LOOP
        IF r.nck <> 1 THEN
            RAISE EXCEPTION 'CHECKSUM MISMATCH across paths for range %', r.range;
        END IF;
        IF r.ng <> 1 THEN
            RAISE EXCEPTION 'GROUP COUNT MISMATCH for range %', r.range;
        END IF;
    END LOOP;
    SELECT count(*) INTO n FROM ck WHERE nsum <> dsum - csum;
    IF n <> 0 THEN
        RAISE EXCEPTION 'sum(debit)-sum(credit) <> sum(debit-credit) in % rows', n;
    END IF;
    RAISE NOTICE 'gate PASS: one checksum per range, one group count, net identity holds';
END $$;
SQL
} | "${PSQL[@]}"

echo
echo "=== per-group net identity, full range ==="
"${PSQL[@]}" -c "
SELECT 'groups where net <> debit - credit: ' || count(*)
FROM xpb_v2_register_report(1, 12, 'heap')
WHERE net_turnover <> debit_turnover - credit_turnover" 2>/dev/null

echo
echo "=== ZLFS arm, checked against the same result (its zone is one range) ==="
"${PSQL[@]}" <<SQL 2>&1 | grep -v '^NOTICE:  v2_register' | sed -n 's/^NOTICE:  //p;/^[0-9a-f]\{32\}/p'
$GUC_SINGLE
SELECT $CK || '  groups=' || count(*) FROM xpb_v2_register_report(1, 12, 'zlfs');
SELECT $CK || '  groups=' || count(*) FROM xpb_v2_register_report(1, 12, 'zlfs');
SELECT $CK || '  groups=' || count(*) FROM ($(sql_body reg2 1 12)) s;
SQL

echo
echo "############ plans ############"
mkdir -p "$HERE/plans"
for tbl in reg2 reg2_col; do
    for range in "1 1" "1 12"; do
        set -- $range
        for cfg in single parallel; do
            g=$GUC_SINGLE; [ "$cfg" = parallel ] && g=$GUC_PARALLEL
            f="$HERE/plans/${tbl}-${1}_${2}-vanilla_${cfg}.txt"
            "${PSQL[@]}" > "$f" 2>&1 <<SQL
$g
EXPLAIN (ANALYZE, BUFFERS, SETTINGS, COSTS OFF)
$(sql_body $tbl $1 $2);
SQL
            echo "  $(basename "$f"): $(grep -cE 'Scan|Join|Aggregate' "$f") plan nodes"
        done
    done
done

echo
echo "############ timings: 1 warm-up + $RUNS measured runs ############"
echo "# path,lo,hi,run,total_ms,open_ms,source_ms,join1_ms,join2_ms,agg_ms,operators_ms,rows,rowgroups_read,rows_in_read_groups,rows_emitted"

# --- xp_batch paths: phases come from the code, not from a total ---
for range in "1 1" "1 3" "1 6" "1 12"; do
    set -- $range; lo=$1; hi=$2

    # ZLFS zone for exactly this range; build cost reported, not timed as query
    "${PSQL[@]}" -c "SELECT zlfs_drop_zone($lo,$hi)" >/dev/null 2>&1 || true
    zb=$("${PSQL[@]}" -c "SELECT zlfs_build_zone('reg2','1,3,4,8,9',$lo,$hi)" 2>&1 \
         | sed -n 's/.*built: \([0-9]*\) rows, \([0-9]*\) ms, \([0-9]*\) KB.*/\1 rows \2 ms \3 KB/p')
    echo "# zlfs zone [$lo..$hi] build: $zb"

    for mode in heap pgcolumnar zlfs; do
        for i in $(seq 0 $RUNS); do      # run 0 is the warm-up, not recorded
            line=$("${PSQL[@]}" -c "$GUC_SINGLE
                      SELECT count(*) FROM xpb_v2_register_report($lo,$hi,'$mode')" 2>&1 \
                   | sed -n 's/^NOTICE:  v2_register_report //p')
            [ "$i" -eq 0 ] && continue
            t=$(sed -n 's/.*total=\([0-9.]*\) ms.*/\1/p' <<<"$line")
            o=$(sed -n 's/.*open=\([0-9.]*\) ms.*/\1/p' <<<"$line")
            s=$(sed -n 's/.*source=\([0-9.]*\) ms.*/\1/p' <<<"$line")
            j1=$(sed -n 's/.*join1=\([0-9.]*\) ms.*/\1/p' <<<"$line")
            j2=$(sed -n 's/.*join2=\([0-9.]*\) ms.*/\1/p' <<<"$line")
            a=$(sed -n 's/.*agg=\([0-9.]*\) ms.*/\1/p' <<<"$line")
            op=$(sed -n 's/.*operators=\([0-9.]*\) ms.*/\1/p' <<<"$line")
            rw=$(sed -n 's/.*rows=\([0-9]*\).*/\1/p' <<<"$line")
            rg=$(sed -n 's/.*rowgroups_read=\([0-9]*\).*/\1/p' <<<"$line"); rg=${rg:-}
            rr=$(sed -n 's/.*rows_in_read_groups=\([0-9]*\).*/\1/p' <<<"$line"); rr=${rr:-}
            re=$(sed -n 's/.*rows_emitted=\([0-9]*\).*/\1/p' <<<"$line"); re=${re:-}
            echo "xpb_$mode,$lo,$hi,$i,$t,$o,$s,$j1,$j2,$a,$op,$rw,$rg,$rr,$re"
        done
    done
done

# --- vanilla SQL: whole-query timing only. No phase breakdown is invented. ---
for range in "1 1" "1 3" "1 6" "1 12"; do
    set -- $range; lo=$1; hi=$2
    for tbl in reg2 reg2_col; do
        for cfg in single parallel; do
            g=$GUC_SINGLE; [ "$cfg" = parallel ] && g=$GUC_PARALLEL
            name="vanilla_${cfg}_$([ "$tbl" = reg2 ] && echo heap || echo col)"
            for i in $(seq 0 $RUNS); do
                ms=$("${PSQL[@]}" <<SQL 2>&1 | sed -n 's/^Time: \([0-9.]*\) ms.*/\1/p'
$g
\\timing on
SELECT count(*) FROM ($(sql_body $tbl $lo $hi)) s;
SQL
)
                [ "$i" -eq 0 ] && continue
                echo "$name,$lo,$hi,$i,$ms,,,,,,,,,,"
            done
        done
    done
done

echo
echo "############ done ############"
