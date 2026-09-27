#!/bin/bash
#
# Dimension Hash Growth v1
#
#   benchmarks/05-1c-like-v2/run-dim-growth.sh <PGPORT> [PGHOST] [DBNAME]
#
# Housekeeping, not a research milestone. reg2_card2 reached 147 456 groups only
# by holding both dimension hashes at their 3/4 load limit -- 192 of 256 slots
# and 768 of 1024 -- so skew measured against it could not attribute a
# regression to the group hash or to a dimension hash. This gives the dimension
# tables the same growth policy the group hash already has and measures that
# the old saturation regime is gone.
#
# What is held still, so that only dimension cardinality varies:
#
#   group cardinality   12 288 at every point, a cardinality the growing group
#                       hash is already measured to handle normally
#   input rows          1 032 192 at every point, every key present
#   source path         heap fixed-offset throughout
#
# Preregistered policy, same as the group hash and not tuned from these numbers:
# grow at load 0.5, double, rehash, no spill.
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

# n_comp n_acct n_grp : group cardinality is n_comp * n_grp, pinned at 12 288
LADDER=("64 256 192" "128 512 96" "129 513 95" "192 768 64" "384 1536 32" "768 3072 16")

echo "############ Dimension Hash Growth v1 ############"
echo
echo "=== policy in force (production, never the test hook) ==="
"${PSQL[@]}" -c "$GUC SELECT count(*) FROM xpb_v2_register_report(1,12,'dimgrow')" 2>&1 \
    | grep -v '^WARNING' \
    | sed -n 's/.*\(dim1_initial_cap=[0-9]*\).*\(dim2_initial_cap=[0-9]*\).*/  \1 \2, grow at half, double/p'

echo
echo "############ correctness gate ############"
echo "Per-group equality against PostgreSQL at every point. A rehash that lost a"
echo "dimension entry would drop every fact row referencing that key -- a wrong"
echo "join result, not an error -- so grand totals are not accepted."
echo
{
echo "$GUC"
echo "CREATE TEMP TABLE ck(pt int, arm text, ck text, groups bigint, rows_in bigint, ord int);"
ord=0
i=0
for pt in "${LADDER[@]}"; do
    set -- $pt
    i=$((i+1))
    echo "SELECT xpe_set_dim_cardinality($1,$2,$3);"
    ord=$((ord+1))
    echo "INSERT INTO ck SELECT $i, 'batch', $CK, count(*), sum(1), $ord
          FROM xpb_v2_register_report(1, 12, 'dimgrow');"
    ord=$((ord+1))
    echo "INSERT INTO ck SELECT $i, 'sql', $CK, count(*), sum(1), $ord FROM (
              SELECT c.company_group, a.account_group, r.company_key,
                     sum(r.debit_cents)::bigint  AS debit_turnover,
                     sum(r.credit_cents)::bigint AS credit_turnover,
                     sum(r.debit_cents - r.credit_cents)::bigint AS net_turnover
              FROM reg2_dim r
              JOIN dim_company_d c ON c.company_key = r.company_key
              JOIN dim_account_d a ON a.account_key = r.account_key
              WHERE r.period BETWEEN 1 AND 12
              GROUP BY 1,2,3) s;"
done
cat <<'SQL'
SELECT 'point ' || pt || '  ' || rpad(arm,6) || ck || '  groups=' || groups
FROM ck ORDER BY ord;

DO $$
DECLARE r record;
BEGIN
    FOR r IN SELECT pt, count(DISTINCT ck) nck, count(DISTINCT groups) ng
             FROM ck GROUP BY pt LOOP
        IF r.nck <> 1 THEN RAISE EXCEPTION 'PER-GROUP CHECKSUM MISMATCH at point %', r.pt; END IF;
        IF r.ng  <> 1 THEN RAISE EXCEPTION 'GROUP COUNT MISMATCH at point %', r.pt; END IF;
    END LOOP;
    RAISE NOTICE 'dim growth gate PASS: batch and PostgreSQL agree per group at every dimension cardinality';
END $$;
SQL
} | "${PSQL[@]}"

echo
echo "############ cardinality verification, per point ############"
for pt in "${LADDER[@]}"; do
    set -- $pt
    echo "-- n_comp=$1 n_acct=$2 n_grp=$3 --"
    echo "   $("${PSQL[@]}" -c "SELECT xpe_set_dim_cardinality($1,$2,$3)")"
done

echo
echo "############ timings: 1 warm-up + $RUNS measured runs ############"
echo "# n_comp,n_acct,n_grp,run,groups,rows,total_ms,source_ms,dim1_build_ms,dim2_build_ms,join1_ms,join2_ms,agg_ms,operators_ms,d1_cap,d1_entries,d1_load,d1_growths,d1_rehash_entries,d1_rehash_ms,d1_probes_per_lookup,d1_probe_current,d1_probe_lifetime,d1_bytes,d1_bytes_peak,d1_cxt_bytes,d2_cap,d2_entries,d2_load,d2_growths,d2_rehash_entries,d2_rehash_ms,d2_probes_per_lookup,d2_probe_current,d2_probe_lifetime,d2_bytes,d2_bytes_peak,d2_cxt_bytes,status"

