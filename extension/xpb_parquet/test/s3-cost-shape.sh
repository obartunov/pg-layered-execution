#!/bin/bash
# Phase 7: the SHAPE of the cost, not a benchmark campaign.
set -uo pipefail
PSQ=(/home/claude/pginstall20/bin/psql -h /tmp/xpb_sock -p 5420 -U pguser -d testdb -X -qAt)
S3=s3://xpb/reg_buh.parquet
LO=/home/claude/pg-layered-execution/benchmarks/02-batch-joins/data/reg_buh.parquet
ALL=period_key,company_key,account_key,amount_dt

med() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{printf "%.1f", a[int((NR+1)/2)]}'; }

shape() {   # shape <label> <uri> <args>
  local lab="$1" uri="$2" args="$3" i t0 t1 w=() dm=() last
  for i in 1 2 3; do "${PSQ[@]}" -c "LOAD 'xpb_parquet'; SELECT rows FROM xpq_scan('$uri',$args)" >/dev/null 2>&1; done
  for i in 1 2 3 4 5; do
    t0=$(date +%s%N)
    last=$("${PSQ[@]}" -c "LOAD 'xpb_parquet'; SELECT decode_ms||' '||s3_get_calls||' '||data_bytes||' '||s3_bytes_transferred||' '||meta_bytes FROM xpq_scan('$uri',$args)" 2>&1 | tail -1)
    t1=$(date +%s%N)
    w+=( $(( (t1-t0)/1000000 )) )
    dm+=( "$(echo $last | awk '{printf "%.0f", $1}')" )
  done
  set -- $last
  printf '%-26s %-6s wall %7s ms   decode %6s ms   GET %5s   data %10s B   transferred %10s B   meta %8s B\n' \
    "$lab" "$(basename $uri | cut -c1-5)" "$(med "${w[@]}")" "$(med "${dm[@]}")" "${2:--}" "${3:--}" "${4:--}" "${5:--}"
}

echo "=== cost shape: same queries, two transports (5 timed reps, median) ==="
shape "footer only (pruned to 0)" "$S3" "'$ALL',9999,9999"
shape "footer only (pruned to 0)" "$LO" "'$ALL',9999,9999"
shape "pruned, 1 col"             "$S3" "'period_key',25,36"
shape "pruned, 1 col"             "$LO" "'period_key',25,36"
shape "pruned, 4 cols"            "$S3" "'$ALL',25,36"
shape "pruned, 4 cols"            "$LO" "'$ALL',25,36"
shape "full scan, 4 cols"         "$S3" "'$ALL',1,120"
shape "full scan, 4 cols"         "$LO" "'$ALL',1,120"

echo
echo "=== XPBatch execution on top of the same source ==="
agg() {
  local lab="$1" uri="$2" i t0 t1 w=()
  for i in 1 2; do "${PSQ[@]}" -c "LOAD 'xpb_parquet'; SELECT sum(total_amt) FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$uri')" >/dev/null 2>&1; done
  for i in 1 2 3 4 5; do
    t0=$(date +%s%N)
    "${PSQ[@]}" -c "LOAD 'xpb_parquet'; SELECT sum(total_amt) FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$uri')" >/dev/null 2>&1
    t1=$(date +%s%N); w+=( $(( (t1-t0)/1000000 )) )
  done
  printf '%-26s %-6s wall %7s ms\n' "$lab" "$(basename $uri | cut -c1-5)" "$(med "${w[@]}")"
}
agg "join2+groupby [25..36]" "$S3"
agg "join2+groupby [25..36]" "$LO"