for pt in "${LADDER[@]}"; do
    set -- $pt
    "${PSQL[@]}" -c "SELECT xpe_set_dim_cardinality($1,$2,$3)" >/dev/null
    for i in $(seq 0 $RUNS); do
        line=$("${PSQL[@]}" -c "$GUC
                 SELECT count(*) FROM xpb_v2_register_report(1,12,'dimgrow')" 2>&1 \
               | grep -v '^WARNING' || true)
        if grep -q '^ERROR' <<<"$line"; then
            [ "$i" -eq 0 ] && continue
            echo "$1,$2,$3,$i,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,ERROR"
            continue
        fi
        line=$(sed -n 's/^NOTICE:  v2_register_report //p' <<<"$line")
        [ "$i" -eq 0 ] && continue
        g() { sed -n "s/.*[ =]$1=\([0-9.]*\).*/\1/p" <<<"$line"; }
        echo "$1,$2,$3,$i,$(g groups),$(g rows),$(g total),$(g source),$(g dim1_build_ms),$(g dim2_build_ms),$(g join1),$(g join2),$(g agg),$(g operators),$(g dim1_cap),$(g dim1_entries),$(g dim1_load_factor),$(g dim1_growths),$(g dim1_rehash_entries),$(g dim1_rehash_ms),$(g dim1_probes_per_lookup_lifetime),$(g dim1_max_probe_current),$(g dim1_max_probe_lifetime),$(g dim1_bytes),$(g dim1_bytes_peak),$(g dim1_cxt_bytes),$(g dim2_cap),$(g dim2_entries),$(g dim2_load_factor),$(g dim2_growths),$(g dim2_rehash_entries),$(g dim2_rehash_ms),$(g dim2_probes_per_lookup_lifetime),$(g dim2_max_probe_current),$(g dim2_max_probe_lifetime),$(g dim2_bytes),$(g dim2_bytes_peak),$(g dim2_cxt_bytes),OK"
    done
done

echo
echo "############ paired pre-sized control ############"
echo "Widest point, 768 x 3072. Both arms end with the same entries, the same"
echo "final capacities and the same result, and differ only in whether they"
echo "doubled their way there. Alternating inside one connection, so the"
echo "comparison is paired rather than across sessions."
echo
echo "# arm,pair,d1_cap,d2_cap,d1_growths,d2_growths,dim1_build_ms,dim2_build_ms,d1_rehash_ms,d2_rehash_ms,total_ms,d1_probe_current,d1_probe_lifetime"
"${PSQL[@]}" -c "SELECT xpe_set_dim_cardinality(768,3072,16)" >/dev/null
# Pre-sizing uses the test hook. It is diagnostic only: the real path always
# grows from its normal initial capacity (section 26).
{
echo "$GUC"
for w in 0 -1; do
    echo "SELECT xpb_dim_test_policy(0,0); SELECT count(*) FROM xpb_v2_register_report(1,12,'dimgrow');"
    echo "SELECT xpb_dim_test_policy(2048,8192); SELECT count(*) FROM xpb_v2_register_report(1,12,'dimgrow');"
done
for i in $(seq 1 10); do
    echo "SELECT xpb_dim_test_policy(0,0); SELECT count(*) FROM xpb_v2_register_report(1,12,'dimgrow');"
    echo "SELECT xpb_dim_test_policy(2048,8192); SELECT count(*) FROM xpb_v2_register_report(1,12,'dimgrow');"
done
} | "${PSQL[@]}" 2>&1 | grep -v '^WARNING' | sed -n 's/^NOTICE:  v2_register_report //p' \
  | awk -v OFS=, '
    {
      match($0, /dim1_initial_cap=[0-9]+/); ic = substr($0, RSTART+17, RLENGTH-17)
      arm = (ic == "256" ? "natural" : "presized")
      n[arm]++
      if (n[arm] > 2) {
        f["d1c"]="dim1_cap"; f["d2c"]="dim2_cap"; f["d1g"]="dim1_growths"; f["d2g"]="dim2_growths"
        f["d1b"]="dim1_build_ms"; f["d2b"]="dim2_build_ms"; f["d1r"]="dim1_rehash_ms"; f["d2r"]="dim2_rehash_ms"
        f["pc"]="dim1_max_probe_current"; f["pl"]="dim1_max_probe_lifetime"
        for (k in f) { match($0, f[k] "=[0-9.]+"); v[k] = substr($0, RSTART+length(f[k])+1, RLENGTH-length(f[k])-1) }
        match($0, /total=[0-9.]+/); tot = substr($0, RSTART+6, RLENGTH-6)
        print arm, n[arm]-2, v["d1c"], v["d2c"], v["d1g"], v["d2g"], v["d1b"], v["d2b"], v["d1r"], v["d2r"], tot, v["pc"], v["pl"]
      }
    }'

echo
echo "############ done ############"
